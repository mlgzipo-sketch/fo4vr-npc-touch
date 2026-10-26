# NpcTouch for Fallout 4 VR

Physical interaction with NPCs in Fallout 4 VR, inspired by PLANCK for Skyrim VR. Built on top of ROCK (hand physics) and FRIK (full body).

## Features

- **Hands collide with NPCs.** Invisible capsules follow each nearby NPC's torso, head, arms and legs, so your physical hands stop against bodies instead of passing through.
- **Bodies react to touch.** A push turns the touched body, head or arm with your hand, then it springs back.
- **NPCs react with their own voices.** Regular NPCs say "Huh?" or "Hey...", then warn you if you keep touching them, especially on the chest or pelvis. Companions react according to their affinity and romance. Lines come from the game's own dialogue, so there is no extra .esp.
- **Shoves.** A fast hand makes NPCs stagger in the direction of the push.
- **Grabs.** Hold grip on an arm, shoulder, head, chest, hips or legs. Arms and heads follow your hand, the elbow bends, and shoulder or chest grabs sway the upper body. Your fingers wrap the part you hold and your palm stays on it. Clothes and hats move with the body instead of stretching.
- **NPCs react to being held.** They look at you, complain, and pull free once they are annoyed enough. Companions don't mind.
- Every feature can be tuned or turned off in `NpcTouch.ini`.

## Requirements

- Fallout 4 VR 1.2.72
- [F4SE VR](https://f4se.silverlock.org/)
- VR Address Library for F4SEVR
- [FRIK](https://www.nexusmods.com/fallout4/mods/53464) 0.79.1 or newer (needs FRIK API v2.3+)
- [ROCK](https://github.com/brunocatani/ROCK) 0.9 or newer, with `npcDynamicCollisions = true` in `ROCK.ini` (the default)

## Installation

Install the archive with Vortex or Mod Organizer 2, or unpack it into the game's `Data` folder. You should end up with:

```
Data\F4SE\Plugins\NpcTouch.dll
Data\F4SE\Plugins\NpcTouch.ini
```

Releases are on the [Releases page](https://github.com/mlgzipo-sketch/fo4vr-npc-touch/releases). The latest test build of every commit is under **Actions**.

## Settings

All settings are in `Data\F4SE\Plugins\NpcTouch.ini`, and each one has a comment. The main switches are:

| Setting | What it does |
| --- | --- |
| `bEnableReactions` | NPCs talk and look at you when touched |
| `bAllowCombat` | An NPC who is touched too much starts a fight (off by default) |
| `bEnableShoves` | Fast hands make NPCs stagger |
| `bHandsCollideWithNPCs` | Hands stop against NPC bodies |
| `bFlinch` | Bodies give way to a push and spring back |
| `bShapeFingers`, `bPinHand` | Fingers wrap what you hold, and the palm sticks to it |
| `bShoulderMovesBody` | Shoulder and chest grabs sway the upper body |
| `bDebugLog` | Detailed log for bug reports |

## Reporting bugs

Set `bDebugLog=1`, reproduce the problem, and attach `Documents\My Games\Fallout4VR\F4SE\NpcTouch.log`. The first lines show the build number.

## Known limitations

- There is no ragdoll yet. NPCs can't be lifted or thrown, only moved and posed. Full active ragdoll is planned once ROCK's companion project SCISSORS is out.
- Sitting and dead NPCs can be touched, but not grabbed.
- Only the 4 nearest NPCs (configurable) get collision bodies.

## Building

You need Visual Studio 2022, CMake and vcpkg (`VCPKG_ROOT`).

```powershell
git clone --recursive https://github.com/mlgzipo-sketch/fo4vr-npc-touch
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Credits

- **FlyingParticle (adamhynek)**: the idea. [PLANCK](https://www.nexusmods.com/skyrimspecialedition/mods/66025) for Skyrim VR showed how physical NPC interaction in VR should feel. This mod is my attempt at bringing that to Fallout 4 VR.
- **brunocatani**: [ROCK](https://github.com/brunocatani/ROCK) and the [RPS SDK](https://github.com/brunocatani/RPS_SDK). ROCK provides the physical hands. The NPC collision bodies are adapted from ROCK's physics body code, and the plugin runtime is based on the RPS SDK examples.
- **FRIK authors**: [FRIK](https://www.nexusmods.com/fallout4/mods/53464) and its API for finger poses and hand placement.
- **CommonLibF4 / CommonLibF4VR contributors** and the **F4SE team**.

## License

GPL-3.0, the same as ROCK, whose code it adapts. The full source is in this repository.
