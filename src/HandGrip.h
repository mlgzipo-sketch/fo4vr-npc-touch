#pragma once

namespace RE
{
    class NiTransform;
}

namespace npctouch::grip
{
    // Shapes the player's fingers around the NPC part a hand holds, through
    // FRIK's hand pose API. bone is the grabbed NPC bone (Hand, ForeArm1,
    // UpperArm, Neck, Head, Chest, ...). Safe to call every frame.
    void hold(bool left, const char* bone) noexcept;

    // Gives the fingers back to FRIK.
    void release(bool left) noexcept;

    // The tracked controller (wand) of a hand, as FRIK reads it this frame.
    // False when FRIK is too old (API v2.3 needed) or not ready.
    bool trackedHand(bool left, RE::NiTransform& out) noexcept;

    // Draws the hand (and solves the arm) at this wrist transform instead of
    // the controller, until unpinHand. Returns false if FRIK refused.
    bool pinHand(bool left, const RE::NiTransform& wrist) noexcept;
    void unpinHand(bool left) noexcept;

    // Whether hands are pinned to what they hold (bPinHand).
    bool pinEnabled() noexcept;

    // Reads the [Grab] grip keys of NpcTouch.ini.
    void loadSettings() noexcept;
}
