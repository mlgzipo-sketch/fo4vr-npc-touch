// Based on the RPS SDK example runtime (github.com/brunocatani/RPS_SDK).
#pragma once

#include <ROCK/Client.h>

#include <cstdint>
#include <string_view>

namespace npctouch::runtime
{
    constexpr bool hasLifecycleFlag(std::uint32_t flags,rock::api::core::LifecycleFlag flag) noexcept {
        return (flags&static_cast<std::uint32_t>(flag))!=0;
    }
    struct Definition
    {
        const char* pluginName{ nullptr };
        std::uint32_t pluginVersion{ 1 };
        bool (*onConnect)(rock::api::Client&,rock::api::QueryInterfaceV1) noexcept{ nullptr };
        bool (*onStart)(std::uint64_t ownerToken) noexcept{ nullptr };
        void (*onStop)(std::uint64_t ownerToken) noexcept{ nullptr };
        void (*onFrame)(
            std::uint64_t ownerToken,
            const rock::api::core::SnapshotV1& snapshot) noexcept{ nullptr };
    };

    [[nodiscard]] const Definition& definition() noexcept;
    // Detail for testing; written only with bDebugLog=1 in NpcTouch.ini.
    void logInfo(std::string_view message) noexcept;
    // Startup and settings lines, always written.
    void logNote(std::string_view message) noexcept;
    void logWarning(std::string_view message) noexcept;
}
