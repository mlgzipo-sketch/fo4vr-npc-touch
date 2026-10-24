// Lets ROCK's physical hands stop against standing NPCs.
//
// ROCK's "npcDynamicCollisions" setting only pairs its dynamic hand and weapon
// layers with BIPED_NO_CC (33), the layer NPC body parts use while sitting.
// Standing NPCs keep their body parts on BIPED (8), so hands pass through.
// This adds the same pairing for BIPED in the game's collision filter matrix.
// Layer numbers and offsets come from ROCK's CollisionLayerPolicy.h and
// HavokOffsets.h for Fallout 4 VR 1.2.72.

#include "NpcCollision.h"
#include "Runtime.h"

#include <RE/Fallout.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_set>

namespace npctouch::collision
{
    namespace
    {
        constexpr std::uint32_t kLayerBiped = 8;
        constexpr std::uint32_t kLayerBipedNoCc = 33;
        // ROCK dynamic right hand, weapon and left hand proxies.
        constexpr std::uint32_t kRockDynamicLayers[]{ 48, 51, 52 };
        constexpr std::uint32_t kLayerMask = 0x7F;

        constexpr std::uintptr_t kCollisionFilterSingleton = 0x59429B8;
        constexpr std::uintptr_t kFilterCollisionMatrix = 0x1A0;
        constexpr std::uintptr_t kCollisionObjectGetFilterInfo = 0x1E08D60;

        bool g_enabled{ true };
        bool g_faulted{ false };
        bool g_loggedRows{ false };
        std::uint32_t g_applyCount{ 0 };
        std::uint32_t g_frame{ 0 };
        std::unordered_set<std::uint32_t> g_describedActors;

        constexpr std::uint64_t bit(const std::uint32_t layer) noexcept
        {
            return std::uint64_t{ 1 } << layer;
        }

        // Raw game memory is read under SEH so a wrong offset only turns this
        // feature off.
        std::uint64_t* readMatrix() noexcept
        {
            __try {
                const auto base = REL::Module::get().base();
                auto* filter = *reinterpret_cast<void**>(base + kCollisionFilterSingleton);
                if (!filter) {
                    return nullptr;
                }
                return reinterpret_cast<std::uint64_t*>(reinterpret_cast<std::uintptr_t>(filter) + kFilterCollisionMatrix);
            } __except (1) {
                return nullptr;
            }
        }

        // Returns how many layer pairs had to be added, or -1 on a fault.
        int applyPairs(std::uint64_t* matrix) noexcept
        {
            __try {
                int added = 0;
                for (const auto layer : kRockDynamicLayers) {
                    // Follow ROCK's own choice: only rows it pairs with sitting NPCs.
                    if ((matrix[layer] & bit(kLayerBipedNoCc)) == 0) {
                        continue;
                    }
                    if ((matrix[layer] & bit(kLayerBiped)) == 0) {
                        matrix[layer] |= bit(kLayerBiped);
                        ++added;
                    }
                    if ((matrix[kLayerBiped] & bit(layer)) == 0) {
                        matrix[kLayerBiped] |= bit(layer);
                        ++added;
                    }
                }
                return added;
            } __except (1) {
                return -1;
            }
        }

        bool copyRow(const std::uint64_t* matrix, const std::uint32_t layer, std::uint64_t& out) noexcept
        {
            __try {
                out = matrix[layer];
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

        void logRows(const std::uint64_t* matrix, const char* when) noexcept
        {
            char message[512]{};
            int length = std::snprintf(message, sizeof(message), "Collision matrix %s:", when);
            for (const auto layer : { kLayerBiped, kLayerBipedNoCc, 43u, 48u, 51u, 52u }) {
                std::uint64_t row{ 0 };
                if (!copyRow(matrix, layer, row)) {
                    runtime::logWarning("Collision matrix could not be read; NPC hand collision is off");
                    g_faulted = true;
                    return;
                }
                length += std::snprintf(message + length, sizeof(message) - length, " [%u]=%016llX", layer, static_cast<unsigned long long>(row));
            }
            runtime::logInfo(message);
        }

        void countLayers(RE::NiAVObject* object, std::uint32_t (&layers)[128], std::uint32_t& bodies, std::uint32_t& unread, const int depth) noexcept
        {
            if (!object || depth > 64) {
                return;
            }
            if (auto* collision = object->collisionObject.get()) {
                ++bodies;
                std::uint32_t info{ 0 };
                if (filterInfoOf(collision, info) && info != 0xFFFF'FFFF) {
                    ++layers[info & kLayerMask];
                } else {
                    ++unread;
                }
            }
            if (auto* node = object->IsNode()) {
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    countLayers(children[index].get(), layers, bodies, unread, depth + 1);
                }
            }
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
                if (key == "bHandsCollideWithNPCs") {
                    g_enabled = std::strtof(line.c_str() + equals + 1, nullptr) != 0.0f;
                }
            }
        } catch (...) {
        }
        runtime::logInfo(g_enabled ? "NPC hand collision: on" : "NPC hand collision: off (bHandsCollideWithNPCs=0)");
    }

    void update(const bool worldAvailable) noexcept
    {
        if (!g_enabled || g_faulted || !worldAvailable) {
            return;
        }
        // ROCK rewrites its rows when the world changes; check about once a second.
        if (g_frame++ % 90 != 0) {
            return;
        }
        auto* matrix = readMatrix();
        if (!matrix) {
            return;
        }
        if (!g_loggedRows) {
            logRows(matrix, "before");
        }
        const int added = applyPairs(matrix);
        if (added < 0) {
            runtime::logWarning("Collision matrix write faulted; NPC hand collision is off");
            g_faulted = true;
            return;
        }
        if (added > 0) {
            ++g_applyCount;
            char message[160]{};
            std::snprintf(message, sizeof(message), "Collision: paired ROCK hands with standing NPCs (%d bits, time %u)", added, g_applyCount);
            runtime::logInfo(message);
            if (!g_loggedRows || g_applyCount <= 3) {
                logRows(matrix, "after");
            }
        } else if (!g_loggedRows) {
            runtime::logInfo("Collision: ROCK rows are not paired with sitting NPCs (npcDynamicCollisions off?); nothing changed");
        }
        g_loggedRows = true;
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
        std::uint32_t bodies{ 0 };
        std::uint32_t unread{ 0 };
        countLayers(root, layers, bodies, unread, 0);

        char message[512]{};
        const char* name = actor->GetDisplayFullName();
        int length = std::snprintf(
            message,
            sizeof(message),
            "Collision bodies of '%s': %u parts (%u unreadable), sitState=%u, layers:",
            name ? name : "?",
            bodies,
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
