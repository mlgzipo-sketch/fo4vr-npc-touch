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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <random>
#include <unordered_set>
#include <vector>

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
            // Topics a regular NPC picks from at random, all voiced by the
            // common voices: DialogueGeneric "Normal to alert" ("Хм?", "Что?",
            // "Чего?", "Эй..."), DialogueGenericCitizens "Hello" ("А?", "Да?")
            // and DialogueGeneric "Hit" ("Уф!", "А!").
            std::vector<std::uint32_t> lineTopics{ 0x0003D77F, 0x000D101E, 0x000219CC };
            // DialogueGenericCitizens "Accused you" ("Больше так не делай.") and
            // "Pickpocket caught" ("Я все вижу!", "Чертовы карманники!").
            std::vector<std::uint32_t> warningTopics{ 0x000D14A6, 0x000711BB };
            // Companion affinity from which chest/pelvis touches count as welcome
            // (1000 is the game's "Idolizes" level). Romanced companions always do.
            float closeAffinity{ 1000.0f };
            // A hand moving at least this fast (game units/s) shoves the NPC.
            bool shoves{ true };
            float shoveSpeed{ 75.0f };
            // Faster still knocks the NPC over (ragdoll), if allowed.
            bool knockdowns{ false };
            float knockdownSpeed{ 180.0f };
            float knockdownForce{ 1.0f };
            // How far NPCs step away when the player bumps into them; the game
            // uses 75-128. Negative keeps the game's values.
            float bumpMoveDistance{ 0.0f };
            // While an NPC is held, annoyance grows by this much per second
            // (twice on the head, three times on chest/pelvis). Once it
            // reaches breakFreeAnnoyance the NPC pulls free. Companions never mind.
            bool grabReactions{ true };
            float grabAnnoyancePerSecond{ 0.6f };
            float breakFreeAnnoyance{ 6.0f };
            // Even an annoyed NPC lets itself be held at least this long.
            float breakFreeMinSeconds{ 3.0f };
        } g_settings{};

        bool g_papyrusAvailable{ false };

        struct Companion
        {
            std::uint32_t npcBase;
            std::uint32_t hitTopic;
            std::uint32_t helloTopic;
        };

        // Base NPC records with the "Hit" and "Hello" topics of each companion's
        // COM quest. The game picks Hello lines by affinity and romance itself.
        constexpr std::array kCompanions{
            Companion{ 0x00002F1E, 0x00162C69, 0x00162C56 },  // Piper
            Companion{ 0x000179FF, 0, 0x000F1248 },           // Codsworth
            Companion{ 0x00027683, 0x0010E26A, 0x0010E271 },  // Danse
            Companion{ 0x00002F24, 0x001576FF, 0x001576EC },  // Nick Valentine
            Companion{ 0x00027686, 0x0010BA55, 0x0010BA40 },  // Curie
            Companion{ 0x00045AC9, 0x000F949F, 0x000F7600 },  // Deacon
            Companion{ 0x0002740E, 0x0011B648, 0x0011CD65 },  // MacCready
            Companion{ 0x00022613, 0x0012605C, 0x00126092 },  // Hancock
            Companion{ 0x00079249, 0x000F450B, 0x0007921C },  // Cait
            Companion{ 0x00027682, 0, 0x0011F765 },           // Strong
            Companion{ 0x00019FD9, 0, 0x0007925D },           // Preston Garvey
            Companion{ 0x000BBEE6, 0x0012B8A7, 0x0011CE29 },  // X6-88
        };

        // Human stagger idles (CharacterMT*): small and medium, forward and back.
        constexpr std::uint32_t kStaggerForwardSmall = 0x00059A94;
        constexpr std::uint32_t kStaggerForwardMedium = 0x00059A93;
        constexpr std::uint32_t kStaggerBackSmall = 0x00059A97;
        constexpr std::uint32_t kStaggerBackMedium = 0x00059A96;

        constexpr std::uint32_t kAffinityValue = 0x000A1B80;  // CA_Affinity
        constexpr std::uint32_t kRomanticValue = 0x00148DF6;  // CA_IsRomantic

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

        // "000219CC, 000711BB" -> form IDs; zeros and junk are skipped.
        std::vector<std::uint32_t> parseTopics(const std::string& value)
        {
            std::vector<std::uint32_t> topics;
            const char* text = value.c_str();
            while (*text) {
                char* end{ nullptr };
                const auto id = std::strtoul(text, &end, 16);
                if (end == text) {
                    ++text;
                    continue;
                }
                if (id != 0) {
                    topics.push_back(static_cast<std::uint32_t>(id));
                }
                text = end;
            }
            return topics;
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
                    if (key == "bGrabReactions") {
                        g_settings.grabReactions = flag;
                    } else if (key == "fGrabAnnoyancePerSecond") {
                        g_settings.grabAnnoyancePerSecond = number;
                    } else if (key == "fBreakFreeMinSeconds") {
                        g_settings.breakFreeMinSeconds = number;
                    } else if (key == "fBreakFreeAnnoyance") {
                        g_settings.breakFreeAnnoyance = number;
                    } else if (key == "fBumpMoveDistance") {
                        g_settings.bumpMoveDistance = number;
                    } else if (key == "bEnableReactions") {
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
                    } else if (key == "sLineTopics") {
                        g_settings.lineTopics = parseTopics(value);
                    } else if (key == "sWarningTopics") {
                        g_settings.warningTopics = parseTopics(value);
                    } else if (key == "fCloseAffinity") {
                        g_settings.closeAffinity = number;
                    } else if (key == "bEnableShoves") {
                        g_settings.shoves = flag;
                    } else if (key == "fShoveSpeed") {
                        g_settings.shoveSpeed = number;
                    } else if (key == "bEnableKnockdowns") {
                        g_settings.knockdowns = flag;
                    } else if (key == "fKnockdownSpeed") {
                        g_settings.knockdownSpeed = number;
                    } else if (key == "fKnockdownForce") {
                        g_settings.knockdownForce = number;
                    }
                }
            } catch (...) {
                runtime::logWarning("Failed to read NpcTouch.ini; using default reaction settings");
            }

            char message[256]{};
            std::snprintf(
                message,
                sizeof(message),
                "Reactions: enabled=%d combat=%d companionsSpeak=%d warning=%.1f combatAt=%.1f decay=%.1f/min cooldown=%.1fs lines=%zu warningLines=%zu closeAffinity=%.0f",
                g_settings.enabled,
                g_settings.allowCombat,
                g_settings.companionsSpeak,
                g_settings.annoyanceForWarning,
                g_settings.annoyanceForCombat,
                g_settings.annoyanceDecayPerMinute,
                g_settings.speechCooldownSeconds,
                g_settings.lineTopics.size(),
                g_settings.warningTopics.size(),
                g_settings.closeAffinity);
            runtime::logNote(message);
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

        // NPCs the player walks into step aside (the game's bump reaction),
        // which in VR means they back away from hands reaching for them.
        void keepNPCsCloseOnBump() noexcept
        {
            if (g_settings.bumpMoveDistance < 0.0f) {
                return;
            }
            if (!addressLibraryHas({ 8308, 8308 })) {
                runtime::logWarning("Address library lacks the game settings entry; NPCs keep stepping away on bumps");
                return;
            }
            auto* settings = RE::GameSettingCollection::GetSingleton();
            if (!settings) {
                return;
            }
            for (const char* name : { "fBumpReactionIdealMoveDist", "fBumpReactionMinMoveDist" }) {
                auto* setting = settings->GetSetting(name);
                char message[160]{};
                if (setting) {
                    const float old = setting->GetFloat();
                    setting->SetFloat(g_settings.bumpMoveDistance);
                    std::snprintf(message, sizeof(message), "Game setting %s: %.0f -> %.0f", name, old, g_settings.bumpMoveDistance);
                } else {
                    std::snprintf(message, sizeof(message), "Game setting %s not found", name);
                }
                runtime::logInfo(message);
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

        // Topics and idles have no script attached, so packing one makes the
        // library create and bind a new script object through ObjectBindPolicy,
        // which faulted in VR. Bind it through the VM's virtual bind interface
        // instead, the same path the game uses.
        RE::BSTSmartPointer<RE::BSScript::Object> formObject(RE::TESForm* form) noexcept
        {
            RE::BSTSmartPointer<RE::BSScript::Object> object;
            auto* vm = virtualMachine();
            if (!vm || !form || g_brokenFunctions.contains("TopicObject")) {
                return object;
            }
            const auto type = static_cast<std::uint32_t>(form->GetFormType());
            RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> typeInfo;
            bool ok{ false };
            auto findType = [&]() { ok = vm->GetScriptObjectType(type, typeInfo) && typeInfo; };
            if (!guarded("TopicObject", "type info", findType) || !ok) {
                return object;
            }
            std::size_t handle{ 0 };
            auto findHandle = [&]() {
                const auto& policy = vm->GetObjectHandlePolicy();
                handle = policy.GetHandleForObject(type, form);
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
                runtime::logWarning("Papyrus could not create a script object for a form");
                g_brokenFunctions.insert("TopicObject");
                return nullptr;
            }
            auto bind = [&]() { binder->BindObject(object, handle, false); };
            if (!guarded("TopicObject", "bind", bind)) {
                return nullptr;
            }
            char message[160]{};
            std::snprintf(message, sizeof(message), "Form %08X bound to a new script object (handle %llX)", form->GetFormID(), static_cast<unsigned long long>(handle));
            runtime::logInfo(message);
            return object;
        }

        bool say(RE::Actor* actor, const std::uint32_t topicId) noexcept
        {
            auto* topic = RE::TESForm::GetFormByID<RE::TESTopic>(topicId);
            if (!topic) {
                return false;
            }
            auto object = formObject(topic);
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

        bool playIdle(RE::Actor* actor, const std::uint32_t idleId) noexcept
        {
            auto* idle = RE::TESForm::GetFormByID(idleId);
            if (!idle || idle->GetFormType() != RE::ENUM_FORM_ID::kIDLE) {
                return false;
            }
            auto object = formObject(idle);
            if (!object) {
                return false;
            }
            return callActor(actor, "PlayIdle", object);
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

        // Reads a game actor value such as CA_Affinity; 0 if it is unavailable.
        float actorValue(RE::Actor* actor, const std::uint32_t valueId) noexcept
        {
            auto* info = RE::TESForm::GetFormByID<RE::ActorValueInfo>(valueId);
            if (!actor || !info || g_brokenFunctions.contains("ActorValue")) {
                return 0.0f;
            }
            float value{ 0.0f };
            auto read = [&]() { value = static_cast<RE::ActorValueOwner*>(actor)->GetActorValue(*info); };
            if (!guarded("ActorValue", "read", read)) {
                return 0.0f;
            }
            return value;
        }

        std::uint32_t pickTopic(const std::vector<std::uint32_t>& topics) noexcept
        {
            if (topics.empty()) {
                return 0;
            }
            static std::minstd_rand random{ static_cast<std::uint32_t>(Clock::now().time_since_epoch().count()) };
            return topics[random() % topics.size()];
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
        keepNPCsCloseOnBump();
        g_papyrusAvailable = addressLibraryHas({ 996227, 614600 });
        if (!g_papyrusAvailable) {
            runtime::logWarning("Address library lacks the Papyrus VM entries; NPCs will not speak or turn");
        }
    }

    void onTouch(RE::Actor* npc, const char* bone, const float (&handVelocity)[3]) noexcept
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

        // A fast hand shoves the NPC: a stagger in the direction of the push,
        // or a knockdown for a hard hit. Companions are never knocked over.
        const char* push = "none";
        const float speed = std::sqrt(handVelocity[0] * handVelocity[0] + handVelocity[1] * handVelocity[1] + handVelocity[2] * handVelocity[2]);
        if (g_settings.shoves && !state.fighting && speed >= g_settings.shoveSpeed) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (g_settings.knockdowns && !friendly && speed >= g_settings.knockdownSpeed && player &&
                callActor(static_cast<RE::Actor*>(player), "PushActorAway", npc, g_settings.knockdownForce)) {
                push = "knockdown";
                state.annoyance += 4.0f;
            } else {
                // The NPC's facing on the ground plane; a push along it sends them forward.
                const float heading = npc->data.angle.z;
                const float along = handVelocity[0] * std::sin(heading) + handVelocity[1] * std::cos(heading);
                const bool medium = speed >= (g_settings.shoveSpeed + g_settings.knockdownSpeed) * 0.5f;
                const auto idle = along > 0.0f ? (medium ? kStaggerForwardMedium : kStaggerForwardSmall) :
                                                 (medium ? kStaggerBackMedium : kStaggerBackSmall);
                if (playIdle(npc, idle)) {
                    push = medium ? "stagger medium" : "stagger small";
                    if (!friendly) {
                        state.annoyance += 2.0f;
                    }
                }
            }
        }

        const bool canSpeak = now - state.lastSpeech >=
                              std::chrono::milliseconds(static_cast<int>(g_settings.speechCooldownSeconds * 1000.0f));
        const char* action = "look";
        std::uint32_t spoken{ 0 };
        float affinity{ 0.0f };
        bool romantic{ false };

        if (friendly) {
            // Companions are never angered by touches. A friendly touch gets a
            // hello; a chest/pelvis touch from someone they are not close to
            // gets a startled "hit" line instead.
            if (companion) {
                affinity = actorValue(npc, kAffinityValue);
                romantic = actorValue(npc, kRomanticValue) > 0.0f;
            }
            const bool close = romantic || affinity >= g_settings.closeAffinity;
            if (g_settings.companionsSpeak && canSpeak) {
                std::uint32_t topic{ 0 };
                if (companion) {
                    topic = zone == Zone::Intimate && !close && companion->hitTopic ? companion->hitTopic : companion->helloTopic;
                } else {
                    topic = pickTopic(g_settings.lineTopics);
                }
                if (topic && say(npc, topic)) {
                    state.lastSpeech = now;
                    spoken = topic;
                    action = companion && topic == companion->helloTopic ? "companion hello" : "companion line";
                }
            }
        } else {
            state.annoyance += annoyanceFor(zone);
            if (g_settings.allowCombat && !state.fighting && state.annoyance >= g_settings.annoyanceForCombat) {
                if (callActor(npc, "StartCombat", static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton()), false)) {
                    state.fighting = true;
                    action = "start combat";
                }
            } else if (canSpeak || (!state.warned && state.annoyance >= g_settings.annoyanceForWarning &&
                                       now - state.lastSpeech >= std::chrono::milliseconds(1500))) {
                const bool warning = state.annoyance >= g_settings.annoyanceForWarning;
                auto topic = warning ? pickTopic(g_settings.warningTopics) : 0;
                if (!topic) {
                    topic = pickTopic(g_settings.lineTopics);
                }
                if (topic && say(npc, topic)) {
                    state.lastSpeech = now;
                    spoken = topic;
                    action = warning ? "warning line" : "line";
                    state.warned = warning;
                }
            }
        }

        char message[256]{};
        std::snprintf(
            message,
            sizeof(message),
            "REACTION: '%s' %s zone=%s speed=%.0f push=%s annoyance=%.1f affinity=%.0f romantic=%d -> %s topic=%08X",
            actorName(npc),
            friendly ? "companion" : "npc",
            zoneName(zone),
            speed,
            push,
            state.annoyance,
            affinity,
            romantic,
            action,
            spoken);
        runtime::logInfo(message);
    }

    bool onGrabHeld(RE::Actor* npc, const char* bone, const float seconds, const float heldSeconds) noexcept
    {
        if (!g_settings.enabled || !g_settings.grabReactions || !npc || seconds <= 0.0f) {
            return false;
        }
        const auto now = Clock::now();
        auto& state = g_states[npc->GetFormID()];
        decay(state, now);
        lookAtPlayer(npc, state, now);

        const auto* companion = findCompanion(npc);
        if (companion || RE::IsPlayerTeammate(*npc)) {
            return false;
        }
        const Zone zone = zoneOf(bone);
        const float factor = zone == Zone::Intimate ? 3.0f : zone == Zone::Head ? 2.0f : 1.0f;
        state.annoyance += g_settings.grabAnnoyancePerSecond * factor * seconds;

        const char* action = "hold";
        std::uint32_t spoken{ 0 };
        bool breakFree = false;
        if (g_settings.allowCombat && !state.fighting && state.annoyance >= g_settings.annoyanceForCombat) {
            if (callActor(npc, "StartCombat", static_cast<RE::Actor*>(RE::PlayerCharacter::GetSingleton()), false)) {
                state.fighting = true;
                action = "start combat";
                breakFree = true;
            }
        } else if (state.annoyance >= g_settings.breakFreeAnnoyance && heldSeconds >= g_settings.breakFreeMinSeconds) {
            // Pull back out of the hand, with an angry line if one is due.
            breakFree = true;
            action = playIdle(npc, kStaggerBackSmall) ? "break free (stagger)" : "break free";
            const auto topic = pickTopic(g_settings.warningTopics);
            if (topic && now - state.lastSpeech >= std::chrono::milliseconds(1500) && say(npc, topic)) {
                state.lastSpeech = now;
                spoken = topic;
            }
            state.warned = true;
        } else {
            const bool canSpeak = now - state.lastSpeech >=
                                  std::chrono::milliseconds(static_cast<int>(g_settings.speechCooldownSeconds * 1000.0f));
            const bool warning = state.annoyance >= g_settings.annoyanceForWarning;
            if ((warning && !state.warned) || (canSpeak && warning)) {
                const auto topic = pickTopic(g_settings.warningTopics);
                if (topic && now - state.lastSpeech >= std::chrono::milliseconds(1500) && say(npc, topic)) {
                    state.lastSpeech = now;
                    spoken = topic;
                    action = "warning line";
                    state.warned = true;
                }
            }
        }

        if (spoken || breakFree) {
            char message[200]{};
            std::snprintf(
                message,
                sizeof(message),
                "GRAB REACTION: '%s' zone=%s annoyance=%.1f -> %s topic=%08X",
                actorName(npc),
                zoneName(zone),
                state.annoyance,
                action,
                spoken);
            runtime::logInfo(message);
        }
        return breakFree;
    }

    bool moveActor(RE::Actor* npc, const float x, const float y, const float z) noexcept
    {
        return npc && callActor(npc, "SetPosition", x, y, z);
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
