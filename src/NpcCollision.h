#pragma once

namespace RE
{
    class Actor;
}

namespace npctouch::collision
{
    // Reads the collision setting from Data/F4SE/Plugins/NpcTouch.ini.
    void initialize() noexcept;

    // Called every frame while the physics world exists: keeps the collision
    // layer pairs that let ROCK's hands stop against standing NPCs.
    void update(bool worldAvailable) noexcept;

    // Logs once per NPC which collision layers its body parts use.
    void describeActor(RE::Actor* actor) noexcept;
}
