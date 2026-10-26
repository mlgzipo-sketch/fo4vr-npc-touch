#include "HandGrip.h"

#include "Runtime.h"

#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace npctouch::grip
{
    namespace
    {
        using frik::api::FRIKApiV2;

        constexpr const char* kTag = "NpcTouch_Grab";
        // Above FRIK's own poses (90) and ROCK's grab and contact claims (100).
        constexpr int kPriority = 120;

        bool g_enabled{ true };
        bool g_pinEnabled{ true };
        bool g_pinned[2]{};
        bool g_pinLogged{ false };
        float g_curlScale{ 1.0f };
        bool g_tried{ false };
        bool g_holding[2]{};
        const char* g_bone[2]{};

        const FRIKApiV2* api() noexcept
        {
            if (!g_tried) {
                g_tried = true;
                const int error = FRIKApiV2::initialize(1);
                char message[160]{};
                if (error == 0) {
                    std::snprintf(
                        message,
                        sizeof(message),
                        "Hand grip: FRIK %s, API v2.%u; fingers wrap what a hand holds",
                        FRIKApiV2::inst->getModVersion(),
                        FRIKApiV2::inst->getVersion());
                } else {
                    std::snprintf(message, sizeof(message), "Hand grip: FRIK API v2 unavailable (error %d); fingers stay as they are", error);
                }
                npctouch::runtime::logNote(message);
            }
            return FRIKApiV2::inst;
        }

        // How far the fingers close for the part held: thin parts (wrist,
        // forearm, neck) take a firm wrap, the upper arm a looser one, the
        // head a cupped hand and the body a palm laid flat.
        FRIKApiV2::HandPoseData poseFor(const char* bone) noexcept
        {
            float prox = 0.15f, mid = 0.2f, dist = 0.15f, thumb = 0.1f;
            if (bone) {
                if (std::strstr(bone, "_Hand") || std::strstr(bone, "ForeArm") || std::strstr(bone, "Leg_Calf") || std::strstr(bone, "Leg_Foot")) {
                    prox = 0.75f, mid = 0.8f, dist = 0.6f, thumb = 0.6f;
                } else if (std::strstr(bone, "UpperArm") || std::strstr(bone, "Leg_Thigh")) {
                    prox = 0.55f, mid = 0.6f, dist = 0.45f, thumb = 0.45f;
                } else if (std::strcmp(bone, "Neck") == 0) {
                    prox = 0.6f, mid = 0.65f, dist = 0.5f, thumb = 0.5f;
                } else if (std::strcmp(bone, "Head") == 0) {
                    prox = 0.35f, mid = 0.4f, dist = 0.3f, thumb = 0.3f;
                }
            }
            // FRIK's finger values run from 0 (closed) to 1 (open).
            const auto scaled = [](const float curl) { return 1.0f - std::clamp(curl * g_curlScale, 0.0f, 1.0f); };
            FRIKApiV2::HandPoseData pose{};
            pose.thumb = { scaled(thumb), scaled(thumb), scaled(thumb * 0.8f), 0.0f };
            pose.index = { scaled(prox * 0.9f), scaled(mid * 0.9f), scaled(dist * 0.9f), 0.0f };
            pose.middle = { scaled(prox), scaled(mid), scaled(dist), 0.0f };
            pose.ring = { scaled(prox), scaled(mid), scaled(dist), 0.0f };
            pose.pinky = { scaled(prox * 1.05f), scaled(mid * 1.05f), scaled(dist * 1.05f), 0.0f };
            return pose;
        }

        FRIKApiV2::Hand frikHand(const bool left) noexcept
        {
            return left ? FRIKApiV2::Hand::Left : FRIKApiV2::Hand::Right;
        }
    }

    void hold(const bool left, const char* bone) noexcept
    {
        const int index = left ? 1 : 0;
        if (!g_enabled || (g_holding[index] && g_bone[index] == bone)) {
            return;
        }
        auto* frik = api();
        if (!frik || !frik->isSkeletonReady()) {
            return;
        }
        const bool set = frik->setHandPoseCustom(kTag, frikHand(left), poseFor(bone), kPriority);
        g_holding[index] = set;
        g_bone[index] = bone;
        const auto state = frik->getHandPoseSetTagState(kTag, frikHand(left));
        char message[160]{};
        std::snprintf(
            message,
            sizeof(message),
            "Hand grip: %s fingers shaped for %s (%s)",
            left ? "left" : "right",
            bone ? bone : "?",
            !set ? "refused" : state == FRIKApiV2::HandPoseTagState::Active ? "active" : "overridden by another mod");
        npctouch::runtime::logInfo(message);
    }

    void release(const bool left) noexcept
    {
        const int index = left ? 1 : 0;
        if (!g_holding[index]) {
            return;
        }
        g_holding[index] = false;
        g_bone[index] = nullptr;
        if (auto* frik = api()) {
            (void)frik->clearHandPose(kTag, frikHand(left));
        }
    }

    bool trackedHand(const bool left, RE::NiTransform& out) noexcept
    {
        auto* frik = api();
        if (!frik || frik->getVersion() < 3 || !frik->isSkeletonReady()) {
            return false;
        }
        return frik->getTrackedHandTransform(frikHand(left), FRIKApiV2::TrackedHandKind::Wand, &out);
    }

    bool pinHand(const bool left, const RE::NiTransform& wrist) noexcept
    {
        auto* frik = api();
        if (!g_pinEnabled || !frik || frik->getVersion() < 3 || !frik->isSkeletonReady()) {
            return false;
        }
        const bool set = frik->setHandWorldTransform(kTag, frikHand(left), wrist, kPriority);
        g_pinned[left ? 1 : 0] = set;
        if (!g_pinLogged) {
            g_pinLogged = true;
            npctouch::runtime::logInfo(set ? "Hand grip: hand pinned to the held NPC" : "Hand grip: FRIK refused to pin the hand");
        }
        return set;
    }

    void unpinHand(const bool left) noexcept
    {
        const int index = left ? 1 : 0;
        if (!g_pinned[index]) {
            return;
        }
        g_pinned[index] = false;
        if (auto* frik = api()) {
            (void)frik->clearHandWorldTransform(kTag, frikHand(left));
        }
    }

    bool pinEnabled() noexcept
    {
        return g_pinEnabled;
    }

    void loadSettings() noexcept
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
                if (key == "bShapeFingers") {
                    g_enabled = value != 0.0f;
                } else if (key == "bPinHand") {
                    g_pinEnabled = value != 0.0f;
                } else if (key == "fFingerCurl") {
                    g_curlScale = std::clamp(value, 0.0f, 2.0f);
                }
            }
        } catch (...) {
        }
    }
}
