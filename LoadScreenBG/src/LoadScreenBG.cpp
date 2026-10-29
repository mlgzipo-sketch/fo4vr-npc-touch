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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
    // VR UI object whose node +0x28 ("World") LoadingMenu hangs the model on.
    constexpr std::uintptr_t kVrLoadingScene = 0x5ACA408;
    // BSModelDB::Demand(path, handle*, args*): the overload LoadingMenu's
    // backdrop loader uses; the node is at +0x20 of the returned entry.
    constexpr std::uintptr_t kModelDemand = 0x1D0DD80;
    // NiAVObject update the loading menu runs after attaching the model.
    constexpr std::uintptr_t kNodeUpdate = 0x1C22FB0;

    // Half the size of LoadScreen02.nif's MenuBGBack plane and how far it
    // sits in front of the mesh's origin, in mesh units.
    constexpr float kPlaneHalfWidth = 557.5f;
    // (MenuBGBack's vertices sit at 991 plus its own 9 unit offset.)
    constexpr float kPlaneDepth = 1000.0f;

    struct Settings
    {
        bool enabled{ true };
        std::string model{ "LoadScreenBG\\Background.nif" };
        float distance{ 900.0f };
        float widthDegrees{ 110.0f };
        float heightOffset{ 0.0f };
        bool showOverlay{ false };
        int rotationMode{ 0 };
        bool drawFirst{ true };
        bool behindText{ true };
        float aimBlend{ 0.5f };
        bool dumpScene{ true };
        bool hudDepth{ true };
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
                } else if (key == "fAimBlend") {
                    g_settings.aimBlend = number;
                } else if (key == "bDumpScene") {
                    g_settings.dumpScene = number != 0.0f;
                } else if (key == "bHudDepth") {
                    g_settings.hudDepth = number != 0.0f;
                } else if (key == "bBehindText") {
                    g_settings.behindText = number != 0.0f;
                } else if (key == "bDrawFirst") {
                    g_settings.drawFirst = number != 0.0f;
                } else if (key == "iRotationMode") {
                    g_settings.rotationMode = static_cast<int>(number);
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
        std::snprintf(message, sizeof(message), "Settings: enabled=%d model=%s distance=%.0f width=%.0f deg height=%.0f aim=%.2f behindText=%d overlay=%d rotationMode=%d",
            g_settings.enabled, g_settings.model.c_str(), g_settings.distance, g_settings.widthDegrees, g_settings.heightOffset,
            g_settings.aimBlend, g_settings.behindText, g_settings.showOverlay, g_settings.rotationMode);
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
        Vec axes[3];  // the mesh's X (right), Y (away from the eye) and Z (up) in world space
        Vec translate;  // mesh origin
        Vec planeCenter;
        float scale{ 1.0f };
    };

    // How NiTransform reads its matrix: 1 = axes in columns, parent * local
    // (as in the CommonLib headers); 2 = axes in rows, local * parent.
    // Picked on the first attach by checking where the engine puts the plane.
    enum Convention : int
    {
        kColumns = 1,
        kRows = 2,
    };

    struct State
    {
        RE::NiAVObject* backdrop{ nullptr };
        RE::NiAVObject* plane{ nullptr };
        int convention{ 0 };
        bool loadTried{ false };
        RE::NiNode* parent{ nullptr };
        Target target;
        bool loggedNoModel{ false };
        // Text quads changed by protectText, with their original flags.
        struct HudQuad
        {
            std::uint8_t* shader{ nullptr };
            std::uint64_t shaderFlags{ 0 };
            std::uint8_t* alpha{ nullptr };
            std::uint16_t alphaFlags{ 0 };
            std::int8_t alphaRef{ 0 };
        } hud[4];
        int hudCount{ 0 };
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
                if (name.starts_with("MenuBGBack")) {
                    g_state.plane = child;
                }
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

    // World target: level with the eye point, centred between the loading
    // text and the model (fAimBlend), at fDistance and fWidthDegrees wide.
    Target computeTarget(const RE::NiNode& parent, const RE::NiAVObject& model, const RE::NiAVObject* text) noexcept
    {
        const Vec eye = toVec(parent.world.translate);
        Vec center = toVec(model.worldBound.center);
        if (model.worldBound.fRadius <= 0.0f) {
            center = toVec(model.world.translate);
        }
        if (text) {
            const float blend = std::clamp(g_settings.aimBlend, 0.0f, 1.0f);
            center = add(mul(center, 1.0f - blend), mul(toVec(text->world.translate), blend));
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
        t.planeCenter = add(add(eye, mul(dir, g_settings.distance)), Vec{ 0.0f, 0.0f, g_settings.heightOffset });
        t.translate = sub(t.planeCenter, mul(dir, kPlaneDepth * t.scale));
        t.axes[0] = right;
        t.axes[1] = dir;
        t.axes[2] = up;
        return t;
    }

    // Local transform under the parent that keeps the backdrop at its world target.
    void applyTarget(RE::NiAVObject& backdrop, const RE::NiNode& parent, const Target& t, const int convention) noexcept
    {
        float p[3][3]{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                p[i][j] = parent.world.rotate.entry[i][j];
            }
        }
        const float ps = parent.world.scale != 0.0f ? parent.world.scale : 1.0f;
        const Vec d = sub(t.translate, toVec(parent.world.translate));
        const float dv[3] = { d.x, d.y, d.z };
        float target[3][3]{};
        for (int axis = 0; axis < 3; ++axis) {
            const float a[3] = { t.axes[axis].x, t.axes[axis].y, t.axes[axis].z };
            for (int k = 0; k < 3; ++k) {
                if (convention == kRows) {
                    target[axis][k] = a[k];
                } else {
                    target[k][axis] = a[k];
                }
            }
        }
        float localPos[3]{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    // columns: parent^T * target; rows: target * parent^T
                    sum += convention == kRows ? target[i][k] * p[j][k] : p[k][i] * target[k][j];
                }
                backdrop.local.rotate.entry[i][j] = sum;
            }
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                // columns: parent^T * d; rows: d * parent^T
                sum += convention == kRows ? dv[k] * p[i][k] : p[k][i] * dv[k];
            }
            localPos[i] = sum / ps;
        }
        backdrop.local.translate = RE::NiPoint3(localPos[0], localPos[1], localPos[2]);
        backdrop.local.scale = t.scale / ps;
    }

    // How far the engine put the plane from where it was meant to be.
    float placementError(const Target& t) noexcept
    {
        if (!g_state.plane) {
            return -1.0f;
        }
        return length(sub(toVec(g_state.plane->worldBound.center), t.planeCenter));
    }

    void packChildren(RE::NiNode& node, RE::NiAVObject* backdrop) noexcept;
    void restoreText() noexcept;

    void detachBackdrop(const char* why) noexcept
    {
        auto* backdrop = g_state.backdrop;
        if (!backdrop) {
            return;
        }
        __try {
            setCulled(backdrop, true);
            if (auto* parent = backdrop->parent) {
                parent->DetachChild(backdrop);
                packChildren(*parent, nullptr);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        restoreText();
        if (g_state.parent) {
            logInfo(why);
        }
        g_state.parent = nullptr;
    }

    // Tries both readings and keeps the one that lands the plane on target.
    void pickConvention(RE::NiAVObject& backdrop, const RE::NiNode& parent) noexcept
    {
        const auto& p = parent.world.rotate.entry;
        char message[400]{};
        std::snprintf(message, sizeof(message), "Parent rotation rows: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f) scale=%.2f; plane target=(%.0f, %.0f, %.0f)",
            p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0], p[2][1], p[2][2], parent.world.scale,
            g_state.target.planeCenter.x, g_state.target.planeCenter.y, g_state.target.planeCenter.z);
        logNote(message);

        float errors[3]{ -1.0f, -1.0f, -1.0f };
        for (const int convention : { kColumns, kRows }) {
            applyTarget(backdrop, parent, g_state.target, convention);
            updateNode(&backdrop);
            errors[convention] = placementError(g_state.target);
            const auto c = g_state.plane ? toVec(g_state.plane->worldBound.center) : Vec{};
            std::snprintf(message, sizeof(message), "  reading %s: plane at (%.0f, %.0f, %.0f), %.0f units off",
                convention == kColumns ? "columns" : "rows", c.x, c.y, c.z, errors[convention]);
            logNote(message);
        }
        int chosen = errors[kRows] >= 0.0f && errors[kRows] < errors[kColumns] ? kRows : kColumns;
        if (g_settings.rotationMode == kColumns || g_settings.rotationMode == kRows) {
            chosen = g_settings.rotationMode;
        }
        g_state.convention = chosen;
        applyTarget(backdrop, parent, g_state.target, chosen);
        updateNode(&backdrop);
        logNote(chosen == kRows ? "Using the rows reading" : "Using the columns reading");
    }

    const char* rttiName(const void* object) noexcept
    {
        __try {
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(object);
            const auto col = *reinterpret_cast<const std::uintptr_t*>(vtable - 8);
            const auto typeRva = *reinterpret_cast<const std::uint32_t*>(col + 12);
            return reinterpret_cast<const char*>(base() + typeRva + 0x10);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return "?";
        }
    }

    void dumpNode(const RE::NiAVObject* object, const int depth) noexcept
    {
        if (!object || depth > 7) {
            return;
        }
        char message[360]{};
        std::snprintf(message, sizeof(message), "  %*s%s [%s] at (%.0f, %.0f, %.0f)%s", depth * 2, "", nameOf(object), rttiName(object),
            object->world.translate.x, object->world.translate.y, object->world.translate.z, (object->flags.flags & 1) ? " culled" : "");
        logInfo(message);
        if (std::string_view(rttiName(object)).find("Node") == std::string_view::npos) {
            return;
        }
        const auto& children = static_cast<const RE::NiNode*>(object)->GetRuntimeData().children;
        for (std::uint16_t i = 0; i < children.capacity() && i < 64; ++i) {
            dumpNode(children[i].get(), depth + 1);
        }
    }

    // One-off map of the loading scene, to find where the loading text lives.
    void dumpScene(const RE::NiNode& parent) noexcept
    {
        static bool done = false;
        if (done || !g_settings.dumpScene) {
            return;
        }
        done = true;
        const RE::NiAVObject* top = &parent;
        while (top->parent) {
            top = top->parent;
        }
        logInfo("Scene above the loading model:");
        dumpNode(top, 0);
        const auto* scene = *reinterpret_cast<std::uint8_t* const*>(base() + kVrLoadingScene);
        if (!scene) {
            return;
        }
        char message[200]{};
        std::snprintf(message, sizeof(message), "VR loading scene object [%s]:", rttiName(scene));
        logInfo(message);
        for (int offset = 0x08; offset < 0x200; offset += 8) {
            const auto* field = *reinterpret_cast<RE::NiAVObject* const*>(scene + offset);
            if (reinterpret_cast<std::uintptr_t>(field) < 0x10000) {
                continue;
            }
            const char* type = rttiName(field);
            if (std::string_view(type).starts_with(".?AV")) {
                std::snprintf(message, sizeof(message), "  +0x%X [%s]", offset, type);
                logInfo(message);
            }
        }
    }

    // The loading text and tips are drawn on HUD quads under a BSOrderedNode
    // in one of World's children (named Point002, like the light). The quads
    // don't write depth and are drawn before the backdrop, so the backdrop
    // has to go into that ordered node ahead of them to stay behind the text.
    RE::NiNode* findTextNode(RE::NiNode& parent, RE::NiAVObject* model, RE::NiNode** ordered) noexcept
    {
        auto& children = parent.GetRuntimeData().children;
        for (std::uint16_t i = 0; i < children.capacity(); ++i) {
            auto* child = children[i].get();
            if (!child || child == model || child == g_state.backdrop || std::string_view(rttiName(child)).find("Node") == std::string_view::npos) {
                continue;
            }
            auto& inner = static_cast<RE::NiNode*>(child)->GetRuntimeData().children;
            for (std::uint16_t j = 0; j < inner.capacity(); ++j) {
                auto* candidate = inner[j].get();
                if (candidate && std::string_view(rttiName(candidate)).find("BSOrderedNode") != std::string_view::npos) {
                    *ordered = static_cast<RE::NiNode*>(candidate);
                    return static_cast<RE::NiNode*>(child);
                }
            }
        }
        return nullptr;
    }

    // Packs a node's children to the front in their current order, with the
    // backdrop (when attached there) first or, without bDrawFirst, last.
    // Swapping slots would also swap the text's own shadow and glass quads,
    // which puts the shadow over the text; packing keeps their order, and
    // running it again after a detach leaves the node as the game built it.
    void packChildren(RE::NiNode& node, RE::NiAVObject* backdrop) noexcept
    {
        auto& children = node.GetRuntimeData().children;
        const std::uint16_t capacity = children.capacity();
        if (capacity == 0) {
            return;
        }
        auto** raw = reinterpret_cast<RE::NiAVObject**>(&children[0]);
        RE::NiAVObject* others[64]{};
        std::uint16_t count = 0;
        bool hasBackdrop = false;
        for (std::uint16_t i = 0; i < capacity; ++i) {
            if (raw[i] == backdrop && backdrop) {
                hasBackdrop = true;
            } else if (raw[i] && count < 64) {
                others[count++] = raw[i];
            }
        }
        if (count + (hasBackdrop ? 1 : 0) > capacity || count == 64) {
            return;
        }
        std::uint16_t slot = 0;
        if (hasBackdrop && g_settings.drawFirst) {
            raw[slot++] = backdrop;
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            raw[slot++] = others[i];
        }
        if (hasBackdrop && !g_settings.drawFirst) {
            raw[slot++] = backdrop;
        }
        for (std::uint16_t i = slot; i < capacity; ++i) {
            raw[i] = nullptr;
        }
        // NiTArray keeps one past the last used slot at +0x12 (_freeIdx).
        *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(&children) + 0x12) = slot;
        for (std::uint16_t i = 0; i < slot; ++i) {
            char message[200]{};
            std::snprintf(message, sizeof(message), "  %s child %u: %s", nameOf(&node), i, nameOf(raw[i]));
            logInfo(message);
        }
    }

    // The text quads are drawn before the backdrop whatever their order, and
    // don't write depth, so the backdrop paints over them. With bHudDepth the
    // quads write depth while the loading screen is up, alpha tested so only
    // the drawn pixels do; the backdrop tests depth and stays behind them.
    // BSShaderProperty keeps its 64-bit flags at +0x30.
    constexpr std::uint64_t kShaderZTest = std::uint64_t{ 1 } << 31;
    constexpr std::uint64_t kShaderZWrite = std::uint64_t{ 1 } << 32;
    constexpr std::uint16_t kAlphaTest = 1 << 9;
    constexpr std::uint16_t kAlphaTestGreater = 4 << 10;
    constexpr std::uint16_t kAlphaTestMask = 7 << 10;

    void protectText(RE::NiNode& ordered) noexcept
    {
        if (!g_settings.hudDepth || g_state.hudCount > 0) {
            return;
        }
        auto& children = ordered.GetRuntimeData().children;
        for (std::uint16_t i = 0; i < children.capacity() && g_state.hudCount < 4; ++i) {
            auto* quad = children[i].get();
            if (!quad || quad == g_state.backdrop || !std::string_view(nameOf(quad)).starts_with("HUD")) {
                continue;
            }
            auto* bytes = reinterpret_cast<std::uint8_t*>(quad);
            // The property slots sit elsewhere in VR's BSGeometry, so look
            // for them by type past the NiAVObject part.
            std::uint8_t* alpha{ nullptr };
            std::uint8_t* shader{ nullptr };
            for (int offset = 0x120; offset < 0x1C0; offset += 8) {
                auto* field = *reinterpret_cast<std::uint8_t**>(bytes + offset);
                if (reinterpret_cast<std::uintptr_t>(field) < 0x10000) {
                    continue;
                }
                const std::string_view type = rttiName(field);
                if (!type.starts_with(".?AV")) {
                    continue;
                }
                char line[200]{};
                std::snprintf(line, sizeof(line), "    %s +0x%X [%.80s]", nameOf(quad), offset, type.data());
                logNote(line);
                if (!alpha && type.find("NiAlphaProperty") != std::string_view::npos) {
                    alpha = field;
                } else if (!shader && type.find("ShaderProperty") != std::string_view::npos) {
                    shader = field;
                }
            }
            const std::string_view alphaType = alpha ? rttiName(alpha) : "none";
            const std::string_view shaderType = shader ? rttiName(shader) : "none";
            auto& saved = g_state.hud[g_state.hudCount];
            char message[400]{};
            if (shaderType.find("ShaderProperty") != std::string_view::npos) {
                auto& flags = *reinterpret_cast<std::uint64_t*>(shader + 0x30);
                saved.shader = shader;
                saved.shaderFlags = flags;
                flags |= kShaderZWrite | kShaderZTest;
            }
            if (alphaType.find("NiAlphaProperty") != std::string_view::npos) {
                auto& flags = *reinterpret_cast<std::uint16_t*>(alpha + 0x28);
                auto& ref = *reinterpret_cast<std::int8_t*>(alpha + 0x2A);
                saved.alpha = alpha;
                saved.alphaFlags = flags;
                saved.alphaRef = ref;
                flags = static_cast<std::uint16_t>((flags & ~kAlphaTestMask) | kAlphaTest | kAlphaTestGreater);
                ref = 8;
            }
            std::snprintf(message, sizeof(message), "  text quad %s: shader [%.60s] flags %016llX, alpha [%.40s] flags %04X ref %d%s",
                nameOf(quad), shaderType.data(), static_cast<unsigned long long>(saved.shaderFlags), alphaType.data(),
                saved.alphaFlags, saved.alphaRef, saved.shader ? "; now writes depth" : "; left as is");
            logNote(message);
            if (saved.shader || saved.alpha) {
                ++g_state.hudCount;
            }
        }
    }

    void restoreText() noexcept
    {
        __try {
            for (int i = 0; i < g_state.hudCount; ++i) {
                auto& saved = g_state.hud[i];
                if (saved.shader) {
                    *reinterpret_cast<std::uint64_t*>(saved.shader + 0x30) = saved.shaderFlags;
                }
                if (saved.alpha) {
                    *reinterpret_cast<std::uint16_t*>(saved.alpha + 0x28) = saved.alphaFlags;
                    *reinterpret_cast<std::int8_t*>(saved.alpha + 0x2A) = saved.alphaRef;
                }
                saved = {};
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_state.hudCount = 0;
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
        RE::NiNode* ordered{ nullptr };
        auto* text = findTextNode(*parent, model, &ordered);
        auto* host = g_settings.behindText && ordered ? ordered : parent;
        if (backdrop->parent != host) {
            if (auto* previous = backdrop->parent) {
                previous->DetachChild(backdrop);
                packChildren(*previous, nullptr);
            }
            g_state.target = computeTarget(*parent, *model, text);
            host->AttachChild(backdrop, true);
            packChildren(*host, backdrop);
            if (ordered) {
                protectText(*ordered);
            }
            dumpScene(*parent);
            setCulled(backdrop, false);
            g_state.parent = host;
            g_state.convention = 0;

            char message[640]{};
            std::snprintf(message, sizeof(message),
                "Backdrop attached to %s: eye=(%.0f, %.0f, %.0f) model=%s at (%.0f, %.0f, %.0f) r=%.0f; text %s at (%.0f, %.0f, %.0f); backdrop origin=(%.0f, %.0f, %.0f) forward=(%.2f, %.2f) scale=%.2f",
                nameOf(host), parent->world.translate.x, parent->world.translate.y, parent->world.translate.z,
                nameOf(model), model->worldBound.center.x, model->worldBound.center.y, model->worldBound.center.z, model->worldBound.fRadius,
                text ? nameOf(text) : "not found", text ? text->world.translate.x : 0.0f, text ? text->world.translate.y : 0.0f, text ? text->world.translate.z : 0.0f,
                g_state.target.translate.x, g_state.target.translate.y, g_state.target.translate.z,
                g_state.target.axes[1].x, g_state.target.axes[1].y, g_state.target.scale);
            logNote(message);
        }
        if (g_state.convention == 0) {
            pickConvention(*backdrop, *host);
            return;
        }
        applyTarget(*backdrop, *host, g_state.target, g_state.convention);
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
