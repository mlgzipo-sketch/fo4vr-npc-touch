// LoadScreenBG: puts the flat game's loading screen backdrop behind the
// spinning model in Fallout 4 VR's loading screens.
//
// With bVrLoadingMenu on (the VR default), LoadingMenu attaches the loading
// model to a VR UI node in front of the player and never shows the backdrop
// the flat game draws behind it (LoadScreen02.nif, used only by the flat
// screen-space path). This plugin loads that backdrop, hangs it on the same
// node as the model, and places it behind the model, level with the eyes.
//
// Addresses are for Fallout4VR.exe 1.2.72, read from its decrypted code.

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <REL/Relocation.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>
#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
    // LoadingMenu vtable; slot 4 is AdvanceMovie, which also places the model.
    constexpr std::uintptr_t kLoadingMenuVtable = 0x2DCA698;
    constexpr std::uintptr_t kLoadingMenuAdvanceMovie = 0x1313F60;
    // The node LoadingMenu attaches to the VR UI node (raw NiAVObject*).
    constexpr std::uintptr_t kLoadingModelNode = 0x5B3D210;
    // bVrLoadingMenu:VR value.
    constexpr std::uintptr_t kVrLoadingMenuSetting = 0x3843F58;
    // BSModelDB::Demand(path, handle*, args*): the overload LoadingMenu's
    // backdrop loader uses; the node is at +0x20 of the returned entry.
    constexpr std::uintptr_t kModelDemand = 0x1D0DD80;
    // NiAVObject update the loading menu runs after attaching the model.
    constexpr std::uintptr_t kNodeUpdate = 0x1C22FB0;

    // Half the size of LoadScreen02.nif's MenuBGBack plane and how far it
    // sits in front of the mesh's origin, in mesh units.
    constexpr float kPlaneHalfWidth = 557.5f;
    constexpr float kPlaneDepth = 991.0f;

    struct Settings
    {
        bool enabled{ true };
        std::string model{ "LoadScreenBG\\Background.nif" };
        float distance{ 900.0f };
        float widthDegrees{ 110.0f };
        float heightOffset{ 0.0f };
        bool showOverlay{ false };
        bool transposeRotation{ false };
        bool debugLog{ true };
    } g_settings;

    std::shared_ptr<spdlog::logger> g_logger;

    void logNote(const std::string_view message) noexcept
    {
        try {
            if (g_logger) {
                g_logger->info("{}", message);
            }
        } catch (...) {
        }
    }

    void logInfo(const std::string_view message) noexcept
    {
        if (g_settings.debugLog) {
            logNote(message);
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
        const auto path = std::filesystem::path(documentsPath.get()) / "My Games" / "Fallout4VR" / "F4SE" / "LoadScreenBG.log";
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path.string(), 1024 * 1024, 2, true);
        g_logger = std::make_shared<spdlog::logger>("LoadScreenBG", std::move(sink));
        g_logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
        g_logger->set_level(spdlog::level::info);
        g_logger->flush_on(spdlog::level::info);
    }

    void readSettings() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/LoadScreenBG.ini");
            std::string line;
            while (file && std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const auto equals = line.find('=');
                if (line.empty() || line[0] == ';' || line[0] == '[' || equals == std::string::npos) {
                    continue;
                }
                auto key = line.substr(0, equals);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                    key.pop_back();
                }
                auto value = line.substr(equals + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.erase(value.begin());
                }
                const float number = std::strtof(value.c_str(), nullptr);
                if (key == "bEnabled") {
                    g_settings.enabled = number != 0.0f;
                } else if (key == "sModel") {
                    g_settings.model = value;
                } else if (key == "fDistance") {
                    g_settings.distance = number;
                } else if (key == "fWidthDegrees") {
                    g_settings.widthDegrees = number;
                } else if (key == "fHeightOffset") {
                    g_settings.heightOffset = number;
                } else if (key == "bShowOverlay") {
                    g_settings.showOverlay = number != 0.0f;
                } else if (key == "bTransposeRotation") {
                    g_settings.transposeRotation = number != 0.0f;
                } else if (key == "bDebugLog") {
                    g_settings.debugLog = number != 0.0f;
                }
            }
        } catch (...) {
        }
        if (g_settings.distance < 50.0f) {
            g_settings.distance = 50.0f;
        }
        if (g_settings.widthDegrees < 10.0f || g_settings.widthDegrees > 170.0f) {
            g_settings.widthDegrees = 110.0f;
        }
        char message[320]{};
        std::snprintf(message, sizeof(message), "Settings: enabled=%d model=%s distance=%.0f width=%.0f deg height=%.0f overlay=%d transpose=%d",
            g_settings.enabled, g_settings.model.c_str(), g_settings.distance, g_settings.widthDegrees, g_settings.heightOffset,
            g_settings.showOverlay, g_settings.transposeRotation);
        logNote(message);
    }

    std::uintptr_t base() noexcept
    {
        return REL::Module::get().base();
    }

    const char* nameOf(const RE::NiAVObject* object) noexcept
    {
        if (!object) {
            return "<null>";
        }
        const char* name = object->name.c_str();
        return name && *name ? name : "<unnamed>";
    }

    // SetAppCulled sits at a different vtable slot in VR than in the
    // headers, so the flag is set directly (bit 0 of NiAVObject::flags).
    void setCulled(RE::NiAVObject* object, const bool culled) noexcept
    {
        if (culled) {
            object->flags.flags |= 1;
        } else {
            object->flags.flags &= ~std::uint64_t{ 1 };
        }
    }

    struct Vec
    {
        float x{}, y{}, z{};
    };

    Vec sub(const Vec& a, const Vec& b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec add(const Vec& a, const Vec& b) noexcept { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec mul(const Vec& a, const float s) noexcept { return { a.x * s, a.y * s, a.z * s }; }
    float length(const Vec& a) noexcept { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
    Vec cross(const Vec& a, const Vec& b) noexcept { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
    Vec toVec(const RE::NiPoint3& p) noexcept { return { p.x, p.y, p.z }; }

    // Where the backdrop should stay in the world while one loading screen is up.
    struct Target
    {
        float rotate[3][3]{};  // columns are the mesh's axes in world space
        Vec translate;
        float scale{ 1.0f };
    };

    struct State
    {
        RE::NiAVObject* backdrop{ nullptr };
        bool loadTried{ false };
        RE::NiNode* parent{ nullptr };
        Target target;
        bool loggedNoModel{ false };
    } g_state;

    // Demand returns the cached copy, so the mesh is loaded once and kept.
    RE::NiAVObject* demandModel(const std::string& path) noexcept
    {
        struct Args
        {
            std::int32_t lodFadeMult{ 0 };
            std::uint32_t loadLevel{ 0 };
            std::uint8_t flags{ 0x3D };  // prepare, error marker, process, fade node, textures
            std::uint8_t pad[3]{};
        } args;
        static_assert(sizeof(Args) == 0x0C);
        using Demand_t = std::uint32_t (*)(const char*, void**, const Args*);
        void* entry{ nullptr };
        std::uint32_t result = 0xFFFFFFFF;
        __try {
            result = reinterpret_cast<Demand_t>(base() + kModelDemand)(path.c_str(), &entry, &args);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!entry) {
            char message[320]{};
            std::snprintf(message, sizeof(message), "Mesh %s not loaded (result %u)", path.c_str(), result);
            logNote(message);
            return nullptr;
        }
        auto* node = *reinterpret_cast<RE::NiAVObject**>(reinterpret_cast<std::uint8_t*>(entry) + 0x20);
        char message[320]{};
        std::snprintf(message, sizeof(message), "Mesh %s loaded (result %u): root %s", path.c_str(), result, nameOf(node));
        logNote(message);
        return node;
    }

    void loadBackdrop() noexcept
    {
        if (g_state.loadTried) {
            return;
        }
        g_state.loadTried = true;
        auto* node = g_settings.model.empty() ? nullptr : demandModel(g_settings.model);
        if (!node) {
            node = demandModel("Interface\\Objects\\LoadScreen\\LoadScreen02.nif");
        }
        if (!node) {
            logWarning("No backdrop mesh could be loaded; the loading screen stays as it is");
            return;
        }
        node->IncRefCount();
        g_state.backdrop = node;

        // BSModelDB always hands back a NiNode root.
        {
            auto& children = static_cast<RE::NiNode*>(node)->GetRuntimeData().children;
            for (std::uint16_t i = 0; i < children.capacity(); ++i) {
                auto* child = children[i].get();
                if (!child) {
                    continue;
                }
                const std::string_view name = nameOf(child);
                const bool overlay = name.starts_with("OverlayFlat");
                if (overlay && !g_settings.showOverlay) {
                    setCulled(child, true);
                }
                char message[200]{};
                std::snprintf(message, sizeof(message), "  backdrop part %s%s", name.data(), overlay && !g_settings.showOverlay ? " (hidden)" : "");
                logInfo(message);
            }
        }
    }

    void updateNode(RE::NiAVObject* object) noexcept
    {
        struct UpdateData
        {
            std::uint32_t time{ 0 };
            std::uint64_t a{ 0 };
            std::uint64_t b{ 0 };
            std::uint32_t flags{ 0 };
        } data;
        using Update_t = void (*)(RE::NiAVObject*, UpdateData*);
        __try {
            reinterpret_cast<Update_t>(base() + kNodeUpdate)(object, &data);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // World target: level with the eye point, centred on the line to the
    // model, at fDistance and fWidthDegrees wide.
    Target computeTarget(const RE::NiNode& parent, const RE::NiAVObject& model) noexcept
    {
        const Vec eye = toVec(parent.world.translate);
        Vec center = toVec(model.worldBound.center);
        if (model.worldBound.fRadius <= 0.0f) {
            center = toVec(model.world.translate);
        }
        Vec dir = sub(center, eye);
        dir.z = 0.0f;
        if (length(dir) < 1.0f) {
            // Model straight above or below: use the parent's forward (Y) axis.
            const auto& r = parent.world.rotate.entry;
            dir = { r[0][1], r[1][1], 0.0f };
        }
        if (length(dir) < 1e-4f) {
            dir = { 0.0f, 1.0f, 0.0f };
        }
        dir = mul(dir, 1.0f / length(dir));
        const Vec up{ 0.0f, 0.0f, 1.0f };
        const Vec right = cross(dir, up);

        Target t;
        const float halfAngle = g_settings.widthDegrees * 0.5f * 3.14159265f / 180.0f;
        t.scale = g_settings.distance * std::tan(halfAngle) / kPlaneHalfWidth;
        // The plane sits kPlaneDepth in front of the mesh origin; put the
        // origin back so the plane lands at fDistance.
        t.translate = add(add(eye, mul(dir, g_settings.distance - kPlaneDepth * t.scale)), Vec{ 0.0f, 0.0f, g_settings.heightOffset });
        const Vec axes[3] = { right, dir, up };
        for (int column = 0; column < 3; ++column) {
            t.rotate[0][column] = axes[column].x;
            t.rotate[1][column] = axes[column].y;
            t.rotate[2][column] = axes[column].z;
        }
        return t;
    }

    // Local transform under the parent that keeps the backdrop at its world target.
    void applyTarget(RE::NiAVObject& backdrop, const RE::NiNode& parent, const Target& t) noexcept
    {
        const auto& pr = parent.world.rotate.entry;
        const float ps = parent.world.scale != 0.0f ? parent.world.scale : 1.0f;
        float local[3][3]{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    sum += pr[k][i] * t.rotate[k][j];  // parent^T * target
                }
                local[i][j] = sum;
            }
        }
        const Vec offset = sub(t.translate, toVec(parent.world.translate));
        Vec localPos{};
        localPos.x = (pr[0][0] * offset.x + pr[1][0] * offset.y + pr[2][0] * offset.z) / ps;
        localPos.y = (pr[0][1] * offset.x + pr[1][1] * offset.y + pr[2][1] * offset.z) / ps;
        localPos.z = (pr[0][2] * offset.x + pr[1][2] * offset.y + pr[2][2] * offset.z) / ps;

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                backdrop.local.rotate.entry[i][j] = g_settings.transposeRotation ? local[j][i] : local[i][j];
            }
        }
        backdrop.local.translate = RE::NiPoint3(localPos.x, localPos.y, localPos.z);
        backdrop.local.scale = t.scale / ps;
    }

    void detachBackdrop(const char* why) noexcept
    {
        auto* backdrop = g_state.backdrop;
        if (!backdrop) {
            return;
        }
        __try {
            setCulled(backdrop, true);
            if (backdrop->parent) {
                backdrop->parent->DetachChild(backdrop);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (g_state.parent) {
            logInfo(why);
        }
        g_state.parent = nullptr;
    }

    void afterAdvance() noexcept
    {
        if (!g_settings.enabled || *reinterpret_cast<const bool*>(base() + kVrLoadingMenuSetting) == false) {
            return;
        }
        auto* model = *reinterpret_cast<RE::NiAVObject**>(base() + kLoadingModelNode);
        auto* parent = model ? model->parent : nullptr;
        if (!model || !parent) {
            if (!g_state.loggedNoModel) {
                logInfo("Loading screen has no model attached yet");
                g_state.loggedNoModel = true;
            }
            return;
        }
        loadBackdrop();
        auto* backdrop = g_state.backdrop;
        if (!backdrop) {
            return;
        }
        if (backdrop->parent != parent) {
            if (backdrop->parent) {
                backdrop->parent->DetachChild(backdrop);
            }
            g_state.target = computeTarget(*parent, *model);
            parent->AttachChild(backdrop, true);
            setCulled(backdrop, false);
            g_state.parent = parent;

            char message[512]{};
            std::snprintf(message, sizeof(message),
                "Backdrop attached to %s: eye=(%.0f, %.0f, %.0f) model=%s at (%.0f, %.0f, %.0f) r=%.0f; backdrop origin=(%.0f, %.0f, %.0f) forward=(%.2f, %.2f) scale=%.2f",
                nameOf(parent), parent->world.translate.x, parent->world.translate.y, parent->world.translate.z,
                nameOf(model), model->worldBound.center.x, model->worldBound.center.y, model->worldBound.center.z, model->worldBound.fRadius,
                g_state.target.translate.x, g_state.target.translate.y, g_state.target.translate.z,
                g_state.target.rotate[0][1], g_state.target.rotate[1][1], g_state.target.scale);
            logNote(message);
        }
        applyTarget(*backdrop, *parent, g_state.target);
        updateNode(backdrop);
    }

    using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint64_t);
    AdvanceMovie_t g_originalAdvanceMovie{ nullptr };

    void hookedAdvanceMovie(RE::IMenu* menu, const float timeDelta, const std::uint64_t time)
    {
        g_originalAdvanceMovie(menu, timeDelta, time);
        __try {
            afterAdvance();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    bool hookLoadingMenu() noexcept
    {
        auto* slot = reinterpret_cast<std::uintptr_t*>(base() + kLoadingMenuVtable) + 4;
        if (*slot != base() + kLoadingMenuAdvanceMovie) {
            logWarning("LoadingMenu::AdvanceMovie is not where expected (another mod hooks it, or a different game version); not hooking");
            return false;
        }
        g_originalAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(*slot);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(slot), reinterpret_cast<std::uintptr_t>(&hookedAdvanceMovie));
        logNote("Hooked LoadingMenu::AdvanceMovie (build " LOADSCREENBG_BUILD ")");
        return true;
    }

    class MenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            try {
                if (event.menuName == std::string_view("LoadingMenu")) {
                    if (event.opening) {
                        g_state.loggedNoModel = false;
                        logInfo("LoadingMenu opened");
                    } else {
                        detachBackdrop("LoadingMenu closed; backdrop removed");
                    }
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuWatcher g_menuWatcher;

    void onF4SEMessage(F4SE::MessagingInterface::Message* message) noexcept
    {
        if (!message || message->type != F4SE::MessagingInterface::kGameLoaded) {
            return;
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuWatcher);
        } else {
            logWarning("UI singleton missing; the backdrop is not removed when loading ends");
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
        a_info->infoVersion = F4SE::PluginInfo::kVersion;
        a_info->name = "LoadScreenBG";
        a_info->version = 1;
        if (a_f4se->IsEditor() || !REL::Module::IsVR()) {
            logWarning("Fallout 4 VR runtime is required");
            return false;
        }
        if (REL::Module::get().version() != F4SE::RUNTIME_VR_1_2_72) {
            logWarning("Unsupported Fallout4VR.exe version; 1.2.72 is required");
            return false;
        }
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
        readSettings();
        if (!g_settings.enabled) {
            logNote("Disabled in LoadScreenBG.ini");
            return true;
        }
        const auto* messaging = F4SE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(onF4SEMessage)) {
            logWarning("F4SE messaging registration failed");
        }
        return hookLoadingMenu();
    } catch (...) {
        return false;
    }
}
