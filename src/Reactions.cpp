// NpcTouch, step 3: NPCs react to being touched.
//
// The reaction depends on who is touched and where:
//   - companions only look at the player and comment with their own lines;
//   - other NPCs grow annoyed, faster for the head and much faster for the
//     chest and pelvis, and once annoyed enough may start a fight (ini).
// Lines come from the game's own "hit" dialogue topics in Fallout4.esm, so
// every voice type already has recordings and no plugin is needed.

#include "Reactions.h"
#include "Runtime.h"

#include <RE/Fallout.h>
#include <RE/Bethesda/SendPapyrusEvent.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace npctouch::reactions
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Settings
        {
            bool enabled{ true };
            bool allowCombat{ false };
            bool companionsSpeak{ true };
            float annoyanceForWarning{ 3.0f };
            float annoyanceForCombat{ 8.0f };
            float annoyanceDecayPerMinute{ 4.0f };
            float speechCooldownSeconds{ 4.0f };
            float lookAtSeconds{ 3.0f };
        } g_settings{};

        bool g_papyrusAvailable{ false };

        // DialogueGeneric "Hit" topic: short "Hey!" style lines for all voices.
        constexpr std::uint32_t kGenericHitTopic = 0x000219CC;

        struct Companion
        {
            std::uint32_t npcBase;
            std::uint32_t hitTopic;
        };

        // Base NPC records and the "Hit" topic in each companion's COM quest.
        constexpr std::array kCompanions{
            Companion{ 0x00002F1E, 0x00162C69 },  // Piper
            Companion{ 0x000179FF, 0x000F0EB0 },  // Codsworth
            Companion{ 0x00027683, 0x0010E26A },  // Danse
            Companion{ 0x00002F24, 0x001576FF },  // Nick Valentine
            Companion{ 0x00027686, 0x0010BA55 },  // Curie
            Companion{ 0x00045AC9, 0x000F949F },  // Deacon
            Companion{ 0x0002740E, 0x0011B648 },  // MacCready
            Companion{ 0x00022613, 0x0012605C },  // Hancock
            Companion{ 0x00079249, 0 },           // Cait
            Companion{ 0x00027682, 0 },           // Strong
        };

        enum class Zone
        {
            Neutral,
            Head,
            Intimate,
        };

        struct NpcState
        {
            float annoyance{ 0.0f };
            Clock::time_point lastUpdate{};
            Clock::time_point lastSpeech{};
            Clock::time_point lookUntil{};
            bool looking{ false };
            bool warned{ false };
            bool fighting{ false };
        };

        std::unordered_map<std::uint32_t, NpcState> g_states;

        // ---------------------------------------------------------------- ini

        std::string trim(std::string value)
        {
            const auto first = value.find_first_not_of(" \t\r\n");
            const auto last = value.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string{} : value.substr(first, last - first + 1);
        }

        void loadSettings() noexcept
        {
            try {
                std::ifstream file("Data/F4SE/Plugins/NpcTouch.ini");
                if (!file) {
                    runtime::logInfo("No NpcTouch.ini found; using default reaction settings");
                    return;
                }
                std::string line;
                while (std::getline(file, line)) {
                    line = trim(line);
                    if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') {
                        continue;
                    }
                    const auto equals = line.find('=');
                    if (equals == std::string::npos) {
                        continue;
                    }
                    const auto key = trim(line.substr(0, equals));
                    const auto value = trim(line.substr(equals + 1));
                    const float number = std::strtof(value.c_str(), nullptr);
                    const bool flag = number != 0.0f;
                    if (key == "bEnableReactions") {
                        g_settings.enabled = flag;
                    } else if (key == "bAllowCombat") {
                        g_settings.allowCombat = flag;
                    } else if (key == "bCompanionsSpeak") {
                        g_settings.companionsSpeak = flag;
                    } else if (key == "fAnnoyanceForWarning") {
                        g_settings.annoyanceForWarning = number;
                    } else if (key == "fAnnoyanceForCombat") {
                        g_settings.annoyanceForCombat = number;
                    } else if (key == "fAnnoyanceDecayPerMinute") {
                        g_settings.annoyanceDecayPerMinute = number;
                    } else if (key == "fSpeechCooldownSeconds") {
                        g_settings.speechCooldownSeconds = number;
                    } else if (key == "fLookAtSeconds") {
                        g_settings.lookAtSeconds = number;
                    }
                }
            } catch (...) {
                runtime::logWarning("Failed to read NpcTouch.ini; using default reaction settings");
            }

            char message[256]{};
            std::snprintf(
                message,
                sizeof(message),
                "Reactions: enabled=%d combat=%d companionsSpeak=%d warning=%.1f combatAt=%.1f decay=%.1f/min cooldown=%.1fs",
                g_settings.enabled,
                g_settings.allowCombat,
                g_settings.companionsSpeak,
                g_settings.annoyanceForWarning,
                g_settings.annoyanceForCombat,
                g_settings.annoyanceDecayPerMinute,
                g_settings.speechCooldownSeconds);
            runtime::logInfo(message);
        }

        // A missing address-library entry would stop the game with an error
        // box, so check the ones the Papyrus calls rely on before using them.
        bool addressLibraryHas(const std::array<std::uint64_t, 2>& ids) noexcept
        {
            try {
                std::ifstream file("Data/F4SE/Plugins/version-1-2-72-0.csv");
                if (!file) {
                    return false;
                }
                std::unordered_set<std::uint64_t> missing(ids.begin(), ids.end());
                std::string line;
                while (!missing.empty() && std::getline(file, line)) {
                    const auto id = std::strtoull(line.c_str(), nullptr, 10);
                    missing.erase(id);
                }
                return missing.empty();
            } catch (...) {
                return false;
            }
        }

        // ------------------------------------------------------------ papyrus

        RE::BSScript::IVirtualMachine* virtualMachine() noexcept
        {
            auto* game = RE::GameVM::GetSingleton();
            return game ? game->GetVM().get() : nullptr;
        }

        // The Papyrus calls go through reverse-engineered VM entry points, and
        // the first VR test faulted inside the VM. Run each step under SEH so a
        // fault only switches the reactions off and names the step in the log.
        // Papyrus functions that faulted once; they are not called again.
        std::unordered_set<std::string> g_brokenFunctions;

        bool runGuarded(void (*step)(void*), void* context) noexcept
        {
            __try {
                step(context);
                return true;
            } __except (1) {
                return false;
            }
        }

        template <class F>
        bool guarded(const char* function, const char* stepName, F& step) noexcept
        {
            if (runGuarded([](void* context) { (*static_cast<F*>(context))(); }, &step)) {
                return true;
            }
            g_brokenFunctions.insert(function);
            char message[160]{};
            std::snprintf(
                message,
                sizeof(message),
                "Papyrus %s faulted at step '%s'; Actor.%s is now off, other calls continue",
                function,
                stepName,
                function);
            runtime::logWarning(message);
            return false;
        }

        std::uint32_t g_loggedCalls{ 0 };

        // Calls a native Papyrus method on the actor, e.g. Actor.Say(...).
        template <class... Args>
        bool callActor(RE::Actor* actor, const char* function, Args... args) noexcept
        {
            RE::BSScript::IVirtualMachine* vm{ nullptr };
            auto findVm = [&]() { vm = virtualMachine(); };
            if (!g_papyrusAvailable || !actor || g_brokenFunctions.contains(function) ||
                !guarded(function, "vm", findVm) || !vm) {
                return false;
            }

            // Packing the actor binds a script object to it if none exists yet.
            auto bind = [&]() {
                RE::BSScript::Variable self;
                RE::BSScript::PackVariable(self, static_cast<const RE::Actor*>(actor));
            };
            if (!guarded(function, "bind", bind)) {
                return false;
            }

            std::size_t handle{ 0 };
            bool hasHandle{ false };
            auto findHandle = [&]() {
                const auto& policy = vm->GetObjectHandlePolicy();
                handle = policy.GetHandleForObject(static_cast<std::uint32_t>(RE::Actor::FORM_ID), actor);
                hasHandle = handle != policy.EmptyHandle();
            };
            if (!guarded(function, "handle", findHandle) || !hasHandle) {
                return false;
            }

            // Pack each argument on its own first so a fault names the argument.
            bool packed{ true };
            int argument{ 0 };
            (
                [&]() {
                    if (!packed) {
                        return;
                    }
                    ++argument;
                    auto pack = [&]() { (void)RE::BSScript::detail::PackVariables(args); };
                    char stepName[32]{};
                    std::snprintf(stepName, sizeof(stepName), "pack argument %d", argument);
                    packed = guarded(function, stepName, pack);
                }(),
                ...);
            if (!packed) {
                return false;
            }

            bool dispatched{ false };
            auto dispatch = [&]() {
                dispatched = Papyrus::detail::DispatchMethodCall(vm, handle, "Actor", function, nullptr, args...);
            };
            if (!guarded(function, "dispatch", dispatch)) {
                return false;
            }
            if (g_loggedCalls < 20) {
                ++g_loggedCalls;
                char message[128]{};
                std::snprintf(message, sizeof(message), "Papyrus Actor.%s queued=%d", function, dispatched);
                runtime::logInfo(message);
            }
            return dispatched;
        }

        // Topics have no script attached, so packing one makes the library
        // create and bind a new script object through ObjectBindPolicy, which
        // faulted in VR. Bind it through the VM's virtual bind interface
        // instead, the same path the game uses.
        RE::BSTSmartPointer<RE::BSScript::Object> topicObject(RE::TESTopic* topic) noexcept
        {
            RE::BSTSmartPointer<RE::BSScript::Object> object;
            auto* vm = virtualMachine();
            if (!vm || !topic || g_brokenFunctions.contains("TopicObject")) {
                return object;
            }
            constexpr auto type = static_cast<std::uint32_t>(RE::TESTopic::FORM_ID);
            RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
            bool ok{ false };
            auto findType = [&]() { ok = vm->GetScriptObjectType(type, typeInfo) && typeInfo; };
            if (!guarded("TopicObject", "type info", findType) || !ok) {
                return object;
            }
            std::size_t handle{ 0 };
            auto findHandle = [&]() {
                const auto& policy = vm->GetObjectHandlePolicy();
                handle = policy.GetHandleForObject(type, topic);
                ok = handle != policy.EmptyHandle();
            };
            if (!guarded("TopicObject", "handle", findHandle) || !ok) {
                return object;
            }
            auto find = [&]() { ok = vm->FindBoundObject(handle, typeInfo->name.c_str(), false, object, false) && object; };
            if (!guarded("TopicObject", "find bound", find)) {
                return nullptr;
            }
            if (ok) {
                return object;
            }
            // IVirtualMachine::CreateObject has two overloads, and MSVC puts
            // overloaded virtuals in the opposite slot order, so that call
            // landed on the wrong game function and faulted. The bind
            // interface has a single create function, which is what the
            // engine itself uses before binding.
            auto* internal = static_cast<RE::BSScript::Internal::VirtualMachine*>(vm);
            auto* binder = static_cast<RE::BSScript::IVMObjectBindInterface*>(internal);
            auto create = [&]() { ok = binder->CreateObjectWithProperties(typeInfo->name, 0, object) && object; };
            if (g_brokenFunctions.contains("TopicCreate") || !guarded("TopicCreate", "create with properties", create)) {
                // Last resort: call the no-properties CreateObject by its game slot.
                using CreateFn = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSFixedString&, RE::BSTSmartPointer<RE::BSScript::Object>&);
                auto createBySlot = [&]() {
                    auto** table = *reinterpret_cast<void***>(vm);
                    ok = reinterpret_cast<CreateFn>(table[0x17])(vm, typeInfo->name, object) && object;
                };
                if (!guarded("TopicObject", "create by slot 0x17", createBySlot)) {
                    return nullptr;
                }
            }
            if (!ok) {
                runtime::logWarning("Papyrus could not create a script object for the topic");
                g_brokenFunctions.insert("TopicObject");
                return nullptr;
            }
            auto bind = [&]() { binder->BindObject(object, handle, false); };
            if (!guarded("TopicObject", "bind", bind)) {
                return nullptr;
            }
            char message[160]{};
            std::snprintf(message, sizeof(message), "Topic %08X bound to a new script object (handle %llX)", topic->GetFormID(), static_cast<unsigned long long>(handle));
            runtime::logInfo(message);
            return object;
        }

        bool say(RE::Actor* actor, const std::uint32_t topicId) noexcept
        {
            auto* topic = RE::TESForm::GetFormByID<RE::TESTopic>(topicId);
            if (!topic) {
                return false;
            }
            auto object = topicObject(topic);
            if (!object) {
                return false;
            }
            return callActor(
                actor,
                "Say",
                object,
                static_cast<RE::Actor*>(nullptr),
                false,
                static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton()));
        }

        // -------------------------------------------------------------- logic

        const Companion* findCompanion(RE::Actor* actor) noexcept
        {
            const auto* base = actor->GetObjectReference();
            if (!base) {
                return nullptr;
            }
            const auto baseId = base->GetFormID();
            for (const auto& companion : kCompanions) {
                if (companion.npcBase == baseId) {
                    return &companion;
                }
            }
            return nullptr;
        }

        Zone zoneOf(const char* bone) noexcept
        {
            if (!bone) {
                return Zone::Neutral;
            }
            if (std::strcmp(bone, "Chest") == 0 || std::strcmp(bone, "Pelvis") == 0) {
                return Zone::Intimate;
            }
            if (std::strcmp(bone, "Head") == 0 || std::strcmp(bone, "Neck") == 0) {
                return Zone::Head;
            }
            return Zone::Neutral;
        }

        const char* zoneName(const Zone zone) noexcept
        {
            switch (zone) {
            case Zone::Head:
                return "head";
            case Zone::Intimate:
                return "intimate";
            default:
                return "neutral";
            }
        }

        float annoyanceFor(const Zone zone) noexcept
        {
            switch (zone) {
            case Zone::Head:
                return 2.0f;
            case Zone::Intimate:
                return 4.0f;
            default:
                return 1.0f;
            }
        }

        void decay(NpcState& state, const Clock::time_point now) noexcept
        {
            if (state.lastUpdate != Clock::time_point{}) {
                const float minutes = std::chrono::duration<float>(now - state.lastUpdate).count() / 60.0f;
                state.annoyance = std::max(0.0f, state.annoyance - minutes * g_settings.annoyanceDecayPerMinute);
                if (state.annoyance < g_settings.annoyanceForWarning) {
                    state.warned = false;
                }
            }
            state.lastUpdate = now;
        }

        void lookAtPlayer(RE::Actor* actor, NpcState& state, const Clock::time_point now) noexcept
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (callActor(actor, "SetLookAt", static_cast<RE::TESObjectREFR*>(player), false)) {
                state.looking = true;
                state.lookUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.lookAtSeconds * 1000.0f));
            }
        }

        const char* actorName(RE::Actor* actor) noexcept
        {
            const char* name = actor->GetDisplayFullName();
            return name && name[0] ? name : "<no name>";
        }
    }

    void initialize() noexcept
    {
        loadSettings();
        g_papyrusAvailable = addressLibraryHas({ 996227, 614600 });
        if (!g_papyrusAvailable) {
            runtime::logWarning("Address library lacks the Papyrus VM entries; NPCs will not speak or turn");
        }
    }

    void onTouch(RE::Actor* npc, const char* bone) noexcept
    {
        if (!g_settings.enabled || !npc) {
            return;
        }
        const auto now = Clock::now();
        auto& state = g_states[npc->GetFormID()];
        decay(state, now);

        const Zone zone = zoneOf(bone);
        const auto* companion = findCompanion(npc);
        const bool teammate = RE::IsPlayerTeammate(*npc);
        const bool friendly = companion || teammate;

        lookAtPlayer(npc, state, now);

        const bool canSpeak = now - state.lastSpeech >=
                              std::chrono::milliseconds(static_cast<int>(g_settings.speechCooldownSeconds * 1000.0f));
        const char* action = "look";

        if (friendly) {
            // Companions are never angered by touches.
            if (g_settings.companionsSpeak && canSpeak) {
                const auto topic = companion && companion->hitTopic ? companion->hitTopic : kGenericHitTopic;
                if (say(npc, topic)) {
                    state.lastSpeech = now;
                    action = "companion line";
                }
            }
        } else {
            state.annoyance += annoyanceFor(zone);
            if (g_settings.allowCombat && !state.fighting && state.annoyance >= g_settings.annoyanceForCombat) {
                if (callActor(npc, "StartCombat", static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton()), false)) {
                    state.fighting = true;
                    action = "start combat";
                }
            } else if (canSpeak || (!state.warned && state.annoyance >= g_settings.annoyanceForWarning)) {
                if (say(npc, kGenericHitTopic)) {
                    state.lastSpeech = now;
                    action = state.annoyance >= g_settings.annoyanceForWarning ? "warning line" : "line";
                    state.warned = state.annoyance >= g_settings.annoyanceForWarning;
                }
            }
        }

        char message[256]{};
        std::snprintf(
            message,
            sizeof(message),
            "REACTION: '%s' %s zone=%s annoyance=%.1f -> %s",
            actorName(npc),
            friendly ? "companion" : "npc",
            zoneName(zone),
            state.annoyance,
            action);
        runtime::logInfo(message);
    }

    void update() noexcept
    {
        if (g_states.empty()) {
            return;
        }
        const auto now = Clock::now();
        for (auto it = g_states.begin(); it != g_states.end();) {
            auto& state = it->second;
            if (state.looking && now >= state.lookUntil) {
                state.looking = false;
                if (auto* actor = RE::TESForm::GetFormByID<RE::Actor>(it->first)) {
                    (void)callActor(actor, "ClearLookAt");
                }
            }
            decay(state, now);
            if (!state.looking && state.annoyance <= 0.0f && !state.fighting &&
                now - state.lastSpeech > std::chrono::minutes(5)) {
                it = g_states.erase(it);
            } else {
                ++it;
            }
        }
    }
}
