// NpcTouch, step 2: log every time the player's hands touch an NPC.
// This build also logs non-NPC touches to diagnose what ROCK reports.
//
// ROCK owns hand physics. This plugin only reads ROCK's collision events and
// contact snapshots, resolves the touched reference, and reports NPC touches
// to the log and to a short in-headset debug overlay.

#include <ROCK/Collision.h>
#include <ROCK/Diagnostics.h>
#include <ROCK/Hands.h>
#include <ROCK/Input.h>
#include "NpcCollision.h"
#include "NpcProxyBodies.h"
#include "Reactions.h"
#include "Runtime.h"

#include <RE/Fallout.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using npctouch::runtime::hasLifecycleFlag;
    namespace collision = rock::api::collision;
    namespace diagnostics = rock::api::diagnostics;
    namespace hands = rock::api::hands;
    namespace input = rock::api::input;

    constexpr auto kOverlayDuration = std::chrono::seconds(3);
    constexpr auto kSummaryInterval = std::chrono::seconds(10);

    const collision::ApiV1* g_collision{};
    const diagnostics::ApiV1* g_diagnostics{};
    const hands::ApiV1* g_hands{};
    const input::ApiV1* g_input{};

    std::uint64_t g_lastEventSequence{ 0 };
    std::uint64_t g_npcTouchCount{ 0 };
    std::uint64_t g_otherTouchCount{ 0 };
    std::uint64_t g_contactBeginCount{ 0 };

    // Last status of each ROCK read, reported in the summary for diagnosis.
    struct Diagnostics
    {
        std::uint32_t eventsStatus{ 0xFFFFFFFF };
        std::uint32_t contactsStatus{ 0xFFFFFFFF };
        std::uint32_t contactsSeen{ 0 };
        struct Hand
        {
            std::uint32_t status{ 0xFFFFFFFF };
            std::uint32_t flags{ 0 };
            float position[3]{};
            // Closest approach to any NPC since the last summary.
            float minGap{ -1.0f };
            char minBone[32]{};
            char minName[48]{};
        } hand[2]{};
        std::uint32_t nearbyActors{ 0 };
        float actorPosition[3]{};
        float bonePosition[3]{};
        bool usedCapsule{ false };
    } g_diag{};
    std::uint64_t g_lostEventCount{ 0 };
    Clock::time_point g_lastSummary{};

    char g_overlayText[128]{};
    Clock::time_point g_overlayUntil{};

    const char* handName(const rock::api::Hand hand) noexcept
    {
        switch (hand) {
        case rock::api::Hand::Right:
            return "Right";
        case rock::api::Hand::Left:
            return "Left";
        default:
            return "None";
        }
    }

    // Returns the touched NPC, or nullptr for objects, the player and unknown forms.
    RE::Actor* resolveNpc(const std::uint32_t formId) noexcept
    {
        if (formId == 0) {
            return nullptr;
        }
        auto* actor = RE::TESForm::GetFormByID<RE::Actor>(formId);
        if (!actor || actor->IsPlayerRef()) {
            return nullptr;
        }
        return actor;
    }

    const char* actorName(RE::Actor* actor) noexcept
    {
        const char* name = actor ? actor->GetDisplayFullName() : nullptr;
        return name && name[0] ? name : "<no name>";
    }

    // Diagnostic description of any touched form: "[NPC_] 'Piper' 0001A4D7".
    void describeForm(const std::uint32_t formId, char* out, const std::size_t size) noexcept
    {
        auto* form = formId ? RE::TESForm::GetFormByID(formId) : nullptr;
        if (!form) {
            std::snprintf(out, size, "form=%08X (unresolved)", formId);
            return;
        }
        auto* ref = form->As<RE::TESObjectREFR>();
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        const char* name = ref ? ref->GetDisplayFullName() : nullptr;
        std::snprintf(
            out,
            size,
            "form=%08X type=%s base=%s '%s'",
            formId,
            form->GetFormTypeString(),
            base ? base->GetFormTypeString() : "-",
            name && name[0] ? name : "");
    }

    // Caps diagnostic lines so touching a wall cannot flood the log.
    bool allowDiagnosticLine() noexcept
    {
        static Clock::time_point windowStart{};
        static int linesInWindow{ 0 };
        const auto now = Clock::now();
        if (now - windowStart >= std::chrono::seconds(1)) {
            windowStart = now;
            linesInWindow = 0;
        }
        return ++linesInWindow <= 20;
    }

    // ROCK's debug overlay only draws when ROCK's own debug view is on, so
    // also post the text as a regular game notification in the corner.
    void showOverlay(const char* text) noexcept
    {
        std::snprintf(g_overlayText, sizeof(g_overlayText), "%s", text);
        g_overlayUntil = Clock::now() + kOverlayDuration;
        RE::SendHUDMessage::ShowHUDMessage(g_overlayText, nullptr, false, false);
    }

    void handleTouchEvent(const collision::EventV1& event) noexcept
    {
        const bool begin = event.kind == static_cast<std::uint32_t>(collision::EventKindV1::Touch);
        auto* npc = resolveNpc(event.formId);
        if (!npc) {
            if (begin) {
                ++g_otherTouchCount;
            }
            if (allowDiagnosticLine()) {
                char described[192]{};
                describeForm(event.formId, described, sizeof(described));
                char message[288]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "%s (not NPC): %s hand -> %s layer=%u",
                    begin ? "touch begin" : "touch end",
                    handName(event.hand),
                    described,
                    event.collisionLayer);
                npctouch::runtime::logInfo(message);
                if (begin) {
                    char overlay[128]{};
                    std::snprintf(overlay, sizeof(overlay), "NpcTouch: %s hand -> object layer %u", handName(event.hand), event.collisionLayer);
                    showOverlay(overlay);
                }
            }
            return;
        }

        char message[256]{};
        std::snprintf(
            message,
            sizeof(message),
            "%s: %s hand -> NPC '%s' form=%08X layer=%u frame=%llu",
            begin ? "TOUCH BEGIN" : "TOUCH END",
            handName(event.hand),
            actorName(npc),
            event.formId,
            event.collisionLayer,
            static_cast<unsigned long long>(event.frameIndex));
        npctouch::runtime::logInfo(message);

        if (begin) {
            ++g_npcTouchCount;
            char overlay[128]{};
            std::snprintf(overlay, sizeof(overlay), "NpcTouch: %s hand -> %s", handName(event.hand), actorName(npc));
            showOverlay(overlay);
        }
    }

    void pollTouchEvents(const std::uint64_t ownerToken) noexcept
    {
        // Drain a bounded number of batches so a burst cannot stall the frame.
        for (int batch = 0; batch < 4; ++batch) {
            std::array<collision::EventV1, 32> events{};
            rock::api::StreamV1 stream{};
            const auto status = g_collision->copyEvents(
                ownerToken,
                g_lastEventSequence,
                events.data(),
                static_cast<std::uint32_t>(events.size()),
                &stream);
            g_diag.eventsStatus = static_cast<std::uint32_t>(status);
            if (status != rock::api::Status::Ok) {
                return;
            }
            if (stream.lostCount > g_lostEventCount) {
                char message[128]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "ROCK dropped %llu collision events before they were read",
                    static_cast<unsigned long long>(stream.lostCount - g_lostEventCount));
                npctouch::runtime::logWarning(message);
                g_lostEventCount = stream.lostCount;
            }
            for (std::uint32_t index = 0; index < stream.copiedCount; ++index) {
                handleTouchEvent(events[index]);
            }
            if (stream.copiedCount != 0) {
                g_lastEventSequence = stream.nextSequence;
            }
            if (stream.remainingCount == 0) {
                return;
            }
        }
    }

    // Finger-level detail: which part of the hand started touching which NPC.
    void pollFingerContacts(const std::uint64_t ownerToken, const rock::api::Hand hand) noexcept
    {
        std::array<collision::SemanticHandContactV1, 20> contacts{};
        std::uint32_t contactCount{ 0 };
        const auto status = g_collision->copySemanticHandContactsV1(
            ownerToken,
            hand,
            0,
            contacts.data(),
            static_cast<std::uint32_t>(contacts.size()),
            &contactCount);
        g_diag.contactsStatus = static_cast<std::uint32_t>(status);
        if (status != rock::api::Status::Ok) {
            return;
        }
        g_diag.contactsSeen += contactCount;

        for (std::uint32_t index = 0; index < contactCount && index < contacts.size(); ++index) {
            const auto& contact = contacts[index];
            if (contact.contactState != collision::SemanticContactStateV1::Begin) {
                continue;
            }
            ++g_contactBeginCount;
            auto* npc = resolveNpc(contact.targetFormId);
            if (!npc) {
                if (allowDiagnosticLine()) {
                    char described[192]{};
                    describeForm(contact.targetFormId, described, sizeof(described));
                    char message[288]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "  contact (not NPC): %s hand finger=%u role=%u targetBody=%u %s",
                        handName(hand),
                        contact.finger,
                        contact.role,
                        contact.targetBodyId,
                        described);
                    npctouch::runtime::logInfo(message);
                }
                continue;
            }
            char message[256]{};
            std::snprintf(
                message,
                sizeof(message),
                "  contact: %s hand finger=%u segment=%u role=%u -> '%s' form=%08X at (%.1f, %.1f, %.1f)",
                handName(hand),
                contact.finger,
                contact.segment,
                contact.role,
                actorName(npc),
                contact.targetFormId,
                contact.contactPointGame.x,
                contact.contactPointGame.y,
                contact.contactPointGame.z);
            npctouch::runtime::logInfo(message);
        }
    }


    // Fallback detector that does not depend on ROCK contact events: compare
    // each hand's position with a few bones of every nearby NPC.
    namespace proximity
    {
        struct Bone
        {
            const char* name;
            float radius;  // game units; roughly half the limb thickness plus palm
        };

        constexpr std::array kBones{
            Bone{ "Head", 13.0f },
            Bone{ "Neck", 9.0f },
            Bone{ "Chest", 15.0f },
            Bone{ "SPINE2", 14.0f },
            Bone{ "SPINE1", 14.0f },
            Bone{ "Pelvis", 14.0f },
            Bone{ "LArm_UpperArm", 8.0f },
            Bone{ "RArm_UpperArm", 8.0f },
            Bone{ "LArm_ForeArm1", 7.0f },
            Bone{ "RArm_ForeArm1", 7.0f },
            Bone{ "LArm_Hand", 7.0f },
            Bone{ "RArm_Hand", 7.0f },
        };

        constexpr float kSearchRadius = 250.0f;
        // ROCK's hand frame sits at the wrist; the palm and fingers reach further.
        constexpr float kHandReach = 6.0f;
        constexpr float kReleaseMargin = 4.0f;

        // A skin's bone transform slot that the grab moves by hand, kept at a
        // fixed offset from a node the grab turns.
        struct SkinLink
        {
            RE::NiTransform* slot{ nullptr };
            RE::NiAVObject* node{ nullptr };
            RE::NiMatrix3 rotate{};
            RE::NiPoint3 translate{};
        };

        struct HandState
        {
            bool valid{ false };
            RE::NiPoint3 position{};
            RE::NiPoint3 previous{};
            Clock::time_point previousTime{};
            float speed{ 0.0f };
            // Smoothed hand velocity in game units per second.
            float velocity[3]{};
            std::uint32_t touchedFormId{ 0 };
            const char* touchedBone{ nullptr };
            // Grabbing: grip held while touching an NPC drags them along.
            bool gripWasHeld{ false };
            std::uint32_t grabbedFormId{ 0 };
            const char* grabbedBone{ nullptr };
            RE::NiAVObject* grabRoot{ nullptr };
            RE::NiAVObject* grabJoint{ nullptr };
            RE::NiPoint3 grabLocal{};  // grabbed point in the joint's own frame
            float grabReach{ 0.0f };   // joint to grabbed point
            std::uint32_t posedFrames{ 0 };
            std::uint32_t keptFrames{ 0 };
            // For diagnosis: what we set last frame on the joint and one skin bone.
            RE::NiAVObject* skinBone{ nullptr };
            RE::NiTransform lastJointWorld{};
            RE::NiTransform lastSkinWorld{};
            // Names of the bones a grab turns; worn clothing can carry copies.
            std::vector<std::string> grabNames;
            std::vector<RE::NiAVObject*> grabTurned;
            std::uint32_t copiesTurned{ 0 };
            // Bones that worn skins bind to outside the skeleton, turned by hand.
            std::vector<RE::NiAVObject*> grabCopies;
            std::vector<SkinLink> skinLinks;
            // Every node of the grabbed actor, while the skin scan runs.
            std::vector<RE::NiAVObject*> scanNodes;
            // Arm grabs below the elbow also bend the elbow (two-bone reach).
            RE::NiAVObject* grabElbow{ nullptr };
            // A share of an arm grab turns the collarbone, a share of a head
            // grab the neck (the rest turns the head), so skin between them
            // stretches less.
            RE::NiAVObject* grabCollar{ nullptr };
            RE::NiAVObject* grabHead{ nullptr };
            // For the skin scan log: unskinned meshes near this point are listed.
            RE::NiPoint3 scanNear{};
            bool scanNearValid{ false };
            RE::NiPoint3 grabLocalFore{};   // grabbed point in the forearm's frame
            RE::NiPoint3 elbowAxisLocal{};  // elbow hinge in the upper arm's frame
            // Reaction to being held: last time the NPC was told.
            Clock::time_point lastHoldReaction{};
            Clock::time_point grabStart{};
            // After the grip opens the limb eases back to the animation.
            bool releasing{ false };
            Clock::time_point releaseStart{};
            RE::NiPoint3 releaseTarget{};
            std::uint32_t gripStatusLogged{ 0 };
        };

        // OpenVR button id of the grip.
        constexpr std::uint32_t kGripButton = 2;
        // The hand may be this far from the body (beyond a touch) to grab.
        constexpr float kGrabReach = 10.0f;
        // The grip slips when the hand gets this much further from the joint
        // than the grabbed point is.
        constexpr float kGrabBreak = 40.0f;
        // How long a released limb takes to ease back to the animation.
        constexpr float kReleaseSeconds = 0.35f;
        // Elbow bend limits (interior angle between upper arm and forearm).
        constexpr float kElbowMinAngle = 0.45f;
        constexpr float kElbowMaxAngle = 3.10f;
        bool g_bendElbow{ true };
        bool g_scanSkinsEnabled{ true };
        float g_collarShare{ 0.3f };
        float g_neckShare{ 0.5f };
        // Flinch settings (see the [Flinch] section of the ini).
        bool g_flinchEnabled{ true };
        float g_flinchFollow{ 0.7f };      // share of the hand's motion the body takes
        float g_flinchStiffness{ 9.0f };   // spring frequency back to the animation, rad/s
        float g_flinchDamping{ 0.55f };    // damping ratio of that spring
        float g_flinchMinSpeed{ 20.0f };   // slower hands only touch
        float g_flinchMaxTorso{ 0.25f };
        float g_flinchMaxHead{ 0.5f };
        float g_flinchMaxArm{ 0.7f };

        std::array<HandState, 2> g_handStates{};

        float distance(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        bool readHand(const std::uint64_t ownerToken, const rock::api::Hand hand, HandState& state) noexcept
        {
            hands::HandFrameV1 frame{};
            if (!g_hands) {
                return false;
            }
            const auto status = g_hands->getHandFrameV1(ownerToken, hand, &frame);
            auto& diag = g_diag.hand[hand == rock::api::Hand::Right ? 0 : 1];
            diag.status = static_cast<std::uint32_t>(status);
            diag.flags = frame.flags;
            for (int axis = 0; axis < 3; ++axis) {
                diag.position[axis] = frame.transform.translate[axis];
            }
            if (status != rock::api::Status::Ok) {
                return false;
            }
            if ((frame.flags & static_cast<std::uint32_t>(hands::HandFrameFlagV1::Valid)) == 0) {
                return false;
            }
            const auto now = Clock::now();
            const RE::NiPoint3 position{ frame.transform.translate[0], frame.transform.translate[1], frame.transform.translate[2] };
            if (state.valid) {
                const float seconds = std::chrono::duration<float>(now - state.previousTime).count();
                if (seconds > 0.0f) {
                    state.speed = distance(position, state.position) / seconds;
                    // Frame-to-frame motion is noisy at 90 fps; blend it over a few frames.
                    constexpr float kBlend = 0.4f;
                    const float delta[3]{ position.x - state.position.x, position.y - state.position.y, position.z - state.position.z };
                    for (int axis = 0; axis < 3; ++axis) {
                        state.velocity[axis] += (delta[axis] / seconds - state.velocity[axis]) * kBlend;
                    }
                }
            }
            state.previous = state.position;
            state.position = position;
            state.previousTime = now;
            state.valid = true;
            return true;
        }

        struct Hit
        {
            RE::Actor* actor{ nullptr };
            const char* bone{ nullptr };
            float gap{ 0.0f };  // distance minus bone radius; negative means inside
        };

        // Walks the scene graph by name. Avoids the virtual GetObjectByName,
        // whose result had an all-zero world transform in Fallout 4 VR.
        RE::NiAVObject* findNode(RE::NiAVObject* object, const char* name, const int depth = 0) noexcept
        {
            if (!object || depth > 64) {
                return nullptr;
            }
            const auto objectName = object->GetName();
            if (objectName.size() == std::strlen(name) && _strnicmp(objectName.data(), name, objectName.size()) == 0) {
                return object;
            }
            auto* node = object->IsNode();
            if (!node) {
                return nullptr;
            }
            auto& children = node->GetRuntimeData().children;
            for (std::uint16_t index = 0; index < children.size(); ++index) {
                if (auto* found = findNode(children[index].get(), name, depth + 1)) {
                    return found;
                }
            }
            return nullptr;
        }

        bool isUsablePosition(const RE::NiPoint3& point) noexcept
        {
            return std::abs(point.x) + std::abs(point.y) + std::abs(point.z) > 1.0f;
        }

        // Rough body used when no bone has a usable position: a vertical
        // segment from the NPC's feet to head height.
        constexpr float kCapsuleHeight = 120.0f;
        constexpr float kCapsuleRadius = 18.0f;

        float capsuleGap(const RE::NiPoint3& hand, const RE::NiPoint3& feet) noexcept
        {
            const float z = std::clamp(hand.z, feet.z + 10.0f, feet.z + kCapsuleHeight);
            return distance(hand, RE::NiPoint3{ feet.x, feet.y, z }) - kCapsuleRadius;
        }

        Hit nearestBone(const RE::NiPoint3& hand, RE::Actor* player) noexcept
        {
            Hit best{};
            auto* lists = RE::ProcessLists::GetSingleton();
            if (!lists || !player) {
                return best;
            }
            for (const auto& handle : lists->highActorHandles) {
                auto actorPtr = handle.get();
                auto* actor = actorPtr.get();
                if (!actor || actor == player || distance(actor->GetPosition(), player->GetPosition()) > kSearchRadius) {
                    continue;
                }
                ++g_diag.nearbyActors;
                const auto feet = actor->GetPosition();
                bool anyBone = false;
                if (auto* root = actor->Get3D()) {
                    for (const auto& bone : kBones) {
                        auto* node = findNode(root, bone.name);
                        if (!node || !isUsablePosition(node->world.translate)) {
                            continue;
                        }
                        anyBone = true;
                        const auto& point = node->world.translate;
                        const float gap = distance(hand, point) - bone.radius;
                        if (!best.actor || gap < best.gap) {
                            best = Hit{ actor, bone.name, gap };
                            g_diag.bonePosition[0] = point.x;
                            g_diag.bonePosition[1] = point.y;
                            g_diag.bonePosition[2] = point.z;
                            g_diag.actorPosition[0] = feet.x;
                            g_diag.actorPosition[1] = feet.y;
                            g_diag.actorPosition[2] = feet.z;
                            g_diag.usedCapsule = false;
                        }
                    }
                }
                if (!anyBone) {
                    const float gap = capsuleGap(hand, feet);
                    if (!best.actor || gap < best.gap) {
                        best = Hit{ actor, "Body", gap };
                        g_diag.actorPosition[0] = feet.x;
                        g_diag.actorPosition[1] = feet.y;
                        g_diag.actorPosition[2] = feet.z;
                        g_diag.usedCapsule = true;
                    }
                }
            }
            return best;
        }

        bool gripHeld(const std::uint64_t ownerToken, const rock::api::Hand hand, HandState& state) noexcept
        {
            if (!g_input) {
                return false;
            }
            input::RawWandButtonStateV1 button{};
            const auto status = g_input->getRawWandButtonStateV1(ownerToken, hand, kGripButton, &button);
            if (state.gripStatusLogged < 2 && (status != rock::api::Status::Ok || button.held)) {
                ++state.gripStatusLogged;
                char message[160]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "Grip %s hand: status=%u available=%u held=%u reason=%u",
                    handName(hand),
                    static_cast<std::uint32_t>(status),
                    button.available,
                    button.held,
                    static_cast<std::uint32_t>(button.availabilityReason));
                npctouch::runtime::logInfo(message);
            }
            return status == rock::api::Status::Ok && button.available && button.held;
        }

        void endGrab(const rock::api::Hand hand, HandState& state, const char* reason) noexcept
        {
            char message[200]{};
            std::snprintf(
                message,
                sizeof(message),
                "GRAB END: %s hand let go of %08X (%s), posed %u frames, pose kept by game %u, clothing bone copies turned %u",
                handName(hand),
                state.grabbedFormId,
                reason,
                state.posedFrames,
                state.keptFrames,
                state.copiesTurned);
            npctouch::runtime::logInfo(message);
            state.grabbedFormId = 0;
            state.grabbedBone = nullptr;
            state.grabJoint = nullptr;
            state.releasing = false;
            state.grabNames.clear();
            state.grabTurned.clear();
            state.grabCopies.clear();
            state.skinLinks.clear();
            state.grabElbow = nullptr;
            state.grabCollar = nullptr;
            state.grabHead = nullptr;
        }

        // The joint a grabbed bone turns around: arms at the shoulder, the head
        // on the neck. Torso grabs hold on without posing anything.
        const char* jointFor(const char* bone) noexcept
        {
            if (!bone) {
                return nullptr;
            }
            if (std::strncmp(bone, "LArm_", 5) == 0) {
                return "LArm_UpperArm";
            }
            if (std::strncmp(bone, "RArm_", 5) == 0) {
                return "RArm_UpperArm";
            }
            if (std::strcmp(bone, "Head") == 0 || std::strcmp(bone, "Neck") == 0) {
                return "Neck";
            }
            return nullptr;
        }

        float length(const RE::NiPoint3& v) noexcept
        {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }

        RE::NiPoint3 cross(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        }

        float dot(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        // Rotation (v' = R v) by angle about a unit axis.
        RE::NiMatrix3 axisAngle(const RE::NiPoint3& axis, const float angle) noexcept
        {
            const float sn = std::sin(angle);
            const float cs = std::cos(angle);
            const float t = 1.0f - cs;
            const float x = axis.x, y = axis.y, z = axis.z;
            RE::NiMatrix3 r{};
            r.entry[0][0] = cs + x * x * t;
            r.entry[0][1] = x * y * t - z * sn;
            r.entry[0][2] = x * z * t + y * sn;
            r.entry[1][0] = y * x * t + z * sn;
            r.entry[1][1] = cs + y * y * t;
            r.entry[1][2] = y * z * t - x * sn;
            r.entry[2][0] = z * x * t - y * sn;
            r.entry[2][1] = z * y * t + x * sn;
            r.entry[2][2] = cs + z * z * t;
            return r;
        }

        // Rotation (v' = R v) that turns unit vector a onto unit vector b, or
        // the given fraction of the way.
        RE::NiMatrix3 rotationBetween(const RE::NiPoint3& a, const RE::NiPoint3& b, float& angle, const float fraction = 1.0f) noexcept
        {
            RE::NiPoint3 axis{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
            const float sine = length(axis);
            const float cosine = std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0f, 1.0f);
            angle = std::atan2(sine, cosine);
            RE::NiMatrix3 r{};
            r.MakeIdentity();
            if (sine < 1e-5f) {
                return r;
            }
            axis = axis * (1.0f / sine);
            const float sn = std::sin(angle * fraction);
            const float cs = std::cos(angle * fraction);
            const float t = 1.0f - cs;
            const float x = axis.x, y = axis.y, z = axis.z;
            r.entry[0][0] = cs + x * x * t;
            r.entry[0][1] = x * y * t - z * sn;
            r.entry[0][2] = x * z * t + y * sn;
            r.entry[1][0] = y * x * t + z * sn;
            r.entry[1][1] = cs + y * y * t;
            r.entry[1][2] = y * z * t - x * sn;
            r.entry[2][0] = z * x * t - y * sn;
            r.entry[2][1] = z * y * t + x * sn;
            r.entry[2][2] = cs + z * z * t;
            return r;
        }

        // Turns everything below a moved joint rigidly with it. The children's
        // current world transforms are kept relative to the joint, so bones the
        // game drives some other way (cloth, physics) do not jump.
        void turnChildren(RE::NiAVObject* object, const RE::NiMatrix3& turn, const RE::NiMatrix3& turnStored, const RE::NiPoint3& pivot, const int depth = 0) noexcept
        {
            auto* node = object ? object->IsNode() : nullptr;
            if (!node || depth > 32) {
                return;
            }
            auto& children = node->GetRuntimeData().children;
            for (std::uint16_t index = 0; index < children.size(); ++index) {
                auto* child = children[index].get();
                if (!child) {
                    continue;
                }
                child->world.rotate = child->world.rotate * turnStored;
                child->world.translate = pivot + turn * (child->world.translate - pivot);
                turnChildren(child, turn, turnStored, pivot, depth + 1);
            }
        }

        // Twist bones carry part of the arm's skin but can hang off the parent
        // instead of the arm bone itself; turn the ones beside the joint too.
        void turnTwistSiblings(RE::NiAVObject* joint, const RE::NiMatrix3& turn, const RE::NiMatrix3& turnStored) noexcept
        {
            auto* parent = joint->parent;
            if (!parent) {
                return;
            }
            const auto jointName = joint->GetName();
            if (jointName.size() < 5) {
                return;
            }
            auto& siblings = parent->GetRuntimeData().children;
            for (std::uint16_t index = 0; index < siblings.size(); ++index) {
                auto* sibling = siblings[index].get();
                if (!sibling || sibling == joint) {
                    continue;
                }
                const auto name = sibling->GetName();
                if (name.size() > 5 && std::strncmp(name.data(), jointName.data(), 5) == 0 && std::strstr(name.data(), "Twist")) {
                    sibling->world.rotate = sibling->world.rotate * turnStored;
                    sibling->world.translate = joint->world.translate + turn * (sibling->world.translate - joint->world.translate);
                    if (auto* siblingParent = sibling->parent) {
                        sibling->local.rotate = sibling->world.rotate * siblingParent->world.rotate.Transpose();
                    }
                    turnChildren(sibling, turn, turnStored, joint->world.translate);
                }
            }
        }

        // Logs, once, the bones around a joint we pose: helps when a pose looks wrong.
        void describeJoint(RE::NiAVObject* joint) noexcept
        {
            static int described = 0;
            if (!joint || described >= 4) {
                return;
            }
            ++described;
            char line[512]{};
            int used = std::snprintf(line, sizeof(line), "Joint %s: parents", joint->GetName().data());
            for (auto* up = joint->parent; up && used < 400; up = up->parent) {
                used += std::snprintf(line + used, sizeof(line) - used, " < %s", up->GetName().data());
            }
            npctouch::runtime::logInfo(line);
            auto listChildren = [&](RE::NiAVObject* object, const char* label) {
                auto* node = object ? object->IsNode() : nullptr;
                if (!node) {
                    return;
                }
                char text[512]{};
                int length = std::snprintf(text, sizeof(text), "  %s %s children:", label, object->GetName().data());
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size() && length < 480; ++index) {
                    if (auto* child = children[index].get()) {
                        length += std::snprintf(text + length, sizeof(text) - length, " %s", child->GetName().data());
                    }
                }
                npctouch::runtime::logInfo(text);
            };
            listChildren(joint->parent, "parent");
            listChildren(joint, "joint");
        }

        // How far a node's world transform is from parent.world * local:
        // zero for bones the scene graph drives, large for bones set elsewhere.
        void hierarchyError(RE::NiAVObject* object, float& rotation, float& translation) noexcept
        {
            rotation = translation = -1.0f;
            auto* parent = object ? object->parent : nullptr;
            if (!parent) {
                return;
            }
            const auto expected = object->local.rotate * parent->world.rotate;
            rotation = 0.0f;
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    rotation = std::max(rotation, std::abs(expected.entry[row][column] - object->world.rotate.entry[row][column]));
                }
            }
            const auto expectedTranslate = parent->world.translate + parent->world.rotate.Transpose() * (object->local.translate * parent->world.scale);
            translation = length(expectedTranslate - object->world.translate);
        }

        float rotationGap(const RE::NiMatrix3& a, const RE::NiMatrix3& b) noexcept
        {
            float gap = 0.0f;
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    gap = std::max(gap, std::abs(a.entry[row][column] - b.entry[row][column]));
                }
            }
            return gap;
        }

        void collectNames(RE::NiAVObject* object, std::vector<std::string>& names, const int depth = 0) noexcept
        {
            if (!object || depth > 32) {
                return;
            }
            const auto name = object->GetName();
            if (!name.empty()) {
                names.emplace_back(name.data(), name.size());
            }
            if (auto* node = object->IsNode()) {
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    collectNames(children[index].get(), names, depth + 1);
                }
            }
        }

        bool turnedByGrab(const HandState& state, RE::NiAVObject* bone) noexcept
        {
            for (auto* up = bone; up; up = up->parent) {
                if (std::find(state.grabTurned.begin(), state.grabTurned.end(), up) != state.grabTurned.end()) {
                    return true;
                }
            }
            return false;
        }

        bool namedLikeGrab(const HandState& state, const std::string_view name) noexcept
        {
            for (const auto& wanted : state.grabNames) {
                if (wanted.size() == name.size() && _strnicmp(wanted.data(), name.data(), name.size()) == 0) {
                    return true;
                }
            }
            return false;
        }

        // Skinned meshes (body, outfit, hat) bind to bone nodes through their
        // skin instance. Log every mesh that binds both to bones the grab turns
        // and to bones it leaves alone, since such a mesh stretches. Bones named
        // like a turned bone that sit outside the skeleton are copies; keep them
        // so the pose turns them too.
        // Where the skin scan was when it last touched game memory, for the
        // fault log.
        const char* g_scanStage = "";
        const RE::NiAVObject* g_scanObject = nullptr;
        std::uint32_t g_scanBone = 0;

        // ROCK treats any fault inside our frame callback as fatal before our own
        // SEH handlers run, so raw pointers from the skin data are checked
        // before they are touched: committed readable memory, and for objects a
        // vtable inside the game's read-only data.
        bool readable(const void* address, const std::size_t size) noexcept
        {
            if (!address) {
                return false;
            }
            REX::W32::MEMORY_BASIC_INFORMATION info{};
            if (REX::W32::VirtualQuery(address, &info, sizeof(info)) == 0 || info.state != REX::W32::MEM_COMMIT) {
                return false;
            }
            constexpr std::uint32_t readableMask = REX::W32::PAGE_READONLY | REX::W32::PAGE_READWRITE | REX::W32::PAGE_WRITECOPY |
                                                   REX::W32::PAGE_EXECUTE_READ | REX::W32::PAGE_EXECUTE_READWRITE;
            if ((info.protect & readableMask) == 0 || (info.protect & 0x100u) != 0) {  // 0x100 = PAGE_GUARD
                return false;
            }
            const auto start = reinterpret_cast<std::uintptr_t>(address);
            const auto end = reinterpret_cast<std::uintptr_t>(info.baseAddress) + info.regionSize;
            return start + size <= end;
        }

        bool looksLikeGameObject(const void* object) noexcept
        {
            if (!object || (reinterpret_cast<std::uintptr_t>(object) & 7) != 0 || !readable(object, 0x40)) {
                return false;
            }
            static const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
            const auto vtable = *static_cast<const std::uintptr_t*>(object);
            return vtable >= rdata.address() && vtable < rdata.address() + rdata.size();
        }

        bool writable(const void* address, const std::size_t size) noexcept
        {
            if (!readable(address, size)) {
                return false;
            }
            REX::W32::MEMORY_BASIC_INFORMATION info{};
            REX::W32::VirtualQuery(address, &info, sizeof(info));
            constexpr std::uint32_t writableMask = REX::W32::PAGE_READWRITE | REX::W32::PAGE_WRITECOPY | REX::W32::PAGE_EXECUTE_READWRITE;
            return (info.protect & writableMask) != 0;
        }

        // A bone name, or "?" when the object or its name cannot be read.
        std::string_view safeName(const RE::NiAVObject* object) noexcept
        {
            if (!looksLikeGameObject(object) || !readable(object, sizeof(RE::NiAVObject))) {
                return "?";
            }
            const char* text = object->GetName().data();
            if (!text || !readable(text, 1)) {
                return "?";
            }
            std::size_t size = 0;
            while (size < 64 && readable(text + size, 1) && text[size] != '\0') {
                ++size;
            }
            return { text, size };
        }

        // Bones worth naming in the log when a skin stretches: same arm, or head area.
        bool relevantToGrab(const HandState& state, const std::string_view name) noexcept
        {
            if (state.grabHead || state.scanNearValid) {
                for (const char* word : { "Head", "Neck", "Hat", "HAT", "Hair", "Helm", "Face", "Jaw", "Eye" }) {
                    if (name.find(word) != std::string_view::npos) {
                        return true;
                    }
                }
                return false;
            }
            return name.size() > 4 && !state.grabNames.empty() && state.grabNames.front().size() > 4 &&
                   _strnicmp(name.data(), state.grabNames.front().data(), 4) == 0;
        }

        // The node of the actor nearest to a point; for a skin slot whose bone
        // pointer cannot be read.
        RE::NiAVObject* nearestNode(const HandState& state, const RE::NiPoint3& point, float& distance) noexcept
        {
            RE::NiAVObject* best = nullptr;
            distance = 1.0e9f;
            for (auto* node : state.scanNodes) {
                const float gap = length(node->world.translate - point);
                if (gap < distance) {
                    distance = gap;
                    best = node;
                }
            }
            return best;
        }

        bool followsGrab(const HandState& state, RE::NiAVObject* node) noexcept
        {
            return node && (turnedByGrab(state, node) || node == state.grabCollar);
        }

        void linkSlot(HandState& state, RE::NiTransform* slot, RE::NiAVObject* node) noexcept
        {
            for (const auto& link : state.skinLinks) {
                if (link.slot == slot) {
                    return;
                }
            }
            const float scale = node->world.scale > 0.0f ? node->world.scale : 1.0f;
            SkinLink link{};
            link.slot = slot;
            link.node = node;
            link.rotate = slot->rotate * node->world.rotate.Transpose();
            link.translate = node->world.rotate * (slot->translate - node->world.translate) * (1.0f / scale);
            state.skinLinks.push_back(link);
        }

        // One skinned mesh: classify each bone it binds to, link the transform
        // slots the grab must move by hand, and log meshes that would stretch.
        void scanGeometry(HandState& state, RE::NiAVObject* object, int& logged) noexcept
        {
            auto* geometry = object->IsGeometry();
            if (!geometry) {
                return;
            }
            const std::string_view meshName = safeName(object);
            auto* skin = geometry->GetRuntimeData().skinInstance.get();
            if (!skin) {
                return;
            }
            const bool skinValid = looksLikeGameObject(skin) && readable(skin, sizeof(RE::BSSkin::Instance)) && skin->bonesData &&
                                   skin->bonesCount > 0 && skin->bonesCount < 256 &&
                                   readable(skin->bonesData, skin->bonesCount * sizeof(void*));
            if (!skinValid) {
                if (logged < 30) {
                    char text[200]{};
                    std::snprintf(text, sizeof(text), "  skin '%.*s' has an unreadable skin instance %p", static_cast<int>(meshName.size()), meshName.data(), static_cast<const void*>(skin));
                    npctouch::runtime::logInfo(text);
                    ++logged;
                }
                return;
            }
            auto** bones = static_cast<RE::NiAVObject**>(skin->bonesData);
            auto** worlds = static_cast<RE::NiTransform**>(skin->boneWorldXformData);
            const bool worldsMatch = worlds && skin->boneWorldXformCount == skin->bonesCount &&
                                     readable(worlds, skin->bonesCount * sizeof(void*));
            std::uint32_t turned = 0, copies = 0, still = 0, unreadable = 0, linked = 0, foreign = 0;
            char stillText[400]{};
            int stillUsed = 0;
            char oddText[500]{};
            int oddUsed = 0;
            for (std::uint32_t index = 0; index < skin->bonesCount; ++index) {
                g_scanStage = "bone";
                g_scanBone = index;
                auto* bone = bones[index];
                RE::NiTransform* slot = worldsMatch ? worlds[index] : nullptr;
                const bool slotOk = slot && writable(slot, sizeof(RE::NiTransform));
                const bool boneOk = looksLikeGameObject(bone) && readable(bone, sizeof(RE::NiAVObject));
                if (!boneOk) {
                    // The bone pointer is not a game object; the mesh still
                    // draws from its transform slot. Make the slot follow the
                    // nearest node when the grab moves that node.
                    ++unreadable;
                    float gap{};
                    auto* nearest = slotOk ? nearestNode(state, slot->translate, gap) : nullptr;
                    const bool follow = nearest && gap < 15.0f && followsGrab(state, nearest);
                    if (follow) {
                        linkSlot(state, slot, nearest);
                        ++linked;
                    }
                    if (oddUsed < 400) {
                        const std::string_view nearName = nearest ? safeName(nearest) : std::string_view{ "-" };
                        oddUsed += std::snprintf(
                            oddText + oddUsed,
                            sizeof(oddText) - oddUsed,
                            " #%u %p slot %s near %.*s %.1f%s",
                            index,
                            static_cast<const void*>(bone),
                            slotOk ? "ok" : "bad",
                            static_cast<int>(nearName.size()),
                            nearName.data(),
                            nearest ? gap : -1.0f,
                            follow ? " linked" : "");
                        oddUsed = std::min(oddUsed, static_cast<int>(sizeof(oddText)) - 1);
                    }
                    continue;
                }
                const std::string_view name = safeName(bone);
                if (slot && slot != &bone->world) {
                    ++foreign;
                    if (slotOk && turnedByGrab(state, bone)) {
                        linkSlot(state, slot, bone);
                        ++linked;
                    }
                }
                if (turnedByGrab(state, bone)) {
                    ++turned;
                } else if (namedLikeGrab(state, name)) {
                    ++copies;
                    if (std::find(state.grabCopies.begin(), state.grabCopies.end(), bone) == state.grabCopies.end()) {
                        state.grabCopies.push_back(bone);
                    }
                } else {
                    ++still;
                    if (relevantToGrab(state, name) && stillUsed < 340) {
                        stillUsed += std::snprintf(stillText + stillUsed, sizeof(stillText) - stillUsed, " %.*s", static_cast<int>(name.size()), name.data());
                        stillUsed = std::min(stillUsed, static_cast<int>(sizeof(stillText)) - 1);
                    }
                }
            }
            const bool involved = (turned + copies + linked) > 0;
            if (!involved || logged >= 30) {
                return;
            }
            char text[1100]{};
            std::snprintf(
                text,
                sizeof(text),
                "  skin '%.*s' (%u bones): turned %u, copies %u, still %u, unreadable %u, linked %u, own slots %u%s%s%s",
                static_cast<int>(meshName.size()),
                meshName.data(),
                skin->bonesCount,
                turned,
                copies,
                still,
                unreadable,
                linked,
                foreign,
                stillUsed ? " | still near grab:" : "",
                stillText,
                worldsMatch ? "" : " | no slot array");
            npctouch::runtime::logInfo(text);
            ++logged;
            if (oddUsed > 0 && logged < 30) {
                char line[600]{};
                std::snprintf(line, sizeof(line), "    unreadable bones:%s", oddText);
                npctouch::runtime::logInfo(line);
                ++logged;
            }
        }

        // The skin instance layout comes from reverse engineering, so a fault in
        // one mesh only skips that mesh.
        bool tryScanGeometry(HandState& state, RE::NiAVObject* object, int& logged) noexcept
        {
            __try {
                scanGeometry(state, object, logged);
                return true;
            } __except (1) {
                return false;
            }
        }

        void scanSkins(HandState& state, RE::NiAVObject* object, int& logged, int& faults, const int depth = 0) noexcept
        {
            if (!looksLikeGameObject(object) || depth > 64) {
                return;
            }
            g_scanObject = object;
            g_scanStage = "geometry";
            if (object->IsGeometry() && !tryScanGeometry(state, object, logged)) {
                ++faults;
                if (faults <= 3) {
                    char text[200]{};
                    std::snprintf(text, sizeof(text), "  skin scan skipped a mesh at %s (object %p, bone %u)", g_scanStage, static_cast<const void*>(object), g_scanBone);
                    npctouch::runtime::logInfo(text);
                }
            }
            if (auto* node = object->IsNode()) {
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    scanSkins(state, children[index].get(), logged, faults, depth + 1);
                }
            }
        }

        // Nodes first (so slots can find their nearest node), then the meshes.
        void collectNodes(HandState& state, RE::NiAVObject* object, const int depth = 0) noexcept
        {
            if (!looksLikeGameObject(object) || depth > 64) {
                return;
            }
            if (auto* node = object->IsNode()) {
                state.scanNodes.push_back(object);
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    collectNodes(state, children[index].get(), depth + 1);
                }
            }
        }

        bool tryCollectNodes(HandState& state, RE::NiAVObject* root) noexcept
        {
            __try {
                collectNodes(state, root);
                return true;
            } __except (1) {
                return false;
            }
        }

        bool tryScanSkins(HandState& state, RE::NiAVObject* root, int& logged, int& faults) noexcept
        {
            __try {
                scanSkins(state, root, logged, faults);
                return true;
            } __except (1) {
                return false;
            }
        }

        // After the pose: skin slots that do not read from a turned bone get
        // that bone's new transform (keeping their offset from it).
        void moveLinkedSlots(HandState& state) noexcept
        {
            for (const auto& link : state.skinLinks) {
                auto* node = link.node;
                const float scale = node->world.scale > 0.0f ? node->world.scale : 1.0f;
                link.slot->rotate = link.rotate * node->world.rotate;
                link.slot->translate = node->world.translate + node->world.rotate.Transpose() * link.translate * scale;
            }
        }

        // Worn clothing and hats can carry their own copies of the skeleton's
        // bones, synced from it before we pose. Turn those copies (and what
        // hangs off them) the same way, skipping the skeleton bones already turned.
        void turnCopies(
            HandState& state,
            RE::NiAVObject* object,
            const RE::NiMatrix3& turn,
            const RE::NiMatrix3& turnStored,
            const RE::NiPoint3& pivot,
            const bool inside,
            const int depth = 0) noexcept
        {
            if (!object || depth > 64) {
                return;
            }
            if (std::find(state.grabTurned.begin(), state.grabTurned.end(), object) != state.grabTurned.end()) {
                return;
            }
            // Skin bone copies are turned on their own after this walk.
            const bool skinCopy = std::find(state.grabCopies.begin(), state.grabCopies.end(), object) != state.grabCopies.end();
            bool turnThis = inside && !skinCopy;
            if (!turnThis && !skinCopy) {
                const auto name = object->GetName();
                for (const auto& wanted : state.grabNames) {
                    if (wanted.size() == name.size() && _strnicmp(wanted.data(), name.data(), name.size()) == 0) {
                        turnThis = true;
                        break;
                    }
                }
            }
            if (turnThis) {
                object->world.rotate = object->world.rotate * turnStored;
                object->world.translate = pivot + turn * (object->world.translate - pivot);
                if (!inside) {
                    ++state.copiesTurned;
                }
            }
            if (auto* node = object->IsNode()) {
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    turnCopies(state, children[index].get(), turn, turnStored, pivot, turnThis, depth + 1);
                }
            }
        }

        // Turns a node (and everything below it) by a world turn about a pivot.
        void turnNode(RE::NiAVObject* node, const RE::NiMatrix3& turn, const RE::NiPoint3& pivot) noexcept
        {
            const auto turnStored = turn.Transpose();
            node->world.rotate = node->world.rotate * turnStored;
            node->world.translate = pivot + turn * (node->world.translate - pivot);
            if (auto* parent = node->parent) {
                node->local.rotate = node->world.rotate * parent->world.rotate.Transpose();
            }
            turnChildren(node, turn, turnStored, pivot);
        }

        // Turn about pivot that moves point toward target by the given share,
        // at most maxAngle radians. False when there is nothing to turn.
        bool aimTurn(const RE::NiPoint3& pivot, const RE::NiPoint3& point, const RE::NiPoint3& target, const float share, const float maxAngle, RE::NiMatrix3& turn) noexcept
        {
            const RE::NiPoint3 from = point - pivot;
            const RE::NiPoint3 to = target - pivot;
            const float fromLength = length(from);
            const float toLength = length(to);
            if (fromLength < 1.0f || toLength < 1.0f || share <= 0.0f) {
                return false;
            }
            float angle{ 0.0f };
            (void)rotationBetween(from * (1.0f / fromLength), to * (1.0f / toLength), angle);
            if (angle < 0.002f) {
                return false;
            }
            const float fraction = std::min(share, maxAngle / angle);
            turn = rotationBetween(from * (1.0f / fromLength), to * (1.0f / toLength), angle, fraction);
            return true;
        }

        // Bends the elbow so the shoulder-to-grab distance matches the
        // shoulder-to-hand distance (law of cosines); the shoulder turn after
        // it then aims the arm. The hinge axis was fixed in the upper arm's
        // frame when the grab began.
        void bendElbow(HandState& state, RE::NiAVObject* shoulder, RE::NiAVObject* elbow, const RE::NiPoint3& target, const float fraction) noexcept
        {
            const RE::NiPoint3 s = shoulder->world.translate;
            const RE::NiPoint3 e = elbow->world.translate;
            const RE::NiPoint3 g = e + elbow->world.rotate.Transpose() * state.grabLocalFore * elbow->world.scale;
            const float a = length(e - s);
            const float b = length(g - e);
            if (a < 4.0f || b < 4.0f) {
                return;
            }
            const float d = std::clamp(length(target - s), std::abs(a - b) + 0.5f, a + b - 0.5f);
            const float wantedAngle = std::clamp(std::acos(std::clamp((a * a + b * b - d * d) / (2.0f * a * b), -1.0f, 1.0f)), kElbowMinAngle, kElbowMaxAngle);
            RE::NiPoint3 axis = shoulder->world.rotate.Transpose() * state.elbowAxisLocal;
            const float axisLength = length(axis);
            if (axisLength < 0.5f) {
                return;
            }
            axis = axis * (1.0f / axisLength);
            const RE::NiPoint3 u = (s - e) * (1.0f / a);
            const RE::NiPoint3 v = (g - e) * (1.0f / b);
            float angle = std::atan2(dot(cross(u, v), axis), dot(u, v));
            if (angle < 0.0f) {
                // Bent past straight the wrong way: measure it as more than 180 degrees.
                angle += 6.2831853f;
            }
            const float delta = (wantedAngle - angle) * fraction;
            if (std::abs(delta) < 0.002f) {
                return;
            }
            const auto turn = axisAngle(axis, delta);
            const auto turnStored = turn.Transpose();
            elbow->world.rotate = elbow->world.rotate * turnStored;
            if (auto* parent = elbow->parent) {
                elbow->local.rotate = elbow->world.rotate * parent->world.rotate.Transpose();
            }
            turnChildren(elbow, turn, turnStored, e);
        }

        // Turns the joint so the grabbed point sits in the hand.
        void poseGrab(HandState& state, RE::NiAVObject* joint) noexcept
        {
            auto* parent = joint->parent;
            if (!parent) {
                return;
            }
            float fraction = 1.0f;
            RE::NiPoint3 target = state.position;
            if (state.releasing) {
                target = state.releaseTarget;
                fraction = 1.0f - std::chrono::duration<float>(Clock::now() - state.releaseStart).count() / kReleaseSeconds;
                if (fraction <= 0.0f) {
                    return;
                }
            }
            auto grabPoint = [&]() {
                if (auto* elbow = state.grabElbow) {
                    return elbow->world.translate + elbow->world.rotate.Transpose() * state.grabLocalFore * elbow->world.scale;
                }
                return joint->world.translate + joint->world.rotate.Transpose() * state.grabLocal * joint->world.scale;
            };
            if (auto* collar = state.grabCollar) {
                RE::NiMatrix3 collarTurn{};
                if (aimTurn(collar->world.translate, grabPoint(), target, g_collarShare * fraction, 0.45f, collarTurn)) {
                    turnNode(collar, collarTurn, collar->world.translate);
                }
            }
            RE::NiPoint3 current = joint->world.rotate.Transpose() * state.grabLocal * joint->world.scale;
            if (state.grabElbow) {
                bendElbow(state, joint, state.grabElbow, target, fraction);
                auto* elbow = state.grabElbow;
                const RE::NiPoint3 point = elbow->world.translate + elbow->world.rotate.Transpose() * state.grabLocalFore * elbow->world.scale;
                current = point - joint->world.translate;
            }
            const RE::NiPoint3 wanted = target - joint->world.translate;
            const float currentLength = length(current);
            const float wantedLength = length(wanted);
            if (currentLength < 1.0f || wantedLength < 1.0f) {
                return;
            }
            float angle{ 0.0f };
            // A head grab turns the neck only part of the way; the head does the rest.
            const float jointShare = state.grabHead ? std::clamp(g_neckShare, 0.0f, 1.0f) : 1.0f;
            const auto turn = rotationBetween(current * (1.0f / currentLength), wanted * (1.0f / wantedLength), angle, fraction * jointShare);
            // If last frame's pose is still there, the game did not re-animate
            // the joint since; worth knowing for timing.
            if (angle < 0.02f) {
                ++state.keptFrames;
            }
            // For the first frames, log whether last frame's pose on the joint
            // and on a skin bone survived until now.
            if (state.posedFrames >= 1 && state.posedFrames <= 3) {
                char message[256]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "  pose frame %u: joint since last set rot %.3f; skin bone %s since last set rot %.3f move %.1f",
                    state.posedFrames,
                    rotationGap(joint->world.rotate, state.lastJointWorld.rotate),
                    state.skinBone ? state.skinBone->GetName().data() : "-",
                    state.skinBone ? rotationGap(state.skinBone->world.rotate, state.lastSkinWorld.rotate) : -1.0f,
                    state.skinBone ? length(state.skinBone->world.translate - state.lastSkinWorld.translate) : -1.0f);
                npctouch::runtime::logInfo(message);
            }
            ++state.posedFrames;
            // Game matrices map world vectors into the node's frame
            // (world = R^T * local), so a world turn T becomes R * T^T.
            const auto turnStored = turn.Transpose();
            joint->world.rotate = joint->world.rotate * turnStored;
            joint->local.rotate = joint->world.rotate * parent->world.rotate.Transpose();
            const RE::NiPoint3 pivot = joint->world.translate;
            turnChildren(joint, turn, turnStored, pivot);
            turnTwistSiblings(joint, turn, turnStored);
            if (state.grabRoot) {
                turnCopies(state, state.grabRoot, turn, turnStored, pivot, false);
            }
            for (auto* copy : state.grabCopies) {
                copy->world.rotate = copy->world.rotate * turnStored;
                copy->world.translate = pivot + turn * (copy->world.translate - pivot);
                ++state.copiesTurned;
            }
            if (auto* head = state.grabHead; head && jointShare < 1.0f) {
                RE::NiMatrix3 headTurn{};
                if (aimTurn(head->world.translate, grabPoint(), target, fraction, 3.2f, headTurn)) {
                    turnNode(head, headTurn, head->world.translate);
                }
            }
            moveLinkedSlots(state);
            state.lastJointWorld = joint->world;
            if (state.skinBone) {
                state.lastSkinWorld = state.skinBone->world;
            }
        }

        // For a grab on the forearm or hand, remember the grabbed point in the
        // forearm's frame and the elbow's hinge axis in the upper arm's frame.
        // The hinge comes from the current bend; a straight arm bends forward.
        void setUpElbow(HandState& state, RE::Actor* actor, RE::NiAVObject* root, RE::NiAVObject* shoulder, const char* bone) noexcept
        {
            state.grabElbow = nullptr;
            if (!g_bendElbow || !bone || (std::strstr(bone, "ForeArm") == nullptr && std::strstr(bone, "_Hand") == nullptr)) {
                return;
            }
            const char* elbowName = bone[0] == 'L' ? "LArm_ForeArm1" : "RArm_ForeArm1";
            auto* elbow = findNode(root, elbowName);
            if (!elbow || elbow->world.scale <= 0.0f) {
                return;
            }
            const RE::NiPoint3 s = shoulder->world.translate;
            const RE::NiPoint3 e = elbow->world.translate;
            const RE::NiPoint3 g = state.position;
            const float a = length(e - s);
            const float b = length(g - e);
            if (a < 4.0f || b < 4.0f) {
                return;
            }
            RE::NiPoint3 axis = cross((s - e) * (1.0f / a), (g - e) * (1.0f / b));
            const char* source = "current bend";
            if (length(axis) < 0.17f) {
                // Straight arm: hinge along the body's left, so the forearm comes forward.
                const float heading = actor->data.angle.z;
                axis = { -std::cos(heading), std::sin(heading), 0.0f };
                source = "body left";
            }
            axis = axis * (1.0f / length(axis));
            state.grabElbow = elbow;
            state.grabLocalFore = elbow->world.rotate * (g - e) * (1.0f / elbow->world.scale);
            state.elbowAxisLocal = shoulder->world.rotate * axis;
            state.grabReach = std::max(state.grabReach, a + b);
            char text[200]{};
            std::snprintf(text, sizeof(text), "  elbow: upper %.0f, lower %.0f, hinge from %s", a, b, source);
            npctouch::runtime::logInfo(text);
        }

        void loadGrabSettings() noexcept
        {
            try {
                std::ifstream file("Data/F4SE/Plugins/NpcTouch.ini");
                std::string line;
                while (file && std::getline(file, line)) {
                    const auto equals = line.find('=');
                    if (line.empty() || line[0] == ';' || equals == std::string::npos) {
                        continue;
                    }
                    auto key = line.substr(0, equals);
                    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                        key.pop_back();
                    }
                    const float value = std::strtof(line.c_str() + equals + 1, nullptr);
                    if (key == "bBendElbow") {
                        g_bendElbow = value != 0.0f;
                    } else if (key == "fCollarboneShare") {
                        g_collarShare = std::clamp(value, 0.0f, 1.0f);
                    } else if (key == "fNeckShare") {
                        g_neckShare = std::clamp(value, 0.0f, 1.0f);
                    } else if (key == "bScanSkins") {
                        g_scanSkinsEnabled = value != 0.0f;
                    } else if (key == "bFlinch") {
                        g_flinchEnabled = value != 0.0f;
                    } else if (key == "fFlinchFollow") {
                        g_flinchFollow = std::clamp(value, 0.0f, 1.0f);
                    } else if (key == "fFlinchStiffness") {
                        g_flinchStiffness = std::clamp(value, 1.0f, 40.0f);
                    } else if (key == "fFlinchDamping") {
                        g_flinchDamping = std::clamp(value, 0.05f, 2.0f);
                    } else if (key == "fFlinchMinSpeed") {
                        g_flinchMinSpeed = std::max(value, 0.0f);
                    } else if (key == "fFlinchMaxTorso") {
                        g_flinchMaxTorso = std::clamp(value, 0.0f, 1.0f);
                    } else if (key == "fFlinchMaxHead") {
                        g_flinchMaxHead = std::clamp(value, 0.0f, 1.2f);
                    } else if (key == "fFlinchMaxArm") {
                        g_flinchMaxArm = std::clamp(value, 0.0f, 1.5f);
                    }
                }
            } catch (...) {
            }
            char message[240]{};
            std::snprintf(
                message,
                sizeof(message),
                "Grab settings: bend elbow %d, scan skins %d, collarbone share %.2f, neck share %.2f",
                g_bendElbow ? 1 : 0,
                g_scanSkinsEnabled ? 1 : 0,
                g_collarShare,
                g_neckShare);
            npctouch::runtime::logInfo(message);
            std::snprintf(
                message,
                sizeof(message),
                "Flinch settings: on %d, follow %.2f, stiffness %.1f, damping %.2f, min speed %.0f, max torso/head/arm %.2f/%.2f/%.2f",
                g_flinchEnabled ? 1 : 0,
                g_flinchFollow,
                g_flinchStiffness,
                g_flinchDamping,
                g_flinchMinSpeed,
                g_flinchMaxTorso,
                g_flinchMaxHead,
                g_flinchMaxArm);
            npctouch::runtime::logInfo(message);
        }

        // Flinch: a touched NPC gives way. The touched part (upper body, head
        // or arm) turns about its joint with the hand's push and springs back.
        // Each flinch keeps its own pose state, so the skin fixes of grabs
        // (bone copies, linked slots) apply to it too.

        struct Flinch
        {
            bool active{ false };
            std::uint32_t formId{ 0 };
            const char* jointName{ nullptr };
            RE::NiAVObject* root{ nullptr };
            float maxAngle{ 0.0f };
            RE::NiPoint3 angle{};     // rotation vector: axis * radians
            RE::NiPoint3 spin{};      // its rate, rad/s
            Clock::time_point last{};
            Clock::time_point lastContact{};
            HandState pose{};
        };

        std::array<Flinch, 4> g_flinches{};

        // The joint a touch on this bone turns, and how far it may go.
        const char* flinchJoint(const char* bone, float& maxAngle) noexcept
        {
            if (!bone) {
                return nullptr;
            }
            if (std::strncmp(bone, "LArm_", 5) == 0 || std::strncmp(bone, "RArm_", 5) == 0) {
                maxAngle = g_flinchMaxArm;
                return bone[0] == 'L' ? "LArm_UpperArm" : "RArm_UpperArm";
            }
            if (std::strcmp(bone, "Head") == 0 || std::strcmp(bone, "Neck") == 0) {
                maxAngle = g_flinchMaxHead;
                return "Neck";
            }
            if (std::strcmp(bone, "Chest") == 0 || std::strcmp(bone, "SPINE2") == 0) {
                maxAngle = g_flinchMaxTorso;
                return "SPINE2";
            }
            if (std::strcmp(bone, "SPINE1") == 0 || std::strcmp(bone, "Pelvis") == 0) {
                maxAngle = g_flinchMaxTorso * 0.6f;
                return "SPINE1";
            }
            return nullptr;
        }

        // Fills a pose state with the bones a turn of this joint moves, finds
        // the clothing bone copies and skin slots that must follow.
        void preparePose(HandState& pose, RE::NiAVObject* root, RE::NiAVObject* joint) noexcept
        {
            pose.grabNames.clear();
            pose.grabTurned.clear();
            pose.grabCopies.clear();
            pose.skinLinks.clear();
            pose.grabCollar = nullptr;
            pose.grabHead = nullptr;
            pose.scanNearValid = false;
            collectNames(joint, pose.grabNames);
            pose.grabTurned.push_back(joint);
            if (auto* parent = joint->parent) {
                const std::string_view ownName = joint->GetName();
                auto& siblings = parent->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < siblings.size(); ++index) {
                    auto* sibling = siblings[index].get();
                    if (!sibling || sibling == joint) {
                        continue;
                    }
                    const std::string_view name = sibling->GetName();
                    if (name.size() > 5 && ownName.size() >= 5 && name.substr(0, 5) == ownName.substr(0, 5) &&
                        name.find("Twist") != std::string_view::npos) {
                        collectNames(sibling, pose.grabNames);
                        pose.grabTurned.push_back(sibling);
                    }
                }
            }
            if (g_scanSkinsEnabled) {
                int logged = 30;  // no per-mesh lines for flinches
                int faults = 0;
                pose.scanNodes.clear();
                if (tryCollectNodes(pose, root)) {
                    (void)tryScanSkins(pose, root, logged, faults);
                }
                pose.scanNodes.clear();
            }
        }

        bool grabbedByHand(const std::uint32_t formId) noexcept
        {
            for (const auto& state : g_handStates) {
                if (state.grabbedFormId == formId) {
                    return true;
                }
            }
            return false;
        }

        // A hand inside an NPC's reach pushes the touched part along.
        void pushFlinch(const HandState& hand, const Hit& hit, const float seconds) noexcept
        {
            if (!g_flinchEnabled || !hit.actor || hit.actor->IsDead(false) || grabbedByHand(hit.actor->GetFormID())) {
                return;
            }
            const RE::NiPoint3 velocity{ hand.velocity[0], hand.velocity[1], hand.velocity[2] };
            if (length(velocity) < g_flinchMinSpeed) {
                return;
            }
            float maxAngle{ 0.0f };
            const char* jointName = flinchJoint(hit.bone, maxAngle);
            auto* root = jointName ? hit.actor->Get3D() : nullptr;
            auto* joint = root ? findNode(root, jointName) : nullptr;
            if (!joint || maxAngle <= 0.0f) {
                return;
            }
            const RE::NiPoint3 arm = hand.position - joint->world.translate;
            const float reach = length(arm);
            if (reach < 5.0f) {
                return;
            }
            const auto formId = hit.actor->GetFormID();
            Flinch* flinch = nullptr;
            for (auto& entry : g_flinches) {
                if (entry.active && entry.formId == formId && entry.jointName == jointName && entry.root == root) {
                    flinch = &entry;
                    break;
                }
            }
            const auto now = Clock::now();
            if (!flinch) {
                for (auto& entry : g_flinches) {
                    if (!entry.active) {
                        flinch = &entry;
                        break;
                    }
                }
                if (!flinch) {
                    return;
                }
                flinch->active = true;
                flinch->formId = formId;
                flinch->jointName = jointName;
                flinch->root = root;
                flinch->maxAngle = maxAngle;
                flinch->angle = {};
                flinch->spin = {};
                flinch->last = now;
                preparePose(flinch->pose, root, joint);
                char message[200]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "FLINCH: '%s' %s gives way (hand %.0f u/s), %zu bone copies, %zu skin slots follow",
                    actorName(hit.actor),
                    jointName,
                    length(velocity),
                    flinch->pose.grabCopies.size(),
                    flinch->pose.skinLinks.size());
                npctouch::runtime::logInfo(message);
            }
            // The hand's turn rate about the joint; the body's spin is drawn
            // toward a share of it while the hand is in contact.
            const RE::NiPoint3 handSpin = cross(arm, velocity) * (1.0f / (reach * reach));
            const float blend = std::min(1.0f, 20.0f * seconds);
            flinch->spin = flinch->spin + (handSpin * g_flinchFollow - flinch->spin) * blend;
            flinch->lastContact = now;
        }

        // Steps every flinch's spring and poses it after the animation.
        void updateFlinches() noexcept
        {
            const auto now = Clock::now();
            for (auto& flinch : g_flinches) {
                if (!flinch.active) {
                    continue;
                }
                const float seconds = std::clamp(std::chrono::duration<float>(now - flinch.last).count(), 0.0f, 0.05f);
                flinch.last = now;
                auto* actor = RE::TESForm::GetFormByID<RE::Actor>(flinch.formId);
                auto* root = actor ? actor->Get3D() : nullptr;
                if (!root || root != flinch.root || actor->IsDead(false) || grabbedByHand(flinch.formId)) {
                    flinch.active = false;
                    continue;
                }
                const float k = g_flinchStiffness * g_flinchStiffness;
                const float c = 2.0f * g_flinchDamping * g_flinchStiffness;
                flinch.spin = flinch.spin + (flinch.angle * -k - flinch.spin * c) * seconds;
                flinch.angle = flinch.angle + flinch.spin * seconds;
                float angle = length(flinch.angle);
                if (angle > flinch.maxAngle) {
                    flinch.angle = flinch.angle * (flinch.maxAngle / angle);
                    angle = flinch.maxAngle;
                    // Stop pushing further at the limit.
                    const RE::NiPoint3 axis = flinch.angle * (1.0f / angle);
                    const float outward = dot(flinch.spin, axis);
                    if (outward > 0.0f) {
                        flinch.spin = flinch.spin - axis * outward;
                    }
                }
                const bool resting = angle < 0.002f && length(flinch.spin) < 0.02f &&
                                     now - flinch.lastContact > std::chrono::milliseconds(150);
                if (resting) {
                    flinch.active = false;
                    continue;
                }
                auto* joint = findNode(root, flinch.jointName);
                if (!joint || !joint->parent || angle < 1e-5f) {
                    continue;
                }
                const auto turn = axisAngle(flinch.angle * (1.0f / angle), angle);
                const auto turnStored = turn.Transpose();
                const RE::NiPoint3 pivot = joint->world.translate;
                auto& pose = flinch.pose;
                joint->world.rotate = joint->world.rotate * turnStored;
                joint->local.rotate = joint->world.rotate * joint->parent->world.rotate.Transpose();
                turnChildren(joint, turn, turnStored, pivot);
                turnTwistSiblings(joint, turn, turnStored);
                turnCopies(pose, root, turn, turnStored, pivot, false);
                for (auto* copy : pose.grabCopies) {
                    copy->world.rotate = copy->world.rotate * turnStored;
                    copy->world.translate = pivot + turn * (copy->world.translate - pivot);
                }
                moveLinkedSlots(pose);
            }
        }

        void updateGrab(const rock::api::Hand hand, HandState& state, const Hit& hit, const bool grip) noexcept
        {
            const bool pressed = grip && !state.gripWasHeld;
            state.gripWasHeld = grip;

            if (state.grabbedFormId != 0) {
                if (state.releasing && pressed) {
                    endGrab(hand, state, "grabbed again");
                } else if (!grip && !state.releasing) {
                    state.releasing = true;
                    state.releaseStart = Clock::now();
                    state.releaseTarget = state.position;
                }
            }
            if (state.grabbedFormId != 0) {
                auto* actor = RE::TESForm::GetFormByID<RE::Actor>(state.grabbedFormId);
                auto* root = actor ? actor->Get3D() : nullptr;
                if (state.releasing && Clock::now() - state.releaseStart >= std::chrono::duration<float>(kReleaseSeconds)) {
                    endGrab(hand, state, "released");
                    return;
                }
                if (!root || actor->IsDead(false) || root != state.grabRoot) {
                    endGrab(hand, state, "NPC gone");
                    return;
                }
                if (state.grabJoint) {
                    const RE::NiPoint3 reach = state.position - state.grabJoint->world.translate;
                    if (!state.releasing && length(reach) > state.grabReach + kGrabBreak) {
                        endGrab(hand, state, "slipped");
                        return;
                    }
                    poseGrab(state, state.grabJoint);
                }
                if (!state.releasing) {
                    const auto now = Clock::now();
                    const float seconds = std::chrono::duration<float>(now - state.lastHoldReaction).count();
                    if (seconds >= 1.0f) {
                        state.lastHoldReaction = now;
                        if (npctouch::reactions::onGrabHeld(
                                actor,
                                state.grabbedBone,
                                std::min(seconds, 2.0f),
                                std::chrono::duration<float>(now - state.grabStart).count())) {
                            npctouch::runtime::logInfo("GRAB: NPC pulled free");
                            state.releasing = true;
                            state.releaseStart = now;
                            state.releaseTarget = state.position;
                        }
                    }
                }
                return;
            }

            if (!pressed) {
                return;
            }
            char message[220]{};
            std::snprintf(
                message,
                sizeof(message),
                "GRIP PRESS: %s hand, nearest '%s' bone=%s gap=%.0f",
                handName(hand),
                hit.actor ? actorName(hit.actor) : "-",
                hit.actor ? hit.bone : "-",
                hit.actor ? hit.gap : -1.0f);
            npctouch::runtime::logInfo(message);
            if (!hit.actor || hit.gap > kGrabReach || hit.actor->IsDead(false)) {
                return;
            }
            auto* root = hit.actor->Get3D();
            const char* jointName = jointFor(hit.bone);
            auto* joint = root && jointName ? findNode(root, jointName) : nullptr;
            state.grabbedFormId = hit.actor->GetFormID();
            state.grabbedBone = hit.bone;
            state.grabRoot = root;
            state.grabJoint = joint;
            state.posedFrames = 0;
            state.keptFrames = 0;
            state.copiesTurned = 0;
            state.releasing = false;
            state.grabNames.clear();
            state.grabTurned.clear();
            if (joint) {
                collectNames(joint, state.grabNames);
                state.grabTurned.push_back(joint);
                if (auto* parent = joint->parent) {
                    const std::string_view ownName = joint->GetName();
                    auto& siblings = parent->GetRuntimeData().children;
                    for (std::uint16_t index = 0; index < siblings.size(); ++index) {
                        auto* sibling = siblings[index].get();
                        if (!sibling || sibling == joint) {
                            continue;
                        }
                        const std::string_view name = sibling->GetName();
                        if (name.size() > 5 && ownName.size() >= 5 && name.substr(0, 5) == ownName.substr(0, 5) &&
                            name.find("Twist") != std::string_view::npos) {
                            collectNames(sibling, state.grabNames);
                            state.grabTurned.push_back(sibling);
                        }
                    }
                }
            }
            state.skinBone = nullptr;
            state.grabCopies.clear();
            state.grabCollar = nullptr;
            state.grabHead = nullptr;
            state.scanNearValid = false;
            if (joint) {
                const std::string_view name = joint->GetName();
                if (name.find("UpperArm") != std::string_view::npos && joint->parent && g_collarShare > 0.0f) {
                    state.grabCollar = joint->parent;
                }
                if (name == "Neck") {
                    if (auto* head = findNode(joint, "Head")) {
                        state.scanNear = head->world.translate;
                        state.scanNearValid = true;
                        if (g_neckShare < 1.0f) {
                            state.grabHead = head;
                        }
                    }
                }
            }
            state.skinLinks.clear();
            if (joint && root && g_scanSkinsEnabled) {
                int logged = 0;
                int faults = 0;
                char text[200]{};
                state.scanNodes.clear();
                const bool nodesOk = tryCollectNodes(state, root);
                const bool walked = nodesOk && tryScanSkins(state, root, logged, faults);
                state.scanNodes.clear();
                std::snprintf(
                    text,
                    sizeof(text),
                    "  skins: %d lines, %zu bone copies, %zu slots linked, %d meshes skipped%s",
                    logged,
                    state.grabCopies.size(),
                    state.skinLinks.size(),
                    faults,
                    walked ? "" : ", walk stopped by a fault");
                npctouch::runtime::logInfo(text);
            }
            if (joint) {
                describeJoint(joint);
                if (auto* node = joint->IsNode()) {
                    char text[512]{};
                    int used = std::snprintf(text, sizeof(text), "  hierarchy error (rot/move) of %s children:", joint->GetName().data());
                    auto& children = node->GetRuntimeData().children;
                    for (std::uint16_t index = 0; index < children.size() && used < 460; ++index) {
                        auto* child = children[index].get();
                        if (!child) {
                            continue;
                        }
                        float rotation{}, translation{};
                        hierarchyError(child, rotation, translation);
                        used += std::snprintf(text + used, sizeof(text) - used, " %s=%.3f/%.1f", child->GetName().data(), rotation, translation);
                        if (!state.skinBone && std::strstr(child->GetName().data(), "_skin")) {
                            state.skinBone = child;
                        }
                    }
                    float rotation{}, translation{};
                    hierarchyError(joint, rotation, translation);
                    used += std::snprintf(text + used, sizeof(text) - used, " | joint=%.3f/%.1f", rotation, translation);
                    npctouch::runtime::logInfo(text);
                }
                const RE::NiPoint3 offset = state.position - joint->world.translate;
                state.grabReach = length(offset);
                state.grabLocal = joint->world.rotate * offset * (1.0f / joint->world.scale);
                setUpElbow(state, hit.actor, root, joint, hit.bone);
            }
            state.lastHoldReaction = Clock::now();
            state.grabStart = state.lastHoldReaction;
            std::snprintf(
                message,
                sizeof(message),
                "GRAB BEGIN: %s hand grabbed '%s' form=%08X bone=%s joint=%s",
                handName(hand),
                actorName(hit.actor),
                state.grabbedFormId,
                hit.bone,
                joint ? jointName : "none");
            npctouch::runtime::logInfo(message);
            char overlay[128]{};
            std::snprintf(overlay, sizeof(overlay), "NpcTouch: grabbed %s (%s)", actorName(hit.actor), hit.bone);
            showOverlay(overlay);
            const float still[3]{};
            npctouch::reactions::onTouch(hit.actor, hit.bone, still);
        }

        void update(const std::uint64_t ownerToken) noexcept
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!g_hands || !player) {
                return;
            }
            constexpr std::array handIds{ rock::api::Hand::Right, rock::api::Hand::Left };
            for (std::size_t index = 0; index < handIds.size(); ++index) {
                auto& state = g_handStates[index];
                if (!readHand(ownerToken, handIds[index], state)) {
                    continue;
                }
                g_diag.nearbyActors = 0;
                const auto hit = nearestBone(state.position, player);
                auto& diag = g_diag.hand[index];
                if (hit.actor && (diag.minGap < 0.0f || hit.gap < diag.minGap)) {
                    diag.minGap = std::max(hit.gap, 0.0f);
                    std::snprintf(diag.minBone, sizeof(diag.minBone), "%s", hit.bone);
                    std::snprintf(diag.minName, sizeof(diag.minName), "%s", actorName(hit.actor));
                }
                updateGrab(handIds[index], state, hit, gripHeld(ownerToken, handIds[index], state));
                const bool inside = hit.actor && hit.gap <= kHandReach;
                if (inside && state.grabbedFormId == 0) {
                    pushFlinch(state, hit, 1.0f / 90.0f);
                }
                const bool released = !hit.actor || hit.gap > kHandReach + kReleaseMargin ||
                                      (state.touchedFormId != 0 && hit.actor->GetFormID() != state.touchedFormId);

                if (state.touchedFormId == 0 && inside) {
                    state.touchedFormId = hit.actor->GetFormID();
                    state.touchedBone = hit.bone;
                    ++g_npcTouchCount;
                    char message[256]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "PROXIMITY TOUCH BEGIN: %s hand -> NPC '%s' form=%08X bone=%s handSpeed=%.0f u/s smoothed=%.0f",
                        handName(handIds[index]),
                        actorName(hit.actor),
                        state.touchedFormId,
                        hit.bone,
                        state.speed,
                        std::sqrt(state.velocity[0] * state.velocity[0] + state.velocity[1] * state.velocity[1] + state.velocity[2] * state.velocity[2]));
                    npctouch::runtime::logInfo(message);
                    char overlay[128]{};
                    std::snprintf(overlay, sizeof(overlay), "NpcTouch: %s hand -> %s (%s)", handName(handIds[index]), actorName(hit.actor), hit.bone);
                    showOverlay(overlay);
                    npctouch::collision::describeActor(hit.actor);
                    npctouch::reactions::onTouch(hit.actor, hit.bone, state.velocity);
                } else if (state.touchedFormId != 0 && released) {
                    char message[160]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "PROXIMITY TOUCH END: %s hand left form=%08X bone=%s",
                        handName(handIds[index]),
                        state.touchedFormId,
                        state.touchedBone ? state.touchedBone : "?");
                    npctouch::runtime::logInfo(message);
                    state.touchedFormId = 0;
                    state.touchedBone = nullptr;
                }
            }
            updateFlinches();
        }
    }

    void logSummary() noexcept
    {
        const auto now = Clock::now();
        if (now - g_lastSummary < kSummaryInterval) {
            return;
        }
        g_lastSummary = now;
        char message[192]{};
        std::snprintf(
            message,
            sizeof(message),
            "Summary: NPC touches so far=%llu, in last %llds: non-NPC touch events=%llu, finger contact begins=%llu",
            static_cast<unsigned long long>(g_npcTouchCount),
            static_cast<long long>(kSummaryInterval.count()),
            static_cast<unsigned long long>(g_otherTouchCount),
            static_cast<unsigned long long>(g_contactBeginCount));
        npctouch::runtime::logInfo(message);
        char detail[256]{};
        std::snprintf(
            detail,
            sizeof(detail),
            "  status: events=%u contacts=%u (seen %u) hands=%s nearbyNPCs=%u",
            g_diag.eventsStatus,
            g_diag.contactsStatus,
            g_diag.contactsSeen,
            g_hands ? "yes" : "no",
            g_diag.nearbyActors);
        npctouch::runtime::logInfo(detail);
        for (int index = 0; index < 2; ++index) {
            auto& diag = g_diag.hand[index];
            char line[256]{};
            std::snprintf(
                line,
                sizeof(line),
                "  %s hand: frame=%u flags=%X at (%.0f, %.0f, %.0f) closest='%s' bone=%s gap=%.0f",
                index == 0 ? "right" : "left",
                diag.status,
                diag.flags,
                diag.position[0],
                diag.position[1],
                diag.position[2],
                diag.minGap < 0.0f ? "-" : diag.minName,
                diag.minGap < 0.0f ? "-" : diag.minBone,
                diag.minGap);
            npctouch::runtime::logInfo(line);
            diag.minGap = -1.0f;
        }
        char where[256]{};
        std::snprintf(
            where,
            sizeof(where),
            "  nearest NPC at (%.0f, %.0f, %.0f) bone at (%.0f, %.0f, %.0f) source=%s",
            g_diag.actorPosition[0],
            g_diag.actorPosition[1],
            g_diag.actorPosition[2],
            g_diag.bonePosition[0],
            g_diag.bonePosition[1],
            g_diag.bonePosition[2],
            g_diag.usedCapsule ? "capsule" : "skeleton");
        npctouch::runtime::logInfo(where);
        g_diag.contactsSeen = 0;
        g_otherTouchCount = 0;
        g_contactBeginCount = 0;
    }

    void publishOverlay(
        const std::uint64_t ownerToken,
        const rock::api::core::SnapshotV1& snapshot) noexcept
    {
        if (!g_diagnostics || Clock::now() >= g_overlayUntil ||
            !hasLifecycleFlag(snapshot.lifecycleFlags, rock::api::core::LifecycleFlag::VisualWriteAllowed)) {
            return;
        }
        diagnostics::DebugOverlayTextV1 text{};
        std::snprintf(text.text, sizeof(text.text), "%s", g_overlayText);
        text.x = 18.0f;
        text.y = 18.0f;
        text.textSize = 1.8f;

        diagnostics::DebugOverlayPublicationV1 publication{};
        publication.textCount = 1;
        publication.textEntries = &text;
        publication.worldGeneration = snapshot.worldGeneration;
        publication.skeletonGeneration = snapshot.skeletonGeneration;
        publication.providerGeneration = snapshot.providerGeneration;
        publication.leaseFrames = 2;
        (void)g_diagnostics->publishDebugOverlayV1(ownerToken, &publication);
    }

    bool connect(rock::api::Client& client, rock::api::QueryInterfaceV1) noexcept
    {
        constexpr auto read = static_cast<std::uint32_t>(collision::PermissionV1::Read);
        if (client.acquire(read, g_collision) != rock::api::Status::Ok) {
            return false;
        }
        // The overlay is a convenience; touches are still logged without it.
        constexpr auto readWrite = static_cast<std::uint32_t>(diagnostics::PermissionV1::Read) |
                                   static_cast<std::uint32_t>(diagnostics::PermissionV1::Write);
        if (client.acquire(readWrite, g_diagnostics) != rock::api::Status::Ok) {
            g_diagnostics = nullptr;
            npctouch::runtime::logWarning("ROCK diagnostics overlay unavailable; logging only");
        }
        // Hands feeds the proximity fallback; without it only ROCK contacts are used.
        if (client.acquire(static_cast<std::uint32_t>(hands::PermissionV1::Read), g_hands) != rock::api::Status::Ok) {
            g_hands = nullptr;
            npctouch::runtime::logWarning("ROCK hands interface unavailable; proximity touch detection disabled");
        }
        // Input gives the grip button for grabbing NPCs.
        if (client.acquire(static_cast<std::uint32_t>(input::PermissionV1::Read), g_input) != rock::api::Status::Ok) {
            g_input = nullptr;
            npctouch::runtime::logWarning("ROCK input interface unavailable; grabbing NPCs disabled");
        } else {
            npctouch::runtime::logInfo("ROCK input interface ready; grip + touch grabs NPCs");
        }
        return true;
    }

    bool start(const std::uint64_t) noexcept
    {
        g_lastEventSequence = 0;
        g_lostEventCount = 0;
        g_otherTouchCount = 0;
        g_lastSummary = Clock::now();
        g_overlayUntil = {};
        npctouch::runtime::logInfo("Listening for hand touches on NPCs (build " NPCTOUCH_BUILD ")");
        static bool reactionsReady = false;
        if (!reactionsReady) {
            npctouch::reactions::initialize();
            npctouch::collision::initialize();
            npctouch::proxies::initialize();
            proximity::loadGrabSettings();
            reactionsReady = true;
        }
        return true;
    }

    void stop(const std::uint64_t ownerToken) noexcept
    {
        if (g_diagnostics) {
            (void)g_diagnostics->clearDebugOverlayV1(ownerToken);
        }
        g_lastEventSequence = 0;
        g_lostEventCount = 0;
    }

    void frame(
        const std::uint64_t ownerToken,
        const rock::api::core::SnapshotV1& snapshot) noexcept
    {
        pollTouchEvents(ownerToken);
        pollFingerContacts(ownerToken, rock::api::Hand::Right);
        pollFingerContacts(ownerToken, rock::api::Hand::Left);
        proximity::update(ownerToken);
        npctouch::reactions::update();
        const bool worldAvailable = npctouch::runtime::hasLifecycleFlag(
            snapshot.lifecycleFlags,
            rock::api::core::LifecycleFlag::WorldAvailable);
        npctouch::collision::update(worldAvailable);
        npctouch::proxies::update(worldAvailable);
        logSummary();
        publishOverlay(ownerToken, snapshot);
    }
}

namespace npctouch::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "NpcTouch",
            .pluginVersion = NPCTOUCH_VERSION,
            .onConnect = &connect,
            .onStart = &start,
            .onStop = &stop,
            .onFrame = &frame,
        };
        return value;
    }
}
