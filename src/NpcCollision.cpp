// Lets ROCK's physical hands stop against standing NPCs.
//
// ROCK's "npcDynamicCollisions" setting pairs its dynamic hands with
// BIPED_NO_CC (33), the layer NPC body parts switch to while sitting. Standing
// NPCs keep their body parts on BIPED (8), so hands pass through them. ROCK
// re-applies its layer matrix every frame, so instead of changing the matrix
// this moves the body parts of NPCs near the player from 8 to 33, and back
// once they are far away. Offsets come from ROCK 0.9 (HavokOffsets.h) for
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

        bool g_enabled{ true };
        float g_radius{ 200.0f };
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

        // Moves every body part on layer `from` to layer `to`; returns how many moved.
        int moveParts(RE::Actor* actor, const std::uint32_t from, const std::uint32_t to) noexcept
        {
            auto* root = actor ? actor->Get3D() : nullptr;
            if (!root) {
                return 0;
            }
            int moved = 0;
            auto visit = [&](RE::NiCollisionObject* collision) {
                if (g_faulted) {
                    return;
                }
                BodyRef body{};
                std::uint32_t info{ 0 };
                if (!resolveBody(collision, body) || !readFilter(body, info) || (info & kLayerMask) != from) {
                    return;
                }
                if (!writeFilter(body, (info & ~kLayerMask) | to)) {
                    runtime::logWarning("Collision: changing an NPC body layer faulted; NPC hand collision is off");
                    g_faulted = true;
                    return;
                }
                ++moved;
            };
            forEachCollisionObject(root, visit);
            return moved;
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
                const float value = std::strtof(line.c_str() + equals + 1, nullptr);
                if (key == "bHandsCollideWithNPCs") {
                    g_enabled = value != 0.0f;
                } else if (key == "fCollisionRadius" && value > 0.0f) {
                    g_radius = value;
                }
            }
        } catch (...) {
        }
        char message[128]{};
        std::snprintf(message, sizeof(message), "NPC hand collision: %s, radius %.0f", g_enabled ? "on" : "off", g_radius);
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
            const int moved = moveParts(actor, kLayerBiped, kLayerBipedNoCc);
            if (g_faulted) {
                return;
            }
            if (moved > 0 && g_loggedSwitches < 30) {
                ++g_loggedSwitches;
                char message[160]{};
                std::snprintf(message, sizeof(message), "Collision: '%s' %d body parts now touchable (layer 8 -> 33)", nameOf(actor), moved);
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
                (void)moveParts(actor, kLayerBipedNoCc, kLayerBiped);
            }
            it = g_switched.erase(it);
        }
        for (const auto formId : stillNear) {
            g_switched.insert(formId);
        }
    }

    void describeActor(RE::Actor* actor) noexcept
    {
        if (!actor || g_describedActors.size() >= 16 || !g_describedActors.insert(actor->GetFormID()).second) {
            return;
        }
        auto* root = actor->Get3D();
        if (!root) {
            return;
        }
        std::uint32_t layers[128]{};
        std::uint32_t parts{ 0 };
        std::uint32_t unread{ 0 };
        auto visit = [&](RE::NiCollisionObject* collision) {
            ++parts;
            std::uint32_t info{ 0 };
            if (filterInfoOf(collision, info) && info != 0xFFFF'FFFF) {
                ++layers[info & kLayerMask];
            } else {
                ++unread;
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
    }
}
