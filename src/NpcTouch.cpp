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
            std::uint32_t gripStatusLogged{ 0 };
        };

        // OpenVR button id of the grip.
        constexpr std::uint32_t kGripButton = 2;
        // The hand may be this far from the body (beyond a touch) to grab.
        constexpr float kGrabReach = 10.0f;
        // The grip slips when the hand gets this much further from the joint
        // than the grabbed point is.
        constexpr float kGrabBreak = 40.0f;

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
                "GRAB END: %s hand let go of %08X (%s), posed %u frames, pose kept by game %u",
                handName(hand),
                state.grabbedFormId,
                reason,
                state.posedFrames,
                state.keptFrames);
            npctouch::runtime::logInfo(message);
            state.grabbedFormId = 0;
            state.grabbedBone = nullptr;
            state.grabJoint = nullptr;
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

        // Rotation (v' = R v) that turns unit vector a onto unit vector b.
        RE::NiMatrix3 rotationBetween(const RE::NiPoint3& a, const RE::NiPoint3& b, float& angle) noexcept
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
            const float t = 1.0f - cosine;
            const float x = axis.x, y = axis.y, z = axis.z;
            r.entry[0][0] = cosine + x * x * t;
            r.entry[0][1] = x * y * t - z * sine;
            r.entry[0][2] = x * z * t + y * sine;
            r.entry[1][0] = y * x * t + z * sine;
            r.entry[1][1] = cosine + y * y * t;
            r.entry[1][2] = y * z * t - x * sine;
            r.entry[2][0] = z * x * t - y * sine;
            r.entry[2][1] = z * y * t + x * sine;
            r.entry[2][2] = cosine + z * z * t;
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

        // Turns the joint so the grabbed point sits in the hand.
        void poseGrab(HandState& state, RE::NiAVObject* joint) noexcept
        {
            auto* parent = joint->parent;
            if (!parent) {
                return;
            }
            const RE::NiPoint3 current = joint->world.rotate.Transpose() * state.grabLocal * joint->world.scale;
            const RE::NiPoint3 wanted = state.position - joint->world.translate;
            const float currentLength = length(current);
            const float wantedLength = length(wanted);
            if (currentLength < 1.0f || wantedLength < 1.0f) {
                return;
            }
            float angle{ 0.0f };
            const auto turn = rotationBetween(current * (1.0f / currentLength), wanted * (1.0f / wantedLength), angle);
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
            turnChildren(joint, turn, turnStored, joint->world.translate);
            turnTwistSiblings(joint, turn, turnStored);
            state.lastJointWorld = joint->world;
            if (state.skinBone) {
                state.lastSkinWorld = state.skinBone->world;
            }
        }

        void updateGrab(const rock::api::Hand hand, HandState& state, const Hit& hit, const bool grip) noexcept
        {
            const bool pressed = grip && !state.gripWasHeld;
            state.gripWasHeld = grip;

            if (state.grabbedFormId != 0) {
                auto* actor = RE::TESForm::GetFormByID<RE::Actor>(state.grabbedFormId);
                auto* root = actor ? actor->Get3D() : nullptr;
                if (!grip) {
                    endGrab(hand, state, "released");
                    return;
                }
                if (!root || actor->IsDead(false) || root != state.grabRoot) {
                    endGrab(hand, state, "NPC gone");
                    return;
                }
                if (state.grabJoint) {
                    const RE::NiPoint3 reach = state.position - state.grabJoint->world.translate;
                    if (length(reach) > state.grabReach + kGrabBreak) {
                        endGrab(hand, state, "slipped");
                        return;
                    }
                    poseGrab(state, state.grabJoint);
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
            state.skinBone = nullptr;
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
            }
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
