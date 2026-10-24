// Lets ROCK's physical hands stop against standing NPCs.
//
// ROCK's "npcDynamicCollisions" setting pairs its dynamic hands with
// BIPED_NO_CC (33), the layer NPC body parts switch to while sitting. Standing
// NPCs keep their body parts on BIPED (8), so hands pass through them. ROCK
// re-applies its layer matrix every frame, so instead of changing the matrix
// this moves the body parts of NPCs near the player from 8 to 33, and back
// once they are far away. Standing NPCs also carry body flags that sitting
// ones do not, so the configured flags are cleared too. Offsets come from ROCK 0.9 (HavokOffsets.h) for
// Fallout 4 VR 1.2.72.

#include "NpcCollision.h"
#include "Runtime.h"

#include <RE/Fallout.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace npctouch::collision
{
    namespace
    {
        constexpr std::uint32_t kLayerBiped = 8;
        constexpr std::uint32_t kLayerBipedNoCc = 33;
        constexpr std::uint32_t kLayerMask = 0x7F;

        constexpr std::uintptr_t kCollisionObjectGetFilterInfo = 0x1E08D60;
        constexpr std::uintptr_t kSetBodyCollisionFilterInfo = 0x1DF5B80;
        constexpr std::uintptr_t kCollisionObjectPhysicsSystem = 0x20;
        constexpr std::uintptr_t kCollisionObjectBodyIndex = 0x28;
        constexpr std::uintptr_t kPhysicsSystemInstance = 0x18;
        constexpr std::uintptr_t kInstanceWorld = 0x18;
        constexpr std::uintptr_t kInstanceBodyIds = 0x20;
        constexpr std::uintptr_t kInstanceBodyCount = 0x28;
        constexpr std::uintptr_t kWorldBodyArray = 0x20;
        constexpr std::uintptr_t kBodyStride = 0x90;
        constexpr std::uintptr_t kBodyFilterInfo = 0x44;
        constexpr std::uintptr_t kBodyId = 0x60;
        constexpr std::uint32_t kInvalidBodyId = 0x7FFF'FFFF;
        constexpr std::uintptr_t kBodyFlags = 0x40;
        constexpr std::uintptr_t kEnableBodyFlags = 0x153C090;
        constexpr std::uintptr_t kDisableBodyFlags = 0x153C150;
        constexpr std::uintptr_t kActivateBody = 0x1546EF0;

        bool g_enabled{ true };
        float g_radius{ 200.0f };
        // Body flags that differ between a standing NPC whose parts hands pass
        // through (00011486) and a sitting one they stop against (0007008E).
        std::uint32_t g_clearFlags{ 0x1000 };
        std::uint32_t g_setFlags{ 0 };
        bool g_activate{ true };
        std::uint32_t g_loggedFlagChanges{ 0 };
        bool g_faulted{ false };
        std::uint32_t g_frame{ 0 };
        std::uint32_t g_loggedSwitches{ 0 };
        std::unordered_set<std::uint32_t> g_describedActors;
        // NPCs whose body parts we moved to layer 33.
        std::unordered_set<std::uint32_t> g_switched;

        struct BodyRef
        {
            void* world{ nullptr };
            std::uint32_t id{ kInvalidBodyId };
        };

        // Raw game memory is read under SEH so a wrong offset only turns this
        // feature off instead of crashing the game.
        bool resolveBody(void* collisionObject, BodyRef& out) noexcept
        {
            __try {
                auto* system = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(collisionObject) + kCollisionObjectPhysicsSystem);
                if (!system) {
                    return false;
                }
                auto* instance = *reinterpret_cast<std::uint8_t**>(system + kPhysicsSystemInstance);
                if (!instance) {
                    return false;
                }
                auto* world = *reinterpret_cast<std::uint8_t**>(instance + kInstanceWorld);
                auto* ids = *reinterpret_cast<std::uint32_t**>(instance + kInstanceBodyIds);
                const auto count = *reinterpret_cast<std::int32_t*>(instance + kInstanceBodyCount);
                const auto index = *reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(collisionObject) + kCollisionObjectBodyIndex);
                if (!world || !ids || index < 0 || index >= count) {
                    return false;
                }
                const auto id = ids[index];
                if (id == kInvalidBodyId) {
                    return false;
                }
                auto* bodies = *reinterpret_cast<std::uint8_t**>(world + kWorldBodyArray);
                if (!bodies || *reinterpret_cast<std::uint32_t*>(bodies + id * kBodyStride + kBodyId) != id) {
                    return false;
                }
                out.world = world;
                out.id = id;
                return true;
            } __except (1) {
                return false;
            }
        }

        bool readFilter(const BodyRef& body, std::uint32_t& info) noexcept
        {
            __try {
                auto* bodies = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(body.world) + kWorldBodyArray);
                info = *reinterpret_cast<std::uint32_t*>(bodies + body.id * kBodyStride + kBodyFilterInfo);
                return true;
            } __except (1) {
                return false;
            }
        }

        // Body flags (+0x40) and motion index (+0x68), for diagnostics.
        bool readBodyState(const BodyRef& body, std::uint32_t& flags, std::uint32_t& motion) noexcept
        {
            __try {
                auto* bodies = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(body.world) + kWorldBodyArray);
                flags = *reinterpret_cast<std::uint32_t*>(bodies + body.id * kBodyStride + kBodyFlags);
                motion = *reinterpret_cast<std::uint32_t*>(bodies + body.id * kBodyStride + 0x68);
                return true;
            } __except (1) {
                return false;
            }
        }

        using SetFilterInfo = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t);

        bool writeFilter(const BodyRef& body, const std::uint32_t info) noexcept
        {
            __try {
                const auto function = reinterpret_cast<SetFilterInfo>(REL::Module::get().base() + kSetBodyCollisionFilterInfo);
                function(body.world, body.id, info, 0);
                return true;
            } __except (1) {
                return false;
            }
        }

        using ChangeBodyFlags = void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t);
        using ActivateBody = void (*)(void*, std::uint32_t);

        // Clears and sets the configured body flags, then wakes the body up.
        bool adjustFlags(const BodyRef& body, const std::uint32_t flags) noexcept
        {
            __try {
                const auto base = REL::Module::get().base();
                if (g_clearFlags & flags) {
                    reinterpret_cast<ChangeBodyFlags>(base + kDisableBodyFlags)(body.world, body.id, g_clearFlags & flags, 0);
                }
                if (g_setFlags & ~flags) {
                    reinterpret_cast<ChangeBodyFlags>(base + kEnableBodyFlags)(body.world, body.id, g_setFlags & ~flags, 0);
                }
                if (g_activate) {
                    reinterpret_cast<ActivateBody>(base + kActivateBody)(body.world, body.id);
                }
                return true;
            } __except (1) {
                return false;
            }
        }

        using GetFilterInfo = void (*)(void*, std::uint32_t*);

        bool filterInfoOf(void* collisionObject, std::uint32_t& info) noexcept
        {
            __try {
                const auto function = reinterpret_cast<GetFilterInfo>(REL::Module::get().base() + kCollisionObjectGetFilterInfo);
                info = 0xFFFF'FFFF;
                function(collisionObject, &info);
                return true;
            } __except (1) {
                return false;
            }
        }

        template <class F>
        void forEachCollisionObject(RE::NiAVObject* object, F& visit, const int depth = 0)
        {
            if (!object || depth > 64) {
                return;
            }
            if (auto* collision = object->collisionObject.get()) {
                visit(collision);
            }
            if (auto* node = object->IsNode()) {
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    forEachCollisionObject(children[index].get(), visit, depth + 1);
                }
            }
        }

        struct MoveResult
        {
            int moved{ 0 };
            int flagged{ 0 };
            std::uint32_t before{ 0 };
            std::uint32_t after{ 0 };
        };

        // Moves every body part on layer `from` to layer `to`. When `adjust` is
        // set, parts on either layer also get their body flags adjusted.
        MoveResult moveParts(RE::Actor* actor, const std::uint32_t from, const std::uint32_t to, const bool adjust) noexcept
        {
            MoveResult result{};
            auto* root = actor ? actor->Get3D() : nullptr;
            if (!root) {
                return result;
            }
            auto visit = [&](RE::NiCollisionObject* collision) {
                if (g_faulted) {
                    return;
                }
                BodyRef body{};
                std::uint32_t info{ 0 };
                if (!resolveBody(collision, body) || !readFilter(body, info)) {
                    return;
                }
                const auto layer = info & kLayerMask;
                if (layer == from) {
                    if (!writeFilter(body, (info & ~kLayerMask) | to)) {
                        runtime::logWarning("Collision: changing an NPC body layer faulted; NPC hand collision is off");
                        g_faulted = true;
                        return;
                    }
                    ++result.moved;
                } else if (layer != to) {
                    return;
                }
                if (!adjust) {
                    return;
                }
                std::uint32_t flags{ 0 };
                std::uint32_t motion{ 0 };
                if (!readBodyState(body, flags, motion)) {
                    return;
                }
                const bool needsChange = (flags & g_clearFlags) || (~flags & g_setFlags);
                if (!needsChange && !g_activate) {
                    return;
                }
                if (!adjustFlags(body, flags)) {
                    runtime::logWarning("Collision: changing NPC body flags faulted; NPC hand collision is off");
                    g_faulted = true;
                    return;
                }
                if (needsChange) {
                    if (result.flagged++ == 0) {
                        result.before = flags;
                        (void)readBodyState(body, result.after, motion);
                    }
                }
            };
            forEachCollisionObject(root, visit);
            return result;
        }

        float distance(const RE::NiPoint3& a, const RE::NiPoint3& b) noexcept
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        const char* nameOf(RE::Actor* actor) noexcept
        {
            const char* name = actor ? actor->GetDisplayFullName() : nullptr;
            return name && name[0] ? name : "<no name>";
        }
    }

    void initialize() noexcept
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
                const char* text = line.c_str() + equals + 1;
                const float value = std::strtof(text, nullptr);
                if (key == "sClearBodyFlags") {
                    g_clearFlags = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
                } else if (key == "sSetBodyFlags") {
                    g_setFlags = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16));
                } else if (key == "bWakeNPCBodies") {
                    g_activate = value != 0.0f;
                } else if (key == "bHandsCollideWithNPCs") {
                    g_enabled = value != 0.0f;
                } else if (key == "fCollisionRadius" && value > 0.0f) {
                    g_radius = value;
                }
            }
        } catch (...) {
        }
        char message[160]{};
        std::snprintf(
            message,
            sizeof(message),
            "NPC hand collision: %s, radius %.0f, clear flags %08X, set flags %08X, wake %d",
            g_enabled ? "on" : "off",
            g_radius,
            g_clearFlags,
            g_setFlags,
            g_activate ? 1 : 0);
        runtime::logInfo(message);
    }

    void update(const bool worldAvailable) noexcept
    {
        if (!g_enabled || g_faulted) {
            return;
        }
        if (!worldAvailable) {
            g_switched.clear();
            return;
        }
        // The game moves parts back to 8 when an NPC stands up, so check often.
        if (g_frame++ % 10 != 0) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!player || !lists) {
            return;
        }
        std::unordered_set<std::uint32_t> stillNear;
        for (const auto& handle : lists->highActorHandles) {
            auto actorPtr = handle.get();
            auto* actor = actorPtr.get();
            if (!actor || actor == player || actor->IsDead(false)) {
                continue;
            }
            const auto formId = actor->GetFormID();
            const float gap = distance(actor->GetPosition(), player->GetPosition());
            const bool wasSwitched = g_switched.contains(formId);
            // Some slack before switching back, so NPCs at the edge do not flicker.
            if (gap > (wasSwitched ? g_radius * 1.3f : g_radius)) {
                continue;
            }
            stillNear.insert(formId);
            const auto result = moveParts(actor, kLayerBiped, kLayerBipedNoCc, true);
            if (g_faulted) {
                return;
            }
            if (result.moved > 0 && g_loggedSwitches < 30) {
                ++g_loggedSwitches;
                char message[160]{};
                std::snprintf(message, sizeof(message), "Collision: '%s' %d body parts now touchable (layer 8 -> 33)", nameOf(actor), result.moved);
                runtime::logInfo(message);
            }
            if (result.flagged > 0 && g_loggedFlagChanges < 30) {
                ++g_loggedFlagChanges;
                char message[192]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "Collision: '%s' %d body parts got new flags, e.g. %08X -> %08X",
                    nameOf(actor),
                    result.flagged,
                    result.before,
                    result.after);
                runtime::logInfo(message);
            }
        }
        // Put NPCs that walked away back on their own layer, unless they sat down.
        for (auto it = g_switched.begin(); it != g_switched.end();) {
            if (stillNear.contains(*it)) {
                ++it;
                continue;
            }
            auto* actor = RE::TESForm::GetFormByID<RE::Actor>(*it);
            if (actor && actor->Get3D() && actor->DoGetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal) {
                (void)moveParts(actor, kLayerBipedNoCc, kLayerBiped, false);
            }
            it = g_switched.erase(it);
        }
        for (const auto formId : stillNear) {
            g_switched.insert(formId);
        }
    }

    void describeActor(RE::Actor* actor) noexcept
    {
        // Sitting and standing are both worth a sample, so key by state too.
        const auto key = actor ? actor->GetFormID() ^ (static_cast<std::uint32_t>(actor->DoGetSitSleepState()) << 28) : 0;
        if (!actor || g_describedActors.size() >= 24 || !g_describedActors.insert(key).second) {
            return;
        }
        auto* root = actor->Get3D();
        if (!root) {
            return;
        }
        std::uint32_t layers[128]{};
        std::uint32_t parts{ 0 };
        std::uint32_t unread{ 0 };
        char samples[256]{};
        int sampleLength = 0;
        int sampleCount = 0;
        auto visit = [&](RE::NiCollisionObject* collision) {
            ++parts;
            std::uint32_t info{ 0 };
            if (filterInfoOf(collision, info) && info != 0xFFFF'FFFF) {
                ++layers[info & kLayerMask];
            } else {
                ++unread;
            }
            BodyRef body{};
            std::uint32_t bodyInfo{ 0 };
            std::uint32_t flags{ 0 };
            std::uint32_t motion{ 0 };
            if (sampleCount < 4 && resolveBody(collision, body) && readFilter(body, bodyInfo) && readBodyState(body, flags, motion)) {
                ++sampleCount;
                sampleLength += std::snprintf(
                    samples + sampleLength,
                    sizeof(samples) - sampleLength,
                    " {id=%u filter=%08X flags=%08X motion=%u}",
                    body.id,
                    bodyInfo,
                    flags,
                    motion);
            }
        };
        forEachCollisionObject(root, visit);

        char message[512]{};
        int length = std::snprintf(
            message,
            sizeof(message),
            "Collision bodies of '%s': %u parts (%u unreadable), sitState=%u, layers:",
            nameOf(actor),
            parts,
            unread,
            static_cast<std::uint32_t>(actor->DoGetSitSleepState()));
        for (std::uint32_t layer = 0; layer < 128 && length < static_cast<int>(sizeof(message)) - 16; ++layer) {
            if (layers[layer] != 0) {
                length += std::snprintf(message + length, sizeof(message) - length, " %u x%u", layer, layers[layer]);
            }
        }
        runtime::logInfo(message);
        std::snprintf(message, sizeof(message), "  body samples:%s", samples);
        runtime::logInfo(message);
    }
}
