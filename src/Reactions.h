#pragma once

namespace RE
{
    class Actor;
}

namespace npctouch::reactions
{
    // Reads Data/F4SE/Plugins/NpcTouch.ini and checks that the game functions
    // the reactions need are in the address library.
    void initialize() noexcept;

    // Called once when a hand starts touching an NPC on the given bone, with
    // the hand's velocity in game units per second.
    void onTouch(RE::Actor* npc, const char* bone, const float (&handVelocity)[3]) noexcept;

    // Called every frame: ends head tracking and lets annoyance fade.
    void update() noexcept;
}
