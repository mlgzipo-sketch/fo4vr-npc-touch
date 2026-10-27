// VRLockpick: pick locks with your own hands in Fallout 4 VR.
// Step 1: watch the game's lockpicking menu, log what it does, and turn the
// bobby pin with the roll of the pick hand instead of the thumbstick.
#include <ROCK/Hands.h>
#include "Runtime.h"

#include <RE/Fallout.h>
#include <REL/Relocation.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace
{
    namespace hands = rock::api::hands;
    using Clock = std::chrono::steady_clock;

    constexpr float kPi = 3.14159265f;

    struct Settings
    {
        bool drivePick{ true };
        bool leftHandPick{ false };
        float pickDegreesPerHandDegree{ 1.0f };
        float maxPickAngle{ 90.0f };
    };
    Settings g_settings;

    const hands::ApiV1* g_hands{};
    std::uint64_t g_ownerToken{ 0 };
    std::uint64_t g_rockFrames{ 0 };

    struct Vec3
    {
        float x{}, y{}, z{};
    };
    float dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross(const Vec3& a, const Vec3& b) noexcept
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    // ROCK hand frame: rotate is row-major and world = R^T * v, so row 0 is
    // the hand's +X (along the fingers) in world space and row 1 its +Y.
    struct HandPose
    {
        Vec3 position;
        Vec3 alongFingers;
        Vec3 side;
    };

    bool readHand(const bool left, HandPose& out) noexcept
    {
        if (!g_hands || g_ownerToken == 0) {
            return false;
        }
        hands::HandFrameV1 frame{};
        const auto hand = left ? rock::api::Hand::Left : rock::api::Hand::Right;
        if (g_hands->getHandFrameV1(g_ownerToken, hand, &frame) != rock::api::Status::Ok ||
            (frame.flags & static_cast<std::uint32_t>(hands::HandFrameFlagV1::Valid)) == 0) {
            return false;
        }
        const auto& r = frame.transform.rotate;
        out.position = { frame.transform.translate[0], frame.transform.translate[1], frame.transform.translate[2] };
        out.alongFingers = { r[0], r[1], r[2] };
        out.side = { r[3], r[4], r[5] };
        return true;
    }

    // How far the hand has rolled around its finger axis since the reference
    // pose, in degrees (twist part of the rotation).
    float rollDegrees(const HandPose& reference, const HandPose& now) noexcept
    {
        const Vec3& axis = now.alongFingers;
        const float along = dot(reference.side, axis);
        const Vec3 projected{ reference.side.x - axis.x * along, reference.side.y - axis.y * along, reference.side.z - axis.z * along };
        const float sine = dot(axis, cross(projected, now.side));
        const float cosine = dot(projected, now.side);
        return std::atan2(sine, cosine) * 180.0f / kPi;
    }

    void readSettings() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/VRLockpick.ini");
            std::string line;
            while (file && std::getline(file, line)) {
                const auto equals = line.find('=');
                if (line.empty() || line[0] == ';' || line[0] == '[' || equals == std::string::npos) {
                    continue;
                }
                auto key = line.substr(0, equals);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                    key.pop_back();
                }
                const float value = std::strtof(line.c_str() + equals + 1, nullptr);
                if (key == "bDrivePick") {
                    g_settings.drivePick = value != 0.0f;
                } else if (key == "bLeftHandPick") {
                    g_settings.leftHandPick = value != 0.0f;
                } else if (key == "fPickDegreesPerHandDegree") {
                    g_settings.pickDegreesPerHandDegree = value;
                } else if (key == "fMaxPickAngle") {
                    g_settings.maxPickAngle = value;
                }
            }
        } catch (...) {
        }
        char message[192]{};
        std::snprintf(message, sizeof(message), "Settings: bDrivePick=%d bLeftHandPick=%d fPickDegreesPerHandDegree=%.2f fMaxPickAngle=%.1f",
            g_settings.drivePick ? 1 : 0, g_settings.leftHandPick ? 1 : 0, g_settings.pickDegreesPerHandDegree, g_settings.maxPickAngle);
        npctouch::runtime::logNote(message);
    }

    // State of one lockpicking session (menu open to close).
    struct Session
    {
        bool active{ false };
        bool haveReference{ false };
        HandPose reference;
        std::uint64_t menuFrames{ 0 };
        std::uint64_t rockFramesAtOpen{ 0 };
        Clock::time_point lastLog{};
        float lastPickAngle{ 0.0f };
        float lastLockAngle{ 0.0f };
        bool lastTurning{ false };
        std::uint32_t lastPicksBroken{ 0 };
    };
    Session g_session;

    void logPoint(const char* label, const RE::NiAVObject* object) noexcept
    {
        char message[256]{};
        if (!object) {
            std::snprintf(message, sizeof(message), "  %s: none", label);
        } else {
            const auto& t = object->world.translate;
            std::snprintf(message, sizeof(message), "  %s: world (%.1f, %.1f, %.1f) scale %.3f", label, t.x, t.y, t.z, object->world.scale);
        }
        npctouch::runtime::logNote(message);
    }

    void logMenuState(const char* why, const RE::LockpickingMenu& menu) noexcept
    {
        char message[320]{};
        std::snprintf(message, sizeof(message),
            "%s: pickAngle=%.2f lockAngle=%.2f sweetSpot=%.2f len=%.2f partial=%.2f damageAngle=%.2f turning=%d animating=%d picksBroken=%u modelsInit=%d",
            why, menu.pickAngle, menu.lockAngle, menu.sweetSpotCenter, menu.sweetSpotLength, menu.partialPickLength,
            menu.lockDamagePickAngle, menu.turningLock ? 1 : 0, menu.animating ? 1 : 0, menu.picksBroken, menu.modelsInit ? 1 : 0);
        npctouch::runtime::logNote(message);
    }

    void onMenuFrame(RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        const auto now = Clock::now();
        if (!s.active) {
            s = Session{};
            s.active = true;
            s.rockFramesAtOpen = g_rockFrames;
            s.lastLog = now;
            logMenuState("Lockpicking opened", menu);
            logPoint("lockRoot", menu.lockRoot.get());
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                logPoint("lockPickParentNode", player->vrNodes.lockPickParentNode);
                logPoint("primaryWand", player->vrNodes.primaryWandNode);
                logPoint("hmd", player->vrNodes.hmdNode);
            }
            HandPose right, left;
            if (readHand(false, right)) {
                char message[160]{};
                std::snprintf(message, sizeof(message), "  right hand: (%.1f, %.1f, %.1f)", right.position.x, right.position.y, right.position.z);
                npctouch::runtime::logNote(message);
            }
            if (readHand(true, left)) {
                char message[160]{};
                std::snprintf(message, sizeof(message), "  left hand: (%.1f, %.1f, %.1f)", left.position.x, left.position.y, left.position.z);
                npctouch::runtime::logNote(message);
            }
        }
        ++s.menuFrames;

        float handRoll = 0.0f;
        bool haveHand = false;
        HandPose pose;
        if (readHand(g_settings.leftHandPick, pose)) {
            if (!s.haveReference) {
                s.reference = pose;
                s.haveReference = true;
            }
            handRoll = rollDegrees(s.reference, pose);
            haveHand = true;
        }

        // Don't fight the game's own intro/break animations.
        if (g_settings.drivePick && haveHand && !menu.animating && !menu.turningLock) {
            float angle = handRoll * g_settings.pickDegreesPerHandDegree;
            if (angle > g_settings.maxPickAngle) {
                angle = g_settings.maxPickAngle;
            } else if (angle < -g_settings.maxPickAngle) {
                angle = -g_settings.maxPickAngle;
            }
            menu.pickAngle = angle;
        }

        const bool changed = menu.turningLock != s.lastTurning || menu.picksBroken != s.lastPicksBroken;
        if (changed) {
            logMenuState(menu.picksBroken != s.lastPicksBroken ? "Pick broke" : (menu.turningLock ? "Lock turn started" : "Lock turn stopped"), menu);
        }
        if (now - s.lastLog > std::chrono::milliseconds(500) &&
            (std::fabs(menu.pickAngle - s.lastPickAngle) > 0.5f || std::fabs(menu.lockAngle - s.lastLockAngle) > 0.5f)) {
            char message[256]{};
            std::snprintf(message, sizeof(message), "  pick=%.1f lock=%.1f handRoll=%.1f (hand %s) sweet=%.1f+-%.1f",
                menu.pickAngle, menu.lockAngle, handRoll, haveHand ? "ok" : "missing", menu.sweetSpotCenter, menu.sweetSpotLength * 0.5f);
            npctouch::runtime::logInfo(message);
            s.lastLog = now;
        }
        s.lastPickAngle = menu.pickAngle;
        s.lastLockAngle = menu.lockAngle;
        s.lastTurning = menu.turningLock;
        s.lastPicksBroken = menu.picksBroken;
    }

    // LockpickingMenu::AdvanceMovie (vtable slot 4) runs every frame the menu is up.
    using AdvanceMovie_t = void (*)(RE::LockpickingMenu*, float, std::uint64_t);
    AdvanceMovie_t g_originalAdvanceMovie{ nullptr };

    void hookedAdvanceMovie(RE::LockpickingMenu* menu, const float timeDelta, const std::uint64_t time)
    {
        g_originalAdvanceMovie(menu, timeDelta, time);
        if (menu) {
            try {
                onMenuFrame(*menu);
            } catch (...) {
            }
        }
    }

    // The vtable is taken from the live menu, so no address library entry is needed.
    void hookMenu(RE::IMenu* menu) noexcept
    {
        if (g_originalAdvanceMovie || !menu) {
            return;
        }
        auto* vtable = *reinterpret_cast<std::uintptr_t**>(menu);
        g_originalAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(vtable[4]);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[4]), reinterpret_cast<std::uintptr_t>(&hookedAdvanceMovie));
        npctouch::runtime::logNote("Hooked LockpickingMenu::AdvanceMovie");
    }

    class MenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            try {
                if (event.menuName == RE::LockpickingMenu::MENU_NAME) {
                    if (event.opening) {
                        npctouch::runtime::logNote("LockpickingMenu open event");
                        if (auto* ui = RE::UI::GetSingleton()) {
                            const auto menu = ui->GetMenu(RE::LockpickingMenu::MENU_NAME);
                            if (!menu) {
                                npctouch::runtime::logWarning("LockpickingMenu not found in the menu map yet");
                            }
                            hookMenu(menu.get());
                        }
                    } else {
                        char message[160]{};
                        std::snprintf(message, sizeof(message), "Lockpicking closed after %llu menu frames (ROCK frames during it: %llu)",
                            static_cast<unsigned long long>(g_session.menuFrames),
                            static_cast<unsigned long long>(g_rockFrames - g_session.rockFramesAtOpen));
                        npctouch::runtime::logNote(message);
                        g_session.active = false;
                    }
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuWatcher g_menuWatcher;

    bool connect(rock::api::Client& client, rock::api::QueryInterfaceV1) noexcept
    {
        if (client.acquire(static_cast<std::uint32_t>(hands::PermissionV1::Read), g_hands) != rock::api::Status::Ok) {
            g_hands = nullptr;
            npctouch::runtime::logWarning("ROCK hands interface unavailable; the pick can't follow the hand");
        }
        return true;
    }

    bool start(const std::uint64_t ownerToken) noexcept
    {
        g_ownerToken = ownerToken;
        static bool ready = false;
        if (!ready) {
            readSettings();
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuWatcher);
                npctouch::runtime::logNote("Watching for the lockpicking menu (build " VRLOCKPICK_BUILD ")");
            } else {
                npctouch::runtime::logWarning("UI singleton missing; lockpicking menu not watched");
            }
            ready = true;
        }
        return true;
    }

    void stop(const std::uint64_t) noexcept
    {
        g_ownerToken = 0;
    }

    void frame(const std::uint64_t, const rock::api::core::SnapshotV1&) noexcept
    {
        ++g_rockFrames;
    }
}

namespace npctouch::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "VRLockpick",
            .pluginVersion = 1,
            .onConnect = &connect,
            .onStart = &start,
            .onStop = &stop,
            .onFrame = &frame,
        };
        return value;
    }
}
