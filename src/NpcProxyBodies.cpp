// Invisible physics bodies that follow NPC bones, so ROCK's hands stop
// against standing NPCs.
//
// Standing NPCs' own body parts take no part in collisions with ROCK's hand
// proxies, whatever their layer or flags. ROCK 0.93 solves this the same way:
// passive copies of the animated limbs. This file gives each nearby NPC a set
// of keyframed capsules (torso, shoulders, head, arms, legs) between pairs of
// bones, on layer 33 (which ROCK's dynamic hands collide with when
// npcDynamicCollisions = true) and in the NPC's own collision group, so the NPC
// does not push against them. Every frame each capsule is driven to its bones.
//
// Body creation follows ROCK's BethesdaPhysicsBody::create (GPL-3.0,
// github.com/brunocatani/ROCK); offsets are for Fallout 4 VR 1.2.72.

#include "NpcProxyBodies.h"
#include "Runtime.h"

#include <RE/Fallout.h>
#include <REX/W32/KERNEL32.h>

#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace npctouch::proxies
{
    namespace
    {
        // Allocators.
        constexpr std::uintptr_t kBethesdaAlloc = 0x1B91950;
        constexpr std::uintptr_t kBethesdaAllocatorInit = 0x1B91DD0;
        constexpr std::uintptr_t kBethesdaAllocatorPool = 0x392E400;
        constexpr std::uintptr_t kBethesdaAllocatorState = 0x392E880;
        constexpr std::uintptr_t kBethesdaTlsIndex = 0x689CACC;
        constexpr std::uintptr_t kBethesdaTlsAllocatorContext = 0x9C0;
        constexpr std::uintptr_t kHavokTlsAllocKey = 0x5B63B20;
        constexpr std::uintptr_t kHkArrayReserveMore = 0x155D820;
        constexpr std::uintptr_t kHkArrayAllocatorGlobal = 0x3866310;

        // Construction.
        constexpr std::uintptr_t kPhysicsSystemDataCtor = 0x5EAB0;
        constexpr std::uintptr_t kMotionCinfoCtor = 0x17A2FC0;
        constexpr std::uintptr_t kBodyCinfoCtor = 0x1561DD0;
        constexpr std::uintptr_t kMaterialCtor = 0x1536CB0;
        constexpr std::uintptr_t kPhysicsSystemCtor = 0x1E0C2B0;
        constexpr std::uintptr_t kPhysicsSystemGetBodyId = 0x1E0C460;
        constexpr std::uintptr_t kCollisionObjectCtor = 0x1E07710;
        constexpr std::uintptr_t kCollisionObjectAddToWorld = 0x1E07BE0;
        constexpr std::uintptr_t kCollisionObjectLinkObject = 0x2996CB0;
        constexpr std::uintptr_t kNiNodeCtor = 0x1C17D30;
        constexpr std::uintptr_t kNiNodeSetName = 0x1C16C30;
        constexpr std::uintptr_t kBSFixedStringCreate = 0x1BC1650;
        constexpr std::uintptr_t kConvexBuildConfigInit = 0x16D4AB0;
        constexpr std::uintptr_t kConvexShapeFromPoints = 0x16D4B30;

        // Body setup and motion.
        constexpr std::uintptr_t kSetBodyMaterial = 0x153AFC0;
        constexpr std::uintptr_t kSetBodyKeyframed = 0x1DF5CB0;
        constexpr std::uintptr_t kSetBodyCollisionFilterInfo = 0x1DF5B80;
        constexpr std::uintptr_t kEnableBodyFlags = 0x153C090;
        constexpr std::uintptr_t kActivateBody = 0x1546EF0;
        constexpr std::uintptr_t kDriveToKeyFrame = 0x1E086E0;
        constexpr std::uintptr_t kSetTransform = 0x1E08A70;
        constexpr std::uintptr_t kRemovePhysicsSystemInstance = 0x1DFAD00;

        // Layouts.
        constexpr std::uintptr_t kSysDataMaterials = 0x10;
        constexpr std::uintptr_t kSysDataMotionCinfos = 0x30;
        constexpr std::uintptr_t kSysDataBodyCinfos = 0x40;
        constexpr std::uintptr_t kSysDataShapes = 0x60;
        constexpr std::uintptr_t kBhkWorldHknpWorld = 0x60;
        constexpr std::uintptr_t kBhkPhysicsSystemInstance = 0x18;
        constexpr std::uintptr_t kWorldBodyArray = 0x20;
        constexpr std::uintptr_t kBodyStride = 0x90;
        constexpr std::uintptr_t kBodyFilterInfo = 0x44;
        constexpr std::uintptr_t kBodyMaterial = 0x70;
        constexpr std::uintptr_t kBodyId = 0x60;
        constexpr std::uintptr_t kCollisionObjectPhysicsSystem = 0x20;
        constexpr std::uintptr_t kCollisionObjectBodyIndex = 0x28;
        constexpr std::uintptr_t kInstanceBodyIds = 0x20;
        constexpr std::uintptr_t kInstanceBodyCount = 0x28;
        constexpr std::size_t kNiNodeSize = 0x180;
        constexpr std::uint32_t kInvalidBodyId = 0x7FFF'FFFF;
        constexpr std::uint32_t kGeneratedBodyRuntimeFlags = 0x0802'0000;
        constexpr std::uint32_t kLayerBiped = 8;
        constexpr std::uint32_t kLayerBipedNoCc = 33;
        constexpr std::uint32_t kLayerMask = 0x7F;
        constexpr float kGameToHavok = 1.0f / 70.0f;
        constexpr const char* kBodyName = "NpcTouch_BodyProxy";

        // One capsule between two bones. With no end bone the capsule runs from
        // the start bone along the parent -> start direction for `extend` units
        // (head, hands). Radii are in game units for a regular-size NPC.
        struct Segment
        {
            const char* name;
            const char* from;
            const char* to;
            const char* parent;
            float extend;
            float radius;
        };
        constexpr Segment kSegments[] = {
            { "belly", "Pelvis", "SPINE2", nullptr, 0.0f, 12.0f },
            { "chest", "SPINE2", "Neck", nullptr, 0.0f, 12.0f },
            { "shoulders", "LArm_UpperArm", "RArm_UpperArm", nullptr, 0.0f, 6.5f },
            { "head", "Head", nullptr, "Neck", 9.0f, 8.5f },
            { "left upper arm", "LArm_UpperArm", "LArm_ForeArm1", nullptr, 0.0f, 4.5f },
            { "left forearm", "LArm_ForeArm1", "LArm_Hand", nullptr, 0.0f, 3.8f },
            { "left hand", "LArm_Hand", nullptr, "LArm_ForeArm1", 7.0f, 3.2f },
            { "right upper arm", "RArm_UpperArm", "RArm_ForeArm1", nullptr, 0.0f, 4.5f },
            { "right forearm", "RArm_ForeArm1", "RArm_Hand", nullptr, 0.0f, 3.8f },
            { "right hand", "RArm_Hand", nullptr, "RArm_ForeArm1", 7.0f, 3.2f },
            { "left thigh", "LLeg_Thigh", "LLeg_Calf", nullptr, 0.0f, 7.0f },
            { "left calf", "LLeg_Calf", "LLeg_Foot", nullptr, 0.0f, 5.0f },
            { "right thigh", "RLeg_Thigh", "RLeg_Calf", nullptr, 0.0f, 7.0f },
            { "right calf", "RLeg_Calf", "RLeg_Foot", nullptr, 0.0f, 5.0f },
        };
        constexpr std::size_t kSegmentCount = sizeof(kSegments) / sizeof(kSegments[0]);

        bool g_enabled{ true };
        float g_range{ 150.0f };
        float g_radiusScale{ 1.0f };
        bool g_legs{ true };
        std::uint32_t g_maxNpcs{ 4 };
        bool g_faulted{ false };
        std::uint32_t g_logged{ 0 };
        std::chrono::steady_clock::time_point g_lastFrame{};
        std::chrono::steady_clock::time_point g_lastReport{};

        struct Bones
        {
            RE::NiAVObject* from{ nullptr };
            RE::NiAVObject* to{ nullptr };
            RE::NiAVObject* parent{ nullptr };
        };

        struct Proxy
        {
            std::size_t segment{ 0 };
            Bones bones;
            void* bhkWorld{ nullptr };
            void* world{ nullptr };
            void* collisionObject{ nullptr };
            void* physicsSystem{ nullptr };
            std::uint32_t bodyId{ kInvalidBodyId };
        };
        struct NpcBodies
        {
            std::uint32_t formId{ 0 };
            void* bhkWorld{ nullptr };
            RE::NiAVObject* root{ nullptr };  // bodies are rebuilt if the 3D changes
            std::vector<Proxy> parts;
        };
        std::vector<NpcBodies> g_npcs;

        std::uintptr_t base() noexcept
        {
            return REL::Module::get().base();
        }

        template <class T>
        T at(const std::uintptr_t offset) noexcept
        {
            return reinterpret_cast<T>(base() + offset);
        }

        void logLimited(const char* message) noexcept
        {
            if (g_logged < 60) {
                ++g_logged;
                runtime::logInfo(message);
            }
        }

        // --- Raw native helpers (SEH only, no C++ objects with destructors) ---

        void* bethesdaAlloc(const std::size_t size) noexcept
        {
            auto* state = at<std::uint32_t*>(kBethesdaAllocatorState);
            auto* pool = at<void*>(kBethesdaAllocatorPool);
            if (*state != 2) {
                at<void* (*)(void*, std::uint32_t*)>(kBethesdaAllocatorInit)(pool, state);
            }
            if (*state != 2) {
                return nullptr;
            }
            return at<void* (*)(void*, std::size_t, std::uint32_t, char)>(kBethesdaAlloc)(pool, size, 0, '\0');
        }

        std::uint32_t* allocatorContextSlot() noexcept
        {
            const auto index = *at<std::uint32_t*>(kBethesdaTlsIndex);
            auto* slots = reinterpret_cast<void**>(__readgsqword(0x58));
            if (!slots || !slots[index]) {
                return nullptr;
            }
            return reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(slots[index]) + kBethesdaTlsAllocatorContext);
        }

        void* allocateHavok(const std::size_t size) noexcept
        {
            auto* block = static_cast<std::uint8_t*>(REX::W32::TlsGetValue(*at<std::uint32_t*>(kHavokTlsAllocKey)));
            if (!block) {
                return nullptr;
            }
            auto* allocator = *reinterpret_cast<void**>(block + 0x58);
            if (!allocator) {
                return nullptr;
            }
            auto* vtable = *reinterpret_cast<void* (***)(void*, std::size_t)>(allocator);
            return vtable[1](allocator, size);
        }

        void havokRelease(void* object) noexcept
        {
            auto* count = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(object) + 0x08);
            for (;;) {
                const long old = *count;
                const auto flags = static_cast<std::uint16_t>((static_cast<std::uint32_t>(old) >> 16) & 0xFFFF);
                const auto refs = static_cast<std::uint16_t>(old & 0xFFFF);
                if (flags == 0 || refs == 0 || refs == 0xFFFF) {
                    return;
                }
                const long next = (old & static_cast<long>(0xFFFF0000u)) | static_cast<long>(static_cast<std::uint16_t>(refs - 1));
                if (_InterlockedCompareExchange(count, next, old) != old) {
                    continue;
                }
                if (refs == 1) {
                    auto** vtable = *reinterpret_cast<void***>(object);
                    reinterpret_cast<void (*)(void*)>(vtable[3])(object);
                }
                return;
            }
        }

        void havokAddRef(void* object) noexcept
        {
            auto* count = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(object) + 0x08);
            for (;;) {
                const long old = *count;
                const auto flags = static_cast<std::uint16_t>((static_cast<std::uint32_t>(old) >> 16) & 0xFFFF);
                const auto refs = static_cast<std::uint16_t>(old & 0xFFFF);
                if (flags == 0 || refs == 0xFFFF) {
                    return;
                }
                const long next = (old & static_cast<long>(0xFFFF0000u)) | static_cast<long>(static_cast<std::uint16_t>(refs + 1));
                if (_InterlockedCompareExchange(count, next, old) == old) {
                    return;
                }
            }
        }

        // Appends one element to an hkArray (data +0, size +8, capacity +0xC).
        std::uint8_t* appendOne(std::uint8_t* array, const int stride) noexcept
        {
            auto& data = *reinterpret_cast<std::uint8_t**>(array);
            auto& size = *reinterpret_cast<std::int32_t*>(array + 0x08);
            const auto capacity = *reinterpret_cast<std::int32_t*>(array + 0x0C) & 0x3FFF'FFFF;
            if (size >= capacity) {
                at<void (*)(void*, void*, int)>(kHkArrayReserveMore)(at<void*>(kHkArrayAllocatorGlobal), array, stride);
            }
            if (!data) {
                return nullptr;
            }
            auto* entry = data + size * stride;
            ++size;
            return entry;
        }

        void niIncRef(void* object) noexcept
        {
            static_cast<RE::NiRefObject*>(object)->IncRefCount();
        }

        void niDecRef(void* object) noexcept
        {
            static_cast<RE::NiRefObject*>(object)->DecRefCount();
        }

        struct StridedPoints
        {
            const void* data;
            std::int32_t count;
            std::int32_t stride;
        };

        // Octagonal prism along local X, rounded by the convex radius: close
        // enough to a capsule. Same point layout as ROCK's capsule-like hulls.
        void* capsuleShapeRaw(const float halfLength, const float radius) noexcept
        {
            alignas(16) float points[16][4]{};
            constexpr float d = 0.7071067f;
            const float ring[8][2] = { { radius, 0 }, { -radius, 0 }, { 0, radius }, { 0, -radius }, { radius * d, radius * d }, { -radius * d, radius * d }, { radius * d, -radius * d }, { -radius * d, -radius * d } };
            int count = 0;
            for (const float x : { -halfLength, halfLength }) {
                for (const auto& corner : ring) {
                    points[count][0] = x * kGameToHavok;
                    points[count][1] = corner[0] * kGameToHavok;
                    points[count][2] = corner[1] * kGameToHavok;
                    ++count;
                }
            }
            alignas(16) std::uint8_t config[0x80]{};
            at<void* (*)(void*)>(kConvexBuildConfigInit)(config);
            const StridedPoints array{ points, count, static_cast<std::int32_t>(sizeof(points[0])) };
            return at<void* (*)(const StridedPoints*, float, void*)>(kConvexShapeFromPoints)(&array, 1.0f * kGameToHavok, config);
        }

        // Steps of ROCK's BethesdaPhysicsBody::create, for one keyframed body.
        // Returns 0 on success, otherwise the number of the step that failed.
        int createBodyRaw(void* world, void* bhkWorld, void* shape, const std::uint32_t filter, const std::uint16_t material, Proxy& out) noexcept
        {
            auto* systemData = static_cast<std::uint8_t*>(allocateHavok(0x78));
            if (!systemData) {
                return 1;
            }
            at<void* (*)(void*)>(kPhysicsSystemDataCtor)(systemData);

            auto* motionArray = systemData + kSysDataMotionCinfos;
            const auto motionIndex = *reinterpret_cast<std::int32_t*>(motionArray + 0x08);
            auto* motion = appendOne(motionArray, 0x70);
            if (motionIndex < 0 || !motion) {
                havokRelease(systemData);
                return 2;
            }
            at<void* (*)(void*)>(kMotionCinfoCtor)(motion);

            auto* cinfo = appendOne(systemData + kSysDataBodyCinfos, 0x60);
            if (!cinfo) {
                havokRelease(systemData);
                return 3;
            }
            at<void* (*)(void*)>(kBodyCinfoCtor)(cinfo);
            *reinterpret_cast<void**>(cinfo + 0x00) = shape;
            *reinterpret_cast<std::uint32_t*>(cinfo + 0x08) = kInvalidBodyId;
            *reinterpret_cast<std::uint32_t*>(cinfo + 0x0C) = static_cast<std::uint32_t>(motionIndex);
            *reinterpret_cast<std::uint16_t*>(cinfo + 0x10) = 0xFF;  // keyframed motion properties
            *reinterpret_cast<std::uint16_t*>(cinfo + 0x12) = 0;     // local material 0
            *reinterpret_cast<std::uint32_t*>(cinfo + 0x14) = filter;
            *reinterpret_cast<float*>(cinfo + 0x1C) = 0.0f;
            *reinterpret_cast<const char**>(cinfo + 0x20) = kBodyName;
            *reinterpret_cast<std::uintptr_t*>(cinfo + 0x28) = 0;
            *reinterpret_cast<std::uint8_t*>(cinfo + 0x50) = 0;

            auto* materialEntry = appendOne(systemData + kSysDataMaterials, 0x50);
            if (!materialEntry) {
                havokRelease(systemData);
                return 4;
            }
            at<void* (*)(void*)>(kMaterialCtor)(materialEntry);

            auto* shapeSlot = appendOne(systemData + kSysDataShapes, 8);
            if (!shapeSlot) {
                havokRelease(systemData);
                return 5;
            }
            *reinterpret_cast<void**>(shapeSlot) = shape;
            havokAddRef(shape);

            auto* physicsSystem = bethesdaAlloc(0x28);
            if (!physicsSystem) {
                havokRelease(systemData);
                return 6;
            }
            at<void* (*)(void*, void*)>(kPhysicsSystemCtor)(physicsSystem, systemData);
            niIncRef(physicsSystem);
            havokRelease(systemData);

            auto* contextSlot = allocatorContextSlot();
            if (!contextSlot) {
                return 7;
            }
            const auto previousContext = *contextSlot;
            *contextSlot = 0x41;
            auto* collisionObject = bethesdaAlloc(0x30);
            if (collisionObject) {
                at<void* (*)(void*, std::uint32_t, void*)>(kCollisionObjectCtor)(collisionObject, 0, physicsSystem);
            }
            *contextSlot = previousContext;
            if (!collisionObject) {
                return 8;
            }
            niIncRef(collisionObject);
            niDecRef(physicsSystem);  // construction reference -> collision object's edge

            auto* node = bethesdaAlloc(kNiNodeSize);
            if (!node) {
                return 9;
            }
            std::memset(node, 0, kNiNodeSize);
            at<void* (*)(void*, std::uint16_t)>(kNiNodeCtor)(node, 0);
            niIncRef(node);
            alignas(8) char name[8]{};
            at<void (*)(void*, const char*)>(kBSFixedStringCreate)(name, kBodyName);
            at<void (*)(void*, const void*)>(kNiNodeSetName)(node, name);
            at<void (*)(void*, void*)>(kCollisionObjectLinkObject)(collisionObject, node);

            at<void (*)(void*, void*)>(kCollisionObjectAddToWorld)(collisionObject, bhkWorld);
            std::uint32_t bodyId{ kInvalidBodyId };
            at<void (*)(void*, std::uint32_t*, std::int32_t)>(kPhysicsSystemGetBodyId)(physicsSystem, &bodyId, 0);
            if (bodyId == kInvalidBodyId) {
                return 10;
            }
            at<void (*)(void*, std::uint32_t, std::uint16_t, std::int32_t)>(kSetBodyMaterial)(world, bodyId, material, 0);
            at<void (*)(void*, std::uint32_t)>(kSetBodyKeyframed)(world, bodyId);
            at<void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t)>(kSetBodyCollisionFilterInfo)(world, bodyId, filter, 1);
            at<void (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t)>(kEnableBodyFlags)(world, bodyId, kGeneratedBodyRuntimeFlags, 0);
            at<void (*)(void*, std::uint32_t)>(kActivateBody)(world, bodyId);

            out.bhkWorld = bhkWorld;
            out.world = world;
            out.collisionObject = collisionObject;
            out.physicsSystem = physicsSystem;
            out.bodyId = bodyId;
            return 0;
        }

        int createBody(void* world, void* bhkWorld, void* shape, const std::uint32_t filter, const std::uint16_t material, Proxy& out) noexcept
        {
            __try {
                return createBodyRaw(world, bhkWorld, shape, filter, material, out);
            } __except (1) {
                return 99;
            }
        }

        void* createCapsule(const float halfLength, const float radius) noexcept
        {
            __try {
                return capsuleShapeRaw(halfLength, radius);
            } __except (1) {
                return nullptr;
            }
        }

        // The memory stays allocated on purpose: the game keeps raw pointers to
        // collision objects after they leave the world (see ROCK's notes).
        bool removeBody(const Proxy& proxy) noexcept
        {
            __try {
                auto* instance = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(proxy.physicsSystem) + kBhkPhysicsSystemInstance);
                if (instance) {
                    at<void (*)(void*, void*)>(kRemovePhysicsSystemInstance)(proxy.bhkWorld, instance);
                }
                return true;
            } __except (1) {
                return false;
            }
        }

        bool placeBody(const Proxy& proxy, const float (&transform)[16], const float dt, const bool teleport) noexcept
        {
            __try {
                if (teleport) {
                    at<std::uint8_t (*)(void*, const void*)>(kSetTransform)(proxy.collisionObject, transform);
                } else {
                    at<std::uint8_t (*)(void*, const void*, float)>(kDriveToKeyFrame)(proxy.collisionObject, transform, dt);
                }
                return true;
            } __except (1) {
                return false;
            }
        }

        bool readBodyPosition(const Proxy& proxy, float (&position)[3]) noexcept
        {
            __try {
                auto* bodies = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(proxy.world) + kWorldBodyArray);
                const auto* translation = reinterpret_cast<const float*>(bodies + proxy.bodyId * kBodyStride + 0x30);
                for (int axis = 0; axis < 3; ++axis) {
                    position[axis] = translation[axis] / kGameToHavok;
                }
                return true;
            } __except (1) {
                return false;
            }
        }

        // Filter info and material of one of the NPC's own biped bodies.
        bool readActorBody(void* collisionObject, std::uint32_t& filter, std::uint16_t& material, void*& world) noexcept
        {
            __try {
                auto* system = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(collisionObject) + kCollisionObjectPhysicsSystem);
                if (!system) {
                    return false;
                }
                auto* instance = *reinterpret_cast<std::uint8_t**>(system + kBhkPhysicsSystemInstance);
                if (!instance) {
                    return false;
                }
                world = *reinterpret_cast<void**>(instance + 0x18);
                auto* ids = *reinterpret_cast<std::uint32_t**>(instance + kInstanceBodyIds);
                const auto count = *reinterpret_cast<std::int32_t*>(instance + kInstanceBodyCount);
                const auto index = *reinterpret_cast<std::int32_t*>(static_cast<std::uint8_t*>(collisionObject) + kCollisionObjectBodyIndex);
                if (!world || !ids || index < 0 || index >= count || ids[index] == kInvalidBodyId) {
                    return false;
                }
                auto* bodies = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(world) + kWorldBodyArray);
                auto* body = bodies + ids[index] * kBodyStride;
                if (*reinterpret_cast<std::uint32_t*>(body + kBodyId) != ids[index]) {
                    return false;
                }
                filter = *reinterpret_cast<std::uint32_t*>(body + kBodyFilterInfo);
                material = *reinterpret_cast<std::uint16_t*>(body + kBodyMaterial);
                return true;
            } __except (1) {
                return false;
            }
        }

        void* hknpWorldOf(void* bhkWorld) noexcept
        {
            __try {
                return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(bhkWorld) + kBhkWorldHknpWorld);
            } __except (1) {
                return nullptr;
            }
        }

        // --- Scene helpers ---

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

        // The first biped body part of the actor tells us its collision group.
        bool actorCollisionInfo(RE::NiAVObject* object, std::uint32_t& filter, std::uint16_t& material, void*& world, const int depth = 0) noexcept
        {
            if (!object || depth > 64) {
                return false;
            }
            if (auto* collision = object->collisionObject.get()) {
                if (readActorBody(collision, filter, material, world)) {
                    const auto layer = filter & kLayerMask;
                    if (layer == kLayerBiped || layer == kLayerBipedNoCc) {
                        return true;
                    }
                }
            }
            if (auto* node = object->IsNode()) {
                auto& children = node->GetRuntimeData().children;
                for (std::uint16_t index = 0; index < children.size(); ++index) {
                    if (actorCollisionInfo(children[index].get(), filter, material, world, depth + 1)) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool findBones(RE::NiAVObject* root, const Segment& segment, Bones& bones) noexcept
        {
            bones.from = findNode(root, segment.from);
            bones.to = segment.to ? findNode(root, segment.to) : nullptr;
            bones.parent = segment.parent ? findNode(root, segment.parent) : nullptr;
            return bones.from && (segment.to ? bones.to != nullptr : bones.parent != nullptr);
        }

        // Where a segment's capsule goes this frame: centre, local X along the
        // bones. Returns false when the bones overlap.
        bool segmentTransform(const Segment& segment, const Bones& bones, float (&transform)[16], float& halfLength) noexcept
        {
            const RE::NiPoint3 start = bones.from->world.translate;
            RE::NiPoint3 end{};
            if (bones.to) {
                end = bones.to->world.translate;
            } else {
                RE::NiPoint3 direction = start - bones.parent->world.translate;
                const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
                if (length < 0.5f) {
                    return false;
                }
                end = start + direction * (segment.extend / length);
            }
            RE::NiPoint3 axis = end - start;
            const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
            if (length < 0.5f) {
                return false;
            }
            axis = axis * (1.0f / length);
            // Any perpendicular works for a round capsule; pick a stable one.
            const RE::NiPoint3 reference = std::abs(axis.z) < 0.9f ? RE::NiPoint3{ 0.0f, 0.0f, 1.0f } : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
            RE::NiPoint3 side{ reference.y * axis.z - reference.z * axis.y, reference.z * axis.x - reference.x * axis.z, reference.x * axis.y - reference.y * axis.x };
            const float sideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
            side = side * (1.0f / sideLength);
            const RE::NiPoint3 up{ axis.y * side.z - axis.z * side.y, axis.z * side.x - axis.x * side.z, axis.x * side.y - axis.y * side.x };
            const RE::NiPoint3 centre = (start + end) * 0.5f;
            std::memset(transform, 0, sizeof(transform));
            const RE::NiPoint3* columns[3] = { &axis, &side, &up };
            for (int column = 0; column < 3; ++column) {
                transform[column * 4 + 0] = columns[column]->x;
                transform[column * 4 + 1] = columns[column]->y;
                transform[column * 4 + 2] = columns[column]->z;
            }
            transform[12] = centre.x * kGameToHavok;
            transform[13] = centre.y * kGameToHavok;
            transform[14] = centre.z * kGameToHavok;
            halfLength = length * 0.5f;
            return true;
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

        void fault(const char* what) noexcept
        {
            char message[160]{};
            std::snprintf(message, sizeof(message), "Proxy bodies: %s faulted; NPC proxy bodies are off", what);
            runtime::logWarning(message);
            g_faulted = true;
        }

        bool isLeg(const Segment& segment) noexcept
        {
            return std::strstr(segment.name, "thigh") || std::strstr(segment.name, "calf");
        }

        // Removes all bodies of one NPC; false if the game faulted.
        bool removeNpc(const NpcBodies& npc) noexcept
        {
            for (const auto& part : npc.parts) {
                if (!removeBody(part)) {
                    return false;
                }
            }
            return true;
        }

        // Builds every capsule for one NPC. Missing bones just skip a part.
        bool createNpc(RE::Actor* actor, void* world, void* bhkWorld, NpcBodies& out) noexcept
        {
            auto* root = actor->Get3D();
            std::uint32_t npcFilter{ 0 };
            std::uint16_t material{ 0 };
            void* npcWorld{ nullptr };
            if (!root || !actorCollisionInfo(root, npcFilter, material, npcWorld) || npcWorld != world) {
                return true;
            }
            const std::uint32_t filter = (npcFilter & 0xFFFF'0000u) | kLayerBipedNoCc;
            out.formId = actor->GetFormID();
            out.bhkWorld = bhkWorld;
            out.root = root;
            for (std::size_t index = 0; index < kSegmentCount; ++index) {
                const auto& segment = kSegments[index];
                if (!g_legs && isLeg(segment)) {
                    continue;
                }
                Bones bones{};
                float transform[16]{};
                float halfLength{ 0.0f };
                if (!findBones(root, segment, bones) || !segmentTransform(segment, bones, transform, halfLength)) {
                    continue;
                }
                const float radius = segment.radius * g_radiusScale;
                auto* shape = createCapsule(halfLength, radius);
                if (!shape) {
                    fault("building a capsule shape");
                    return false;
                }
                Proxy part{};
                part.segment = index;
                part.bones = bones;
                const int step = createBody(world, bhkWorld, shape, filter, material, part);
                if (step != 0) {
                    char message[200]{};
                    std::snprintf(message, sizeof(message), "Proxy bodies: creating '%s' for '%s' failed at step %d", segment.name, nameOf(actor), step);
                    runtime::logWarning(message);
                    fault("creating a body");
                    return false;
                }
                if (!placeBody(part, transform, 0.0f, true)) {
                    fault("placing a new body");
                    return false;
                }
                out.parts.push_back(part);
            }
            char message[200]{};
            std::snprintf(
                message,
                sizeof(message),
                "Proxy bodies: '%s' got %zu capsules (filter %08X, material %u)",
                nameOf(actor),
                out.parts.size(),
                filter,
                material);
            logLimited(message);
            return true;
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
                } else if (key == "fProxyRange" && value > 0.0f) {
                    g_range = value;
                } else if (key == "fProxyThickness" && value > 0.1f) {
                    g_radiusScale = value;
                } else if (key == "bProxyLegs") {
                    g_legs = value != 0.0f;
                } else if (key == "iMaxProxyNPCs" && value >= 1.0f) {
                    g_maxNpcs = static_cast<std::uint32_t>(value);
                }
            }
        } catch (...) {
        }
        char message[160]{};
        std::snprintf(
            message,
            sizeof(message),
            "NPC proxy bodies: %s, range %.0f, thickness x%.2f, legs %d, up to %u NPCs",
            g_enabled ? "on" : "off",
            g_range,
            g_radiusScale,
            g_legs ? 1 : 0,
            g_maxNpcs);
        runtime::logInfo(message);
    }

    void update(const bool worldAvailable) noexcept
    {
        if (!g_enabled || g_faulted) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - g_lastFrame).count();
        g_lastFrame = now;
        dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 20.0f);

        if (!worldAvailable) {
            // The world is going away and takes our bodies with it.
            g_npcs.clear();
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* lists = RE::ProcessLists::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        void* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
        if (!lists || !bhkWorld) {
            g_npcs.clear();
            return;
        }
        void* world = hknpWorldOf(bhkWorld);
        if (!world) {
            return;
        }
        // A new physics world (another cell) means the old bodies are gone.
        if (!g_npcs.empty() && g_npcs.front().bhkWorld != bhkWorld) {
            logLimited("Proxy bodies: physics world changed, dropping old bodies");
            g_npcs.clear();
        }

        // NPCs near enough to touch, nearest first. NPCs that already have
        // bodies keep them a little further out, so they do not flicker.
        struct Candidate
        {
            RE::Actor* actor;
            float gap;
        };
        std::vector<Candidate> candidates;
        for (const auto& handle : lists->highActorHandles) {
            auto actorPtr = handle.get();
            auto* actor = actorPtr.get();
            if (!actor || actor == player || actor->IsDead(false) || actor->GetParentCell() != cell || !actor->Get3D()) {
                continue;
            }
            const auto formId = actor->GetFormID();
            const bool has = std::any_of(g_npcs.begin(), g_npcs.end(), [&](const NpcBodies& n) { return n.formId == formId; });
            const float gap = distance(actor->GetPosition(), player->GetPosition());
            if (gap <= (has ? g_range + 60.0f : g_range)) {
                candidates.push_back({ actor, gap });
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.gap < b.gap; });
        if (candidates.size() > g_maxNpcs) {
            candidates.resize(g_maxNpcs);
        }

        // Remove bodies of NPCs that are no longer close.
        for (auto it = g_npcs.begin(); it != g_npcs.end();) {
            const bool keep = std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c) {
                return c.actor->GetFormID() == it->formId && c.actor->Get3D() == it->root;
            });
            if (keep) {
                ++it;
                continue;
            }
            if (!removeNpc(*it)) {
                fault("removing a body");
                return;
            }
            char message[128]{};
            std::snprintf(message, sizeof(message), "Proxy bodies: removed %zu capsules of NPC %08X", it->parts.size(), it->formId);
            logLimited(message);
            it = g_npcs.erase(it);
        }

        bool createdThisFrame = false;
        for (const auto& candidate : candidates) {
            const auto formId = candidate.actor->GetFormID();
            auto existing = std::find_if(g_npcs.begin(), g_npcs.end(), [&](const NpcBodies& n) { return n.formId == formId; });
            if (existing == g_npcs.end()) {
                // One new NPC per frame keeps a crowd from stalling a frame.
                if (createdThisFrame) {
                    continue;
                }
                createdThisFrame = true;
                NpcBodies npc{};
                if (!createNpc(candidate.actor, world, bhkWorld, npc)) {
                    return;
                }
                if (!npc.parts.empty()) {
                    g_npcs.push_back(std::move(npc));
                }
                continue;
            }
            for (const auto& part : existing->parts) {
                float transform[16]{};
                float halfLength{ 0.0f };
                if (!segmentTransform(kSegments[part.segment], part.bones, transform, halfLength)) {
                    continue;
                }
                if (!placeBody(part, transform, dt, false)) {
                    fault("moving a body");
                    return;
                }
            }
        }

        // Every few seconds, show whether the bodies keep up with the bones.
        if (!g_npcs.empty() && now - g_lastReport > std::chrono::seconds(5)) {
            g_lastReport = now;
            const auto& npc = g_npcs.front();
            auto* actor = RE::TESForm::GetFormByID<RE::Actor>(npc.formId);
            for (const auto& part : npc.parts) {
                if (part.segment != 1) {
                    continue;
                }
                float transform[16]{};
                float halfLength{ 0.0f };
                float position[3]{};
                if (segmentTransform(kSegments[part.segment], part.bones, transform, halfLength) && readBodyPosition(part, position)) {
                    char message[200]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "Proxy bodies: %zu NPCs; '%s' chest target (%.0f, %.0f, %.0f) body (%.0f, %.0f, %.0f)",
                        g_npcs.size(),
                        nameOf(actor),
                        transform[12] / kGameToHavok,
                        transform[13] / kGameToHavok,
                        transform[14] / kGameToHavok,
                        position[0],
                        position[1],
                        position[2]);
                    logLimited(message);
                }
            }
        }
    }
}
