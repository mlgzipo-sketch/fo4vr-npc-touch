// Based on the RPS SDK example runtime (github.com/brunocatani/RPS_SDK).
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <REL/Relocation.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>
#include <Windows.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
    using npctouch::runtime::hasLifecycleFlag;

    rock::api::Client g_client;
    const rock::api::core::ApiV1* g_core{};
    std::shared_ptr<spdlog::logger> g_logger;
    std::uint64_t g_ownerToken{ 0 };
    std::uint64_t g_callbackToken{ 0 };

    bool g_verbose{ false };

    bool readDebugLogSetting() noexcept
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
                if (key == "bDebugLog") {
                    return std::strtof(line.c_str() + equals + 1, nullptr) != 0.0f;
                }
            }
        } catch (...) {
        }
        return false;
    }

    void initializeLogger()
    {
        // F4SE::log::log_directory() needs F4SE::Init, which runs after Query,
        // so before that it drops the "Fallout4VR" part of the path.
        wchar_t* documents{ nullptr };
        const auto result = REX::W32::SHGetKnownFolderPath(
            REX::W32::FOLDERID_Documents, REX::W32::KF_FLAG_DEFAULT, nullptr, std::addressof(documents));
        std::unique_ptr<wchar_t[], decltype(&REX::W32::CoTaskMemFree)> documentsPath(documents, REX::W32::CoTaskMemFree);
        if (!documentsPath || result != 0) {
            return;
        }
        auto directory = std::filesystem::path(documentsPath.get()) / "My Games" / "Fallout4VR" / "F4SE";
        const auto& example = npctouch::runtime::definition();
        directory /= std::string(example.pluginName) + ".log";
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            directory.string(),
            2 * 1024 * 1024,
            2,
            true);
        g_logger = std::make_shared<spdlog::logger>(example.pluginName, std::move(sink));
        g_logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
        g_logger->set_level(spdlog::level::info);
        g_logger->flush_on(spdlog::level::info);
        g_verbose = readDebugLogSetting();
    }

    void disconnectRock() noexcept
    {
        const auto& example = npctouch::runtime::definition();
        if (g_ownerToken != 0 && example.onStop) {
            example.onStop(g_ownerToken);
        }
        const auto result=g_client.close();
        if(result!=rock::api::Status::Ok && result!=rock::api::Status::OwnerNotRegistered) {
            npctouch::runtime::logWarning("ROCK owner cleanup was rejected");return;
        }
        g_callbackToken=0;g_ownerToken=0;g_core=nullptr;
    }

    void ROCK_CALL onRockFrame(
        const rock::api::core::SnapshotV1* snapshot,
        void*)
    {
        if (!snapshot || g_ownerToken == 0) {
            return;
        }
        try {
            const auto& example = npctouch::runtime::definition();
            if (example.onFrame) {
                example.onFrame(g_ownerToken, *snapshot);
            }
        } catch (const std::exception& error) {
            npctouch::runtime::logWarning(error.what());
        } catch (...) {
            npctouch::runtime::logWarning("Unhandled frame callback failure");
        }
    }

    bool connectRock() noexcept
    {
        if (g_ownerToken != 0) {
            return true;
        }

        const auto& example = npctouch::runtime::definition();
        const auto module=GetModuleHandleA("ROCK.dll");
        const auto query=module?reinterpret_cast<rock::api::QueryInterfaceV1>(GetProcAddress(module,rock::api::kQueryExportName)):nullptr;
        if(g_client.connect(query,example.pluginName)!=rock::api::Status::Ok)return false;
        if(g_client.acquire(5,g_core)!=rock::api::Status::Ok ||
           (example.onConnect && !example.onConnect(g_client,query))) {
            (void)g_client.close();
            npctouch::runtime::logWarning("A required ROCK interface major or permission is unavailable");return false;
        }
        g_ownerToken=g_client.owner();
        if (example.onStart && !example.onStart(g_ownerToken)) {
            disconnectRock();
            return false;
        }

        const auto callbackResult =
            g_core->registerFrameCallbackForOwnerV1(
                g_ownerToken,
                &onRockFrame,
                nullptr,
                &g_callbackToken);
        if (callbackResult != rock::api::Status::Ok || g_callbackToken == 0) {
            disconnectRock();
            npctouch::runtime::logWarning("ROCK owner frame callback registration failed");
            return false;
        }

        npctouch::runtime::logNote("Connected to independently negotiated ROCK providers");
        return true;
    }

    void onF4SEMessage(F4SE::MessagingInterface::Message* message) noexcept
    {
        if (!message) {
            return;
        }
        switch (message->type) {
        case F4SE::MessagingInterface::kGameLoaded:
        case F4SE::MessagingInterface::kGameDataReady:
        case F4SE::MessagingInterface::kPostLoadGame:
        case F4SE::MessagingInterface::kNewGame:
            (void)connectRock();
            break;
        // The ROCK owner stays registered across save loads: ROCK rejects
        // closing it from the load message, and frames keep flowing anyway.
        default:
            break;
        }
    }
}

namespace npctouch::runtime
{
    void logInfo(const std::string_view message) noexcept
    {
        if (g_verbose) {
            logNote(message);
        }
    }

    void logNote(const std::string_view message) noexcept
    {
        try {
            if (g_logger) {
                g_logger->info("{}", message);
            }
        } catch (...) {
        }
    }

    void logWarning(const std::string_view message) noexcept
    {
        try {
            if (g_logger) {
                g_logger->warn("{}", message);
            }
        } catch (...) {
        }
    }
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info) noexcept
{
    try {
        if (!a_f4se || !a_info) {
            return false;
        }

        initializeLogger();
        const auto& example = npctouch::runtime::definition();
        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = example.pluginName;
        a_info->version = example.pluginVersion;

        if (a_f4se->IsEditor()) {
            npctouch::runtime::logWarning("Editor runtime is unsupported");
            return false;
        }
        if (!REL::Module::IsVR()) {
            npctouch::runtime::logWarning("Fallout 4 VR runtime is required");
            return false;
        }

        const auto executableVersion = REL::Module::get().version();
        if (executableVersion != F4SE::RUNTIME_VR_1_2_72) {
            npctouch::runtime::logWarning(
                "Unsupported Fallout4VR.exe version; 1.2.72 is required");
            return false;
        }

        char message[192]{};
        std::snprintf(
            message,
            sizeof(message),
            "Query passed: F4SE=%s loaderRuntime=%s executable=%s",
            a_f4se->F4SEVersion().string().c_str(),
            a_f4se->RuntimeVersion().string().c_str(),
            executableVersion.string().c_str());
        npctouch::runtime::logNote(message);
        return true;
    } catch (...) {
        return false;
    }
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(
    const F4SE::LoadInterface* a_f4se) noexcept
{
    try {
        if (!a_f4se) {
            return false;
        }
        F4SE::Init(a_f4se, false);
        const auto* messaging = F4SE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(onF4SEMessage)) {
            npctouch::runtime::logWarning("F4SE messaging registration failed");
            return false;
        }
        npctouch::runtime::logNote("Plugin loaded; waiting for game data");
        return true;
    } catch (...) {
        return false;
    }
}
