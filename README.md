# Jak Enhanced Trilogy

A remaster of the **Jak and Daxter trilogy** for PC (Windows), built on [OpenGOAL](https://opengoal.dev):
modern lighting, living weather, reflections, denser vegetation and cities, and an in-game menu that
turns every change on and off. Each game plays on its own; the plan is to join the three into one.

Unofficial and free. You need **your own disc images** (.iso) of the PS2 games: no game data is in
this repository.

## Install and play

You need one, two or all three PS2 discs as .iso (PAL, NTSC or NTSC-J), about 30 GB free a game and a
recent graphics card.

### Players

1. Download **`Jak-Enhanced-Trilogy-<version>.zip`** from the
   [Releases page](https://github.com/stichlola/jak-enhanced-trilogy/releases/latest) and unzip it.
2. Put your disc images in its **`ISO`** folder.
3. Double-click **`Jak Enhanced Trilogy.bat`**.

The first time you play a game it extracts your disc (20-40 minutes, once). To update, unzip a new
release over the old folder: your extracted discs stay.

- **HD textures** (optional, 4x AI upscale): the packs for
  [Jak 1, 2 and 3](https://drive.google.com/drive/folders/18hjJ85N1T702fA--h_zcC9uUGmpo0oaz?usp=sharing)
  unzip into the same folder; the launcher installs them on the next start.
- **Jak 3 save in slot 4**: the game finished with everything unlocked, to try every feature.

### Developers

1. `git clone https://github.com/stichlola/jak-enhanced-trilogy.git`
2. Put your disc images in `ISO` and double-click `Jak Enhanced Trilogy.bat`: it installs what's
   missing with winget (VS 2022 Build Tools with Clang, CMake, NASM), builds (about an hour the first
   time), extracts and plays.
3. After a change: `cmake --build out\build\Release` for the C++,
   `out\build\Release\bin\goalc.exe --user-auto --game jak3 --cmd "(mi)"` (or `jak1`, `jak2`) for the
   game code.

`scripts\enhanced\setup.ps1` does the same from PowerShell (`-Jak1Iso`, `-SkipBuild`...).

## Controls

| | Keyboard | Pad |
|---|---|---|
| Enhanced menu | **F1** or **`\`** | **Select + R3** |
| Nap | **N** | **Select** |
| Next game of the trilogy | **F9** | |
| Jak's lantern (Jak 3, desert) | | hold **triangle** |
| OpenGOAL's debug toolbar | **F12** | |

## What's new

**Graphics**
- Sun shadows (soft), ambient occlusion, indirect light, real normals for the levels.
- The levels' lamps, torches, fires, shots and explosions light their surroundings; dark nights lit
  by lamps and windows; place your own lights from the menu.
- Volumetric clouds and their shadows, night sky, light shafts, ground mist, lens flare.
- Headlights with dust in the beams (Jak II and 3), Jak's floating lantern in the desert (Jak 3).
- Reflections on water, wet streets and metal; refraction and caustics; wakes and spray.
- Color grade per area, by day and by night; TAA, bloom, depth of field in cutscenes.
- More grass and plants, climbing vines; grass bends in the wind and away from Jak.
- Footprints and tire ruts in sand and snow; dust behind vehicles in the desert.

**Weather**
- Living weather in the open areas (fair, clouds, rain, storms, fog), different on every visit;
  fixed where the game wants it (the swamp's rain, Misty Island's fog, the mountain's snow).
- Rain with splashes and puddles that fill and dry; lightning; snow; the desert's sandstorms.
- 20 weather presets in the menu.
- Ambient sounds: the rain, light and heavy; dripping after it; wind in storms and on desert nights;
  crickets at night.

**Gameplay**
- **Nap**: pick 1-12 hours; Jak lies down, sleeps while the sun or moon crosses the sky, gets up
  and stretches.
- City population from Low to Extreme (Jak II and 3).
- Jak 3: faster jetboard, faster guns with twice the ammo, a charged scattergun, desert driving modes.
- Jak II: twice the ammo, every gun, upgrade and Dark Jak power from the start.

**In-game menu**: four graphics presets (Classic, Performance, Medium, Maximum), a switch for every
effect, weather, time of day, field of view, population, minimap, teleport (Jak 3). Choices are saved.

Technical notes for each feature are in [ENHANCED.md](ENHANCED.md); the trilogy's roadmap in
[TRILOGIA.md](TRILOGIA.md) (Italian).

## Known issues

- Water and material reflections still show some artifacts at the screen's edges and shorelines.
- Switching games (F9) closes and reopens the window.
- HD textures cover only part of each game.

## Change it yourself

- **With an AI assistant**: the project was made with [Claude Code](https://claude.com/claude-code).
  Open the folder in it, ask it to set things up following this README, then describe the change
  you want. [ENHANCED.md](ENHANCED.md) and `AGENTS.md` give it the project's map and rules.
- **By hand**: graphics in `game/graphics/opengl_renderer/EnhancedPost.cpp` and
  `game/graphics/opengl/shaders/enh_*`; game code (GOAL) in `goal_src/jak1|jak2|jak3`, the changes
  marked `Enhanced`; zones, grades and weather in `game/assets/enhanced/world.json` (reloaded live).

Contributions are welcome: read [CONTRIBUTING](.github/CONTRIBUTING.md) first (no game data,
everything can be switched off, write down how it works).

## Credits

- **Jak Enhanced Trilogy** by [stichlola](https://github.com/stichlola), made with Claude Code. If you
  make a video, a mod or a fork from it, please credit "Jak Enhanced Trilogy by stichlola" with a link
  to this repository.
- **[OpenGOAL](https://opengoal.dev)** and its team: the decompiled games and PC port everything here is
  built on. Not affiliated with them: please don't report Enhanced bugs to them. Their own README is
  [here](https://github.com/open-goal/jak-project#readme).
- **Naughty Dog**, for the trilogy; **Ratchet & Clank Enhanced**, the inspiration.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) for the HD textures, [Dear ImGui](https://github.com/ocornut/imgui)
  for the menu, the effects' techniques from their authors; the desert sand texture made with
  [Tripo](https://www.tripo3d.ai), the ambient sounds with [ElevenLabs](https://elevenlabs.io).

## Legal

Use your own legally owned copies of the games. Jak and Daxter: The Precursor Legacy, Jak II and
Jak 3 are © Sony Interactive Entertainment / Naughty Dog. This project is unofficial and contains no
game assets. OpenGOAL's license applies to the code ([LICENSE](LICENSE)).

**No donations of any kind are accepted for this project.** It is free and stays free: no paid
features, no early access, no tip jar.
