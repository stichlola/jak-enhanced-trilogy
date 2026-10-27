# Jak Enhanced Trilogy

A remaster of the **Jak and Daxter trilogy** for PC (Windows), built on [OpenGOAL](https://opengoal.dev):
modern lighting, living weather, reflections, denser vegetation and cities, and an in-game menu
that turns every change on and off. It plays each game on its own today. **If you have all three**,
the plan is to join them into **one big game**: from Jak 1 to Jak 3 without stopping, with new
bridge missions for what the games never showed.

## Install and play

You need **your own disc images** (.iso) of the PS2 games, one, two or all three (PAL, NTSC or
NTSC-J), about 30 GB free a game and a recent graphics card.

### Players: download and double-click

1. Download **`Jak-Enhanced-Trilogy-<version>.zip`** from the
   [Releases page](https://github.com/stichlola/jak-enhanced-trilogy/releases/latest) and unzip it
   anywhere.
2. Put your disc images (`.iso`) in its **`ISO`** folder.
3. Double-click **`Jak Enhanced Trilogy.bat`**.

That's all: nothing else to install. It recognises which game each disc is. The first time you play
a game it extracts your disc (20-40 minutes, only once); after that it asks which game to play and
starts it.

**Updating to a new release:** unzip it over your old folder (replace the files). Your extracted
discs stay: the launcher only compiles each game again for the new version (a minute or two).

**Optional HD textures** (4x AI upscale): download the pack for your game from
[this folder](https://drive.google.com/drive/folders/18hjJ85N1T702fA--h_zcC9uUGmpo0oaz?usp=sharing) (`Jak-Enhanced-HD-Textures-jak1.zip`, `-jak2`, `-jak3`: 0.5, 2.6 and 3.7 GB),
unzip it into the Jak Enhanced Trilogy folder and double-click the launcher again: it puts them in
the game by itself (20-40 minutes, only once). They're made from the games' own textures, so they're
only for people who own the games. The upload is still in progress: if a pack isn't there yet, the community's packs work too
(their `custom_assets` folder goes in `data`): Jak 3, [Cioscos' HD pack on Nexus Mods](https://www.nexusmods.com/opengoal/mods/31);
Jak 2, [the ESRGAN pack on GitHub](https://github.com/Aloqas/OpenGOAL-Jak2-HD-Texture-Pack) (Code → Download ZIP).
The Drive packs are the ones made for this project.

**Jak 3 save in slot 4**: the whole game finished, everything unlocked (guns, powers, vehicles,
secrets) to try every feature at once. A new game starts as the original; delete or overwrite the
slot when you like (the launcher only fills it while it's empty).

> **Tip:** in the game, press **`\`** (the key left of `1`) for the **Enhanced menu**: graphics
> presets, every effect, weather, time of day. **F9** switches to the next game of the trilogy.

### Developers: from the source code

1. Clone the repository: `git clone https://github.com/stichlola/jak-enhanced-trilogy.git`
2. Put your disc images in its `ISO` folder and double-click `Jak Enhanced Trilogy.bat`. In the
   source code it first installs what's missing with winget (Visual Studio 2022 Build Tools with
   Clang, CMake, NASM) and builds the programs (about an hour the first time), then extracts and
   plays like the release.
3. Change things (see [Change it yourself](#change-it-yourself)): rebuild the C++ with
   `cmake --build out\build\Release`, recompile a game's code with
   `out\build\Release\bin\goalc.exe --user-auto --game jak3 --cmd "(mi)"` (or `jak1`, `jak2`).

`scripts\enhanced\setup.ps1` does the same steps from PowerShell with explicit paths
(`-Jak1Iso`, `-Jak2Iso`, `-Jak3Iso`, `-SkipBuild`...). **HD textures**: the
[ready-made packs](https://drive.google.com/drive/folders/18hjJ85N1T702fA--h_zcC9uUGmpo0oaz?usp=sharing) go in the source code too (their `data\custom_assets` folder is the
repository's `custom_assets`), or make them yourself with
[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN): `python scripts\enhanced\hd_textures.py
--game jak1|jak2|jak3` (`make_hd_pack.py` zips them). Either way the launcher then extracts the
levels again, once. They're never committed: they come from the games' own textures.

## About

It started as *Jak 3 Enhanced*, inspired by *Ratchet & Clank Enhanced*; now the same improvements
come to all three games.

Everything here is code: the games themselves come from **your own copies** of the PS2 discs. No
game data is in this repository.

Technical notes for each feature (how it works, its test switches) are in [ENHANCED.md](ENHANCED.md);
the trilogy's roadmap is in [TRILOGIA.md](TRILOGIA.md) (Italian).

> An unofficial fork, not affiliated with the OpenGOAL team: please don't report Enhanced bugs to them.
> Made by [stichlola](https://github.com/stichlola) with AI assistance (Claude Code). Forks and
> improvements are welcome: see [CONTRIBUTING](.github/CONTRIBUTING.md), and please credit the
> project in videos and mods (see [Credits](#credits)).

## The trilogy

| | Graphics | Weather | Gameplay | Status |
|---|---|---|---|---|
| **Jak 3** | everything below | Haven, Spargus, the desert, the forest | population, guns, jetboard, vehicles | the most complete |
| **Jak 2** | everything below, HD textures as an optional pack | Haven, the wilds, the forest | population, all guns and Dark Jak powers unlocked, twice the ammo | playable |
| **Jak 1** | everything below, HD textures as an optional pack | Sandover and the islands, the jungle, the swamp, Misty Island, Snowy Mountain, the volcano | | playable |

- **One program, three games**: press **F9** in a game to switch to the next one (Jak 1 → 2 → 3 → 1)
  without leaving the program. The next steps: the window kept across the switch, one trilogy menu
  and save, the end of each game leading into the next.
- **Bridge levels** (planned): the end of Jak 1 into Jak 2's opening, and a new level for what Jak
  3 only tells: Jak arrested in a Haven at war and sent into the desert.
- **The same look everywhere**: the world's zones, their color grades and their weather for the
  three games are in one file, [game/assets/enhanced/world.json](game/assets/enhanced/world.json),
  reloaded while the game runs. `python scripts/enhanced/check_games.py` checks what each game has.

## What's new

### Graphics
- **Lighting**: sun shadow map (level, characters and wind-blown trees cast shadows) with soft
  shadows (sharp where they touch, softer further from what casts them), contact shadows, ambient
  occlusion (SSAO), one bounce of indirect light (SSGI), and computed normals for the levels, which
  originally had only baked vertex colors.
- **Lights**: the levels' own lamps and every glow (torches, shots, explosions) light their
  surroundings and cast short shadows. Night lamps switch on at night.
- **Dark nights**: where no light reaches, the night is dark (the desert almost black under the
  stars); lamps, fires and windows light the world in their own colors and glow in the night haze.
  The eyes adapt a little to a dark view. A menu switch brings back the game's own nights.
- **Night sky**: twinkling stars, a faint milky way, the moon and its halo; Haven and Spargus glow
  warm on the low sky.
- **Headlights** (Jak 3): vehicles light the road ahead with real beams, visible in the night air.
- **Daylight**: cooler, deeper shadows and a fuller sunlit side; bright sand and walls keep their
  color; a partial white balance takes off the strongest color casts.
- **Fires** flicker, the air above them shimmers, and embers rise from them.
- **Explosions**: a flash of light, a shockwave that bends the picture, sparks flying out on their
  arcs and hot air shimmering over the blast. Smoke and dust are lit by the lights near them.
- **Sky**: volumetric clouds lit by the sun (bright edges toward it, shaded undersides), a deep
  blue sky by day, orange at sunset, grey and low under the rain.
- **Color grade per area**: Haven, the desert, the forest, the Metal Head city, underground, the
  volcano, the temples, Sandover's islands, the jungle, the swamp, the snow... easing from one to
  the next, by day and by night.
- **Vivid colors** (a separate setting, off by default): saturated greens and blues, warm sun, deep
  sky.
- **Characters**: soft, warm light past the edge of the lit side, a cool rim on the shadow side,
  darker and glossy in the rain.
- **Cutscenes**: depth of field with autofocus on who's in the middle of the shot.
- **Atmosphere**: volumetric ground mist with light shafts, god rays, heat haze, depth of field,
  bloom, camera motion blur and color grading.
- **Anti-aliasing**: TAA (temporal), with FXAA as the fallback.
- **Water**: screen-space reflections on the sea, open-sea swell, refraction and caustics in
  shallow water, and rings spreading where Jak swims or rides.
- **Materials**: metal and glass pick up reflections and highlights (found from the texture names),
  and the surfaces show relief from their textures.
- **Ray-traced reflections and ambient occlusion** in software, through the levels' real geometry
  (off by default for now: still artifacts, and slow).
- **Vegetation**: more grass and plants around the original ones, and climbing plants on the walls
  near the level's own vines. Grass sways in the wind and bends away from Jak, enemies and vehicles.
- **Footprints and tire ruts** in sand, dirt and snow; dust plumes behind vehicles and the
  jetboard in the desert, spray, a wake and foam where they cross water.
- **HD textures** (Jak 3): a 4x AI upscale of Spargus, the desert, the vehicles, Jak, Daxter and the
  guns (an optional step, see below).
- **Draw distance**: no more buildings popping in around corners and over rooftops.

### Weather
- **Living weather, different on every visit**: in the open places with no weather of their own in
  the story (Sandover, Haven, Spargus, the forests...) the weather comes in spells: mostly fair,
  sometimes clouds, a drizzle, a downpour or a thunderstorm, sometimes light or thick fog. Each
  visit starts somewhere new. No fog is added by default.
- **Fixed where the game wants it**: it always rains in Boggy Billy's swamp, the fog never lifts on
  Misty Island, it always snows on Snowy Mountain, the air shimmers over the lava.
- **Rain** falls in the world with splashes on the ground; wet streets reflect the windows and
  signs, puddles fill and dry, Jak's steps splash in them, lamps get halos in the wet air,
  characters get wet and glossy. After the rain the sun comes back out (light shafts, steam, a
  rainbow).
- **Thunderstorms**: lightning bolts with the game's own thunder, sheet lightning inside the clouds.
- **Snow**: flakes drifting down with the wind.
- **Sandstorms**: the game's own sandstorm in the desert blows real clouds of sand through the
  world.
- **Menu**: "Changing" (the above), or 20 presets from Clear and Overcast to Downpour, Storm in the
  fog, Dense fog, Blizzard and Dusty haze, each a mix of rain, thunder, fog, ground mist, snow,
  dust, clouds and wet ground that you can change by hand.

### City (Jak 2 and 3)
- **Population** setting from Low to Extreme. At High (the default in Jak 3) there are twice as many
  passers-by and as much traffic, and six times as many Metal Heads and KG robots in the street war.
- The vehicles' engine sounds no longer go silent when both factions are out in the streets (Jak 3).
- The **Jak 2 plaza** is rebuilt inside Jak 3's Haven, with passages cut into the port and the Metal
  Head city. It needs a Jak 2 disc too.

### Gameplay
- Jak 3: faster jetboard, always at hand; guns fire 1.5x faster and carry twice the ammo; driving
  modes for the desert vehicles (classic, sport, turbo). The scattergun charges: hold fire for a
  huge, wide shot that throws enemies back and kicks Jak back too.
- Jak 2: twice the ammo; every gun, upgrade, the jetboard and Dark Jak's powers from the start (the
  security passes stay with the story).

### In-game menu (press `\`)
- **Graphics**: three presets (Classic: the game as it was; Medium: the costly effects off; Maximum:
  everything) and a switch for every effect. Also vegetation density, lamp strength, vivid colors,
  the weather and the water effects.
- **City**: population (Low / Normal / High / Extreme) and the minimap (shown by default).
- **Field of view**: 80-140% of the game's, in play (cutscenes as filmed).
- **Time of day**: six buttons (dawn to night), a slider, and a "freeze time" switch.
- **Teleport** (Jak 3): quick travel to nine places in Haven, Spargus and the desert.

The choices are saved and come back at the next start.

## Known issues
- **Water and PBR reflections/refraction** still show artifacts (less than before): shimmering or
  smeared edges where the screen-space reflections run out of picture, and the refraction bends the
  wrong things at the shoreline.
- Switching games (F9) closes and reopens the window for now.
- The Jak 2 plaza in Jak 3 has no passers-by or enemies yet (it has no navigation mesh for them).
- HD textures cover only Jak 3's Spargus, the desert, the vehicles and the characters.
- No hardware ray tracing (the renderer is OpenGL): the ray-traced reflections run in software on
  the GPU, and they don't show characters or vehicles yet.

## Change it yourself

### The fastest way: with an AI coding assistant
This mod was made by talking to [Claude Code](https://claude.com/claude-code), Anthropic's coding
agent: you describe what you want in plain words, and it reads the code, makes the change, builds,
runs the game to check, and commits.

1. Install Claude Code (desktop app or `npm install -g @anthropic-ai/claude-code`) and open the
   `jak-project` folder in it.
2. Ask for the setup first: *"Set this project up and build it following README.md; my ISOs are at
   C:\...\Jak 1.iso, ...\Jak 2.iso and ...\Jak 3.iso"*.
3. Then ask for changes the way you'd say them to a friend: *"make the night darker"*, *"more Metal
   Heads in the port"*, *"add a menu switch for the fog"*. It's good practice to ask it to explain
   the change and to try it in the game before committing.

[ENHANCED.md](ENHANCED.md) and `AGENTS.md` give the assistant the project's map and rules.

### By hand
- **Graphics** (C++ and shaders): `game/graphics/opengl_renderer/EnhancedPost.cpp` (the post-processing
  passes and the menu) and `game/graphics/opengl/shaders/enh_*`. Rebuild with
  `setup.ps1 -SkipTools -SkipExtract`.
- **Game code** (GOAL, a Lisp): `goal_src/jak1/`, `goal_src/jak2/`, `goal_src/jak3/`. The changes
  are marked `Enhanced`, for example `engine/ai/traffic-h.gc` (population) and
  `engine/target/logic-target.gc` (the menu's requests). Recompile with
  `out\build\Release\bin\goalc.exe --user-auto --game jak3 --cmd "(mi)"` (or `jak1`, `jak2`; a few
  seconds to minutes), then restart the game.
- **The world's look and weather**: `game/assets/enhanced/world.json` (zones, grades, weather, per
  game): save it while the game runs and the change shows within a second.
- **Live changes**: start the game with `-debug` (`gk.exe --game jak3 -- -boot -fakeiso -debug`), open
  the compiler (`goalc.exe --user-auto --game jak3`), type `(lt)` then `(mi)`, and change values while
  you play, e.g. `(set! *enhanced-board-speed* 1.6)`.
- **Effect settings**: many sliders are in the debug menu (`-debug`, then "Renderer Debug" → "Enhanced
  Post"). The environment variables in ENHANCED.md (`JAK_ENHANCED_*`) switch effects for a test run.

## Help wanted
Good places to start, roughly from easiest:
- the known issues above (the water and PBR artifacts);
- HD textures for Haven, the other areas and Jak 1 and 2;
- the trilogy's next steps (TRILOGIA.md): the window kept across the switch, one menu and save;
- passers-by and enemies in the Jak 2 plaza (the plan is in ENHANCED.md, "Not done");
- translations of the menu.

Read [CONTRIBUTING](.github/CONTRIBUTING.md) first: three short rules (no game data, everything can
be switched off, write down how it works) and where the code is.

## Credits
- **Jak Enhanced Trilogy** (formerly Jak 3 Enhanced) by [stichlola](https://github.com/stichlola), made with
  [Claude Code](https://claude.com/claude-code) (Anthropic). **If you make a video, a mod or a fork
  from it, please credit "Jak Enhanced Trilogy by stichlola" with a link to
  https://github.com/stichlola/jak-enhanced-trilogy.** That's all I ask.
- **[OpenGOAL](https://opengoal.dev)** and its team: the decompiled game, the compiler and the PC port
  that everything here is built on, years of their work. Support them.
- **Naughty Dog**, for the Jak and Daxter trilogy.
- **Ratchet & Clank Enhanced** (the PS2 remaster mod), the inspiration.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (Xintao Wang et al.) for the HD textures,
  [Dear ImGui](https://github.com/ocornut/imgui) for the menu, and the effects' techniques from their
  authors (Alchemy AO, Lottes' FXAA, Henyey-Greenstein scattering, dual-filter bloom...).
- The desert sand texture was generated with [Tripo](https://www.tripo3d.ai) (AI).

## Legal
Use your own legally owned copies of the games. Jak and Daxter: The Precursor Legacy, Jak II and Jak 3
are © Sony Interactive Entertainment / Naughty Dog.
This project is unofficial and contains no game assets. OpenGOAL's license applies to the code
([LICENSE](LICENSE)).

---

# Original OpenGOAL README

<p align="center">
  <img width="500" height="100%" src="./docs/img/logo-text-colored-new.png">
</p>

<p align="center">
  <a href="https://opengoal.dev/docs/intro" rel="nofollow"><img src="https://img.shields.io/badge/Documentation-Here-informational" alt="Documentation Badge" style="max-width:100%;"></a>
  <a title="Crowdin" target="_blank" href="https://crowdin.com/project/opengoal"><img src="https://badges.crowdin.net/opengoal/localized.svg"></a>
  <a target="_blank" rel="noopener noreferrer" href="https://github.com/open-goal/jak-project/actions/workflows/build-matrix.yaml"><img src="https://github.com/open-goal/jak-project/actions/workflows/build-matrix.yaml/badge.svg" alt="Linux and Windows Build" style="max-width:100%;"></a>
  <a href="https://www.codacy.com/gh/open-goal/jak-project/dashboard?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=open-goal/jak-project&amp;utm_campaign=Badge_Grade" rel="nofollow"><img src="https://app.codacy.com/project/badge/Grade/29316d04a1644aa390c33be07289f3f5" alt="Codacy Badge" style="max-width:100%;"></a>
  <a href="https://discord.gg/VZbXMHXzWv"><img src="https://img.shields.io/discord/756287461377703987" alt="Discord"></a>
</p>

## Please read first <!-- omit from toc -->

> [!IMPORTANT]
> Our repositories on GitHub are for development of the project and tracking active issues. Most of the information you will find here pertains to setting up the project for development purposes and is not relevant to the end-user.

For a setup guide on how to install and play the game there is the following video that you can check out: https://youtu.be/K84UUMnkJc4

For questions or additional information pertaining to the project, we have a Discord for discussion here: https://discord.gg/VZbXMHXzWv

Additionally, you can find further documentation and answers to **frequently asked questions** on the project's main website: https://opengoal.dev

> [!WARNING]
> **Do not use this decompilation project without the use of your own legally purchased copy of the game.** OpenGOAL does not include any assets from the original games, so you must provide your own legitimately obtained PS2 copy of the game. OpenGOAL supports every retail PAL, NTSC, and NTSC-J build, including Greatest Hits copies. Please note that does NOT include any of the later releases (PS3/PS4/PS5).

- [Project Description](#project-description)
  - [Current Status](#current-status)
  - [Methodology](#methodology)
- [Setting up a Development Environment](#setting-up-a-development-environment)
  - [OS Setup](#os-setup)
  - [Editor Setup](#editor-setup)
  - [Building and Running the Game](#building-and-running-the-game)
    - [Extract Assets](#extract-assets)
    - [Build the Game (Running the Compiler)](#build-the-game-running-the-compiler)
    - [Run the Game](#run-the-game)
      - [Connecting the REPL to the Game](#connecting-the-repl-to-the-game)
      - [Running the Game Without Auto-Booting](#running-the-game-without-auto-booting)
- [Technical Project Overview](#technical-project-overview)

## Project Description

The project's goal is to port the original trilogy (Jak 1 -> Jak 3) to PC. Over 98% of the games were written in GOAL, a custom LISP language developed by Naughty Dog. Our strategy is:
- decompile the original game code into human-readable GOAL code
- develop our own compiler for GOAL and recompile the game code for x86-64
- create a tool to extract game assets into formats that can be easily viewed or modified
- create tools to repack game assets into a format that our port uses.

Our objectives are:
- make the port a "native application" on x86-64, with high performance. It shouldn't be emulated, interpreted, or transpiled.
- Our GOAL compiler's performance should be around the same as unoptimized C.
- try to match things from the original game and development as possible. For example, the original GOAL compiler supported live modification of code while the game is running, so we do the same, even though it's not required for just porting the game.
- support modifications. It should be possible to make edits to the code without everything else breaking.

At the moment we support **x86_64** on Windows, Linux and macOS (via Rosetta translation).  There are no plans to ever make a mobile release.

### Current Status

- Jak 1 has been considered in a polished, complete state for years at this point.
- Jak 2 is considered in beta due to a few issues we are aware of that need fixing, however to the casual user, the game is essentially complete.
- Jak 3 has a good amount of work left to do.

![](./docs/img/promosmall1.png)
![](./docs/img/promosmall2.png)

YouTube playlist showcasing some of the early progress for Jak 1:
https://www.youtube.com/playlist?list=PLWx9T30aAT50cLnCTY1SAbt2TtWQzKfXX

### Methodology

To assist with decompiling, we've built a decompiler that can process GOAL code and unpack game assets. We manually specify function types and locations where we believe the original code had type casts (or where they feel appropriate) until the decompilation succeeds, then we clean up the output of the decompiled code by adding comments and adjusting formatting, then save it in `goal_src/`.

Our decompiler is designed specifically for processing the output of the original GOAL compiler. As a result, when given correct casts, it often produces code that can be directly fed into a compiler and works perfectly. This is continually tested as part of our unit tests.

## Setting up a Development Environment

The remainder of this README is aimed at people interested in building the project from source, typically with the intention of contributing as a developer.

If this does not sound like you and you just want to play the game, refer to the above section [Quick Start](#quick-start)

### OS Setup

- [Windows](/docs/setup/system/windows.md)
- [Linux](/docs/setup/system/linux.md)
- [MacOS](/docs/setup/system/macos.md)
- [Docker](/docs/setup/system/docker.md)

### Editor Setup

You can of course use whatever editor you want, but here is some documentation that should help you get started on some of the editor's we have used and have written about:

- [Visual Studio (Windows)](/docs/setup/dev/vs.md)
- [Visual Studio Code](/docs/setup/dev/vscode.md)
- [Zed](/docs/setup/dev/zed.md)

### Building and Running the Game

Getting a running game involves 4 main steps:

1. Build C++ tools (follow Getting Started steps above for your platform)
2. Extract assets from the game
3. Build the game
4. Run the game

#### Extract Assets

First, we have to setup our environment so we know which game and version we are operating with. For the black label version of Jak 1 we would run the following:

```sh
task set-game-jak1
task set-decomp-ntscv1 # or for example for PAL, `task set-decomp-pal`
```

> Run `task --list` to see the other available options

Next, ensure you extract your ISO file contents into the relevant `iso_data/<game-name>` folder.  In the case of Jak 1 this is `iso_data/jak1`.

Once this is done, open a terminal in the `jak-project` folder and run the following:

```sh
task extract
```

#### Build the Game (Running the Compiler)

The next step is to build the game itself.  To do so, in the same terminal run the following:

```sh
task repl
```

You will be greeted with a prompt like so:

```sh
 _____             _____ _____ _____ __
|     |___ ___ ___|   __|     |  _  |  |
|  |  | . | -_|   |  |  |  |  |     |  |__
|_____|  _|___|_|_|_____|_____|__|__|_____|
      |_|
Welcome to OpenGOAL 0.8!
Run (repl-help) for help with common commands and REPL usage.
Run (lt) to connect to the local target.

g >
```

Run the following to build the game:

```sh
g > (mi)
```

> IMPORTANT NOTE! If you're not using the non-default version of the game, you may hit issues trying to run `(mi)` in this step. An example error might include something like:
>
> `Input file iso_data/jak1/MUS/TWEAKVAL.MUS does not exist.`
>
> This is because the decompiler inputs/outputs using the `gameName` JSON field in the decompiler config. For example if you are using Jak 1 PAL, it will assume `iso_data/jak1_pal` and `decompiler_out/jak1_pal`.  Therefore, you can inform the REPL/compiler of this via the `gameVersionFolder` config field described [here](./goal_src/user/README.md)

#### Run the Game

Finally the game can be launched.  Open a second terminal from the `jak-project` directory and run the following:

```sh
task boot-game
```

The game should boot automatically if everything was done correctly.

##### Connecting the REPL to the Game

Connecting the REPL to the game allows you to inspect and modify code or data while the game is running.

To do so, in the REPL after a successful `(mi)`, run the following:

```sh
g > (lt)
```

If successful, your prompt should change to:

```sh
gc>
```

For example, running the following will print out some basic information about Jak:

```sh
gc> *target*
```

##### Running the Game Without Auto-Booting

You can also start up the game without booting.  To do so run the following in one terminal

```sh
task run-game
```

And then in your REPL run the following (after a successful `(mi)`):

```sh
g > (lt)
[Listener] Socket connected established! (took 0 tries). Waiting for version...
Got version 0.8 OK!
[Debugger] Context: valid = true, s7 = 0x147d24, base = 0x2123000000, tid = 2438049

gc> (lg)
10836466        #xa559f2              0.0000        ("game" "kernel")

gc> (test-play)
(play :use-vis #t :init-game #f) has been called!
0        #x0              0.0000        0

gc>
```

## Technical Project Overview

Some more detail about the various components of the project can be found [here](/docs/project-overview.md)
