#pragma once

namespace RE
{
    class Actor;
}

namespace npctouch::collision
{
    // Reads the collision setting from Data/F4SE/Plugins/NpcTouch.ini.
    void initialize() noexcept;

    // Called every frame: moves the body parts of NPCs near the player onto
    // the layer ROCK's hands collide with, and back once they leave.
    void update(bool worldAvailable) noexcept;

    // Logs once per NPC which collision layers its body parts use.
    void describeActor(RE::Actor* actor) noexcept;
}
