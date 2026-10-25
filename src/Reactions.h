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

    // Called about once a second while a hand holds the NPC, with the
    // seconds since the last call and since the grab began. Returns true
    // when the NPC pulls free.
    bool onGrabHeld(RE::Actor* npc, const char* bone, float seconds, float heldSeconds) noexcept;

    // Moves the NPC to a new spot (Papyrus SetPosition), for dragging.
    bool moveActor(RE::Actor* npc, float x, float y, float z) noexcept;

    // Called every frame: ends head tracking and lets annoyance fade.
    void update() noexcept;
}
