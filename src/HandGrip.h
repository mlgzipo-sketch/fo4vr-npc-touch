#pragma once

namespace npctouch::grip
{
    // Shapes the player's fingers around the NPC part a hand holds, through
    // FRIK's hand pose API. bone is the grabbed NPC bone (Hand, ForeArm1,
    // UpperArm, Neck, Head, Chest, ...). Safe to call every frame.
    void hold(bool left, const char* bone) noexcept;

    // Gives the fingers back to FRIK.
    void release(bool left) noexcept;

    // Reads the [Grab] grip keys of NpcTouch.ini.
    void loadSettings() noexcept;
}
