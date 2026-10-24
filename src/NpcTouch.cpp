// NpcTouch, step 1: log every time the player's hands touch an NPC.
// This build also logs non-NPC touches to diagnose what ROCK reports.
//
// ROCK owns hand physics. This plugin only reads ROCK's collision events and
// contact snapshots, resolves the touched reference, and reports NPC touches
// to the log and to a short in-headset debug overlay.

#include <ROCK/Collision.h>
#include <ROCK/Diagnostics.h>
#include "Runtime.h"

#include <RE/Fallout.h>

#include <array>
#include <chrono>
#include <cstdio>

namespace
{
    using Clock = std::chrono::steady_clock;
    using npctouch::runtime::hasLifecycleFlag;
    namespace collision = rock::api::collision;
    namespace diagnostics = rock::api::diagnostics;

    constexpr auto kOverlayDuration = std::chrono::seconds(3);
    constexpr auto kSummaryInterval = std::chrono::seconds(10);

    const collision::ApiV1* g_collision{};
    const diagnostics::ApiV1* g_diagnostics{};

    std::uint64_t g_lastEventSequence{ 0 };
    std::uint64_t g_npcTouchCount{ 0 };
    std::uint64_t g_otherTouchCount{ 0 };
    std::uint64_t g_contactBeginCount{ 0 };
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
            if (g_collision->copyEvents(
                    ownerToken,
                    g_lastEventSequence,
                    events.data(),
                    static_cast<std::uint32_t>(events.size()),
                    &stream) != rock::api::Status::Ok) {
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
        if (g_collision->copySemanticHandContactsV1(
                ownerToken,
                hand,
                0,
                contacts.data(),
                static_cast<std::uint32_t>(contacts.size()),
                &contactCount) != rock::api::Status::Ok) {
            return;
        }

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
        return true;
    }

    bool start(const std::uint64_t) noexcept
    {
        g_lastEventSequence = 0;
        g_lostEventCount = 0;
        g_otherTouchCount = 0;
        g_lastSummary = Clock::now();
        g_overlayUntil = {};
        npctouch::runtime::logInfo("Listening for hand touches on NPCs");
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
