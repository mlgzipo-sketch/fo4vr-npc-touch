// NpcTouch, step 2: log every time the player's hands touch an NPC.
// This build also logs non-NPC touches to diagnose what ROCK reports.
//
// ROCK owns hand physics. This plugin only reads ROCK's collision events and
// contact snapshots, resolves the touched reference, and reports NPC touches
// to the log and to a short in-headset debug overlay.

#include <ROCK/Collision.h>
#include <ROCK/Diagnostics.h>
#include <ROCK/Hands.h>
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

    constexpr auto kOverlayDuration = std::chrono::seconds(3);
    constexpr auto kSummaryInterval = std::chrono::seconds(10);

    const collision::ApiV1* g_collision{};
    const diagnostics::ApiV1* g_diagnostics{};
    const hands::ApiV1* g_hands{};

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
        std::uint32_t handStatus{ 0xFFFFFFFF };
        std::uint32_t handFlags{ 0 };
        float handPosition[3]{};
        std::uint32_t nearbyActors{ 0 };
        float nearestGap{ -1.0f };
        char nearestBone[32]{};
        char nearestName[48]{};
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

    void showOverlay(const char* text) noexcept
    {
        std::snprintf(g_overlayText, sizeof(g_overlayText), "%s", text);
        g_overlayUntil = Clock::now() + kOverlayDuration;
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
        constexpr float kReleaseMargin = 4.0f;

        struct HandState
        {
            bool valid{ false };
            RE::NiPoint3 position{};
            RE::NiPoint3 previous{};
            Clock::time_point previousTime{};
            float speed{ 0.0f };
            std::uint32_t touchedFormId{ 0 };
            const char* touchedBone{ nullptr };
        };

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
            if (hand == rock::api::Hand::Right) {
                g_diag.handStatus = static_cast<std::uint32_t>(status);
                g_diag.handFlags = frame.flags;
                for (int axis = 0; axis < 3; ++axis) {
                    g_diag.handPosition[axis] = frame.transform.translate[axis];
                }
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
                if (index == 0) {
                    g_diag.nearestGap = hit.actor ? hit.gap : -1.0f;
                    std::snprintf(g_diag.nearestBone, sizeof(g_diag.nearestBone), "%s", hit.bone ? hit.bone : "-");
                    std::snprintf(g_diag.nearestName, sizeof(g_diag.nearestName), "%s", hit.actor ? actorName(hit.actor) : "-");
                }
                const bool inside = hit.actor && hit.gap <= 0.0f;
                const bool released = !hit.actor || hit.gap > kReleaseMargin ||
                                      (state.touchedFormId != 0 && hit.actor->GetFormID() != state.touchedFormId);

                if (state.touchedFormId == 0 && inside) {
                    state.touchedFormId = hit.actor->GetFormID();
                    state.touchedBone = hit.bone;
                    ++g_npcTouchCount;
                    char message[256]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "PROXIMITY TOUCH BEGIN: %s hand -> NPC '%s' form=%08X bone=%s handSpeed=%.0f u/s",
                        handName(handIds[index]),
                        actorName(hit.actor),
                        state.touchedFormId,
                        hit.bone,
                        state.speed);
                    npctouch::runtime::logInfo(message);
                    char overlay[128]{};
                    std::snprintf(overlay, sizeof(overlay), "NpcTouch: %s hand -> %s (%s)", handName(handIds[index]), actorName(hit.actor), hit.bone);
                    showOverlay(overlay);
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
        char detail[384]{};
        std::snprintf(
            detail,
            sizeof(detail),
            "  status: events=%u contacts=%u (seen %u) hands=%s handFrame=%u flags=%X rightHand=(%.0f, %.0f, %.0f) nearbyNPCs=%u nearest='%s' bone=%s gap=%.0f",
            g_diag.eventsStatus,
            g_diag.contactsStatus,
            g_diag.contactsSeen,
            g_hands ? "yes" : "no",
            g_diag.handStatus,
            g_diag.handFlags,
            g_diag.handPosition[0],
            g_diag.handPosition[1],
            g_diag.handPosition[2],
            g_diag.nearbyActors,
            g_diag.nearestName,
            g_diag.nearestBone,
            g_diag.nearestGap);
        npctouch::runtime::logInfo(detail);
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
