#pragma once

namespace npctouch::proxies
{
    // Reads the proxy body settings from Data/F4SE/Plugins/NpcTouch.ini.
    void initialize() noexcept;

    // Called every frame: keeps an invisible physics body on the chest of each
    // NPC near the player, so ROCK's hands stop against it.
    void update(bool worldAvailable) noexcept;
}
