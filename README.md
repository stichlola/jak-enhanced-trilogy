# Jak 3 Enhanced

A personal, for-fun remaster of **Jak 3** on top of [OpenGOAL](https://opengoal.dev), inspired by
*Ratchet & Clank Enhanced*. It adds modern lighting and post-processing, denser vegetation and
cities, a few gameplay tweaks and an in-game menu that turns every change on and off. Everything here
is code: the game itself comes from **your own copy** of the PS2 disc. No game data is in this
repository.

Technical notes for each feature (how it works, its test switches) are in [ENHANCED.md](ENHANCED.md).

## What's new

### Graphics
- **Lighting**: sun shadow map (level, characters and wind-blown trees cast shadows), contact
  shadows, ambient occlusion (SSAO), one bounce of indirect light (SSGI), and computed normals for the
  levels, which originally had only baked vertex colors.
- **Lights**: the levels' own lamps and every glow (torches, shots, explosions) light their
  surroundings and cast short shadows. Night lamps switch on at night.
- **Fires** flicker, the air above them shimmers, and embers rise from them.
- **Explosions**: a flash of light and a shockwave that bends the picture. Smoke and dust are lit by
  the lights near them.
- **Atmosphere**: volumetric ground mist with light shafts, god rays, distance and height fog,
  heat haze, depth of field, bloom, camera motion blur and color grading.
- **Anti-aliasing**: TAA (temporal), with FXAA as the fallback.
- **Water**: screen-space reflections on the sea, open-sea swell, refraction and caustics in
  shallow water, and rings spreading where Jak swims or rides.
- **Materials**: metal and glass pick up reflections and highlights (found from the texture names),
  and the surfaces show relief from their textures.
- **Vegetation**: more grass and plants around the original ones, and climbing plants on the walls
  near the level's own vines. Grass sways in the wind and bends away from Jak, enemies and vehicles.
- **Desert**: footprints and tire ruts in the sand, and a new sand texture on the dunes.
- **HD textures**: a 4x AI upscale of Spargus, the desert, the vehicles, Jak, Daxter and the guns
  (an optional step, see below).
- **Draw distance**: no more buildings popping in around corners and over rooftops.

### City
- **Population** setting from Low to Extreme. At High (the default) there are twice as many
  passers-by and as much traffic, and six times as many Metal Heads and KG robots in the street war.
- The vehicles' engine sounds no longer go silent when both factions are out in the streets.
- The **Jak 2 plaza** is rebuilt inside Haven, with passages cut into the port and the Metal Head
  city. It needs a Jak 2 disc too.

### Gameplay
- Faster jetboard, always at hand.
- Guns fire 1.5x faster and carry twice the ammo.
- Driving modes for the desert vehicles (classic, sport, turbo).

### In-game menu (press `\`)
- **Graphics**: three presets (Classic: the game as it was; Medium: the costly effects off; Maximum:
  everything) and a switch for every effect. Also vegetation density, lamp strength and the water
  effects.
- **City**: population (Low / Normal / High / Extreme) and the minimap, which is hidden by default.
- **Time of day**: six buttons (dawn to night), a slider, and a "freeze time" switch.
- **Showcase**: five stops that show the mod off, in order (see below).
- **Teleport**: quick travel to nine places in Haven, Spargus and the desert.

The choices are saved and come back at the next start.

### Showcase tour
Open the menu with `\`, then press the stops in order:

| # | Stop | Time | What to look at |
|---|------|------|-----------------|
| 1 | Forest | 9:00 | vegetation, climbing plants, wind, god rays through the trees, mist |
| 2 | Spargus | 12:00 | HD textures, the sea's reflections and swell, metal and glass |
| 3 | Desert | 18:00 | dunes, dust, footprints and ruts (drive a vehicle), heat haze at sunset |
| 4 | Haven war zone | 15:00 | crowds, the Krimson Guard against the Metal Heads, explosions and smoke |
| 5 | Haven port at night | 22:00 | lamps lighting the streets, light shadows, the water at night |

Some stops need the area unlocked in your save, as on any continue point. Stop 4 sets the population to
High.

## Known issues
- **Water and PBR reflections/refraction** still show artifacts (less than before): shimmering or
  smeared edges where the screen-space reflections run out of picture, and the refraction bends the
  wrong things at the shoreline.
- **Extreme population crashes the game in Haven's war zone** (near the port, where the guards fight
  the Metal Heads): the streets' navigation meshes run out of room for that many characters. Use High
  there.
- The Jak 2 plaza has no passers-by or enemies yet (it has no navigation mesh for them).
- HD textures cover only Spargus, the desert, the vehicles and the characters.
- No hardware ray tracing (the renderer is OpenGL).

## Install and play (Windows)

You need **your own Jak 3 disc image** (an ISO of the PS2 game: PAL, NTSC or NTSC-J), about 30 GB free
and a recent GPU.

1. Get the code: install [Git](https://git-scm.com/download/win), then in a terminal
   (PowerShell):
   ```
   git clone -b jak3-enhanced https://github.com/stichlola/jak3-enhanced.git jak-project
   cd jak-project
   ```
   Name the folder `jak-project`: the OpenGOAL tools look for that name.
2. Run the setup with your ISO:
   ```
   powershell -ExecutionPolicy Bypass -File scripts\enhanced\setup.ps1 -Iso "C:\path\to\Jak 3.iso"
   ```
   It installs what's missing with winget (Visual Studio 2022 Build Tools with Clang, CMake, Ninja,
   NASM), builds the game, extracts and compiles your disc's data, and writes
   `Play-Jak3-Enhanced.bat`. The first run takes about an hour.
3. Play: double-click `Play-Jak3-Enhanced.bat`. Press `\` in the game for the Enhanced menu.

Optional HD textures: with [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) installed, run
`python scripts\enhanced\hd_textures.py`, then extract the levels again (the decompiler command in
ENHANCED.md).

## Change it yourself

### The fastest way: with an AI coding assistant
This mod was made by talking to [Claude Code](https://claude.com/claude-code), Anthropic's coding
agent: you describe what you want in plain words, and it reads the code, makes the change, builds,
runs the game to check, and commits.

1. Install Claude Code (desktop app or `npm install -g @anthropic-ai/claude-code`) and open the
   `jak-project` folder in it.
2. Ask for the setup first: *"Set this project up and build it following README.md; my ISO is at
   C:\...\Jak 3.iso"*.
3. Then ask for changes the way you'd say them to a friend: *"make the night darker"*, *"more Metal
   Heads in the port"*, *"add a menu switch for the fog"*. It's good practice to ask it to explain
   the change and to try it in the game before committing.

[ENHANCED.md](ENHANCED.md) and `AGENTS.md` give the assistant the project's map and rules.

### By hand
- **Graphics** (C++ and shaders): `game/graphics/opengl_renderer/EnhancedPost.cpp` (the post-processing
  passes and the menu) and `game/graphics/opengl/shaders/enh_*`. Rebuild with
  `setup.ps1 -SkipTools -SkipExtract`.
- **Game code** (GOAL, a Lisp): `goal_src/jak3/`. The changes are marked `Enhanced`, for example
  `engine/ai/traffic-h.gc` (population) and `engine/target/logic-target.gc` (the menu's requests).
  Recompile with `out\build\Release\bin\goalc.exe --user-auto --game jak3 --cmd "(mi)"` (a few
  seconds to minutes), then restart the game.
- **Live changes**: start the game with `-debug` (`gk.exe --game jak3 -- -boot -fakeiso -debug`), open
  the compiler (`goalc.exe --user-auto --game jak3`), type `(lt)` then `(mi)`, and change values while
  you play, e.g. `(set! *enhanced-board-speed* 1.6)`.
- **Effect settings**: many sliders are in the debug menu (`-debug`, then "Renderer Debug" → "Enhanced
  Post"). The environment variables in ENHANCED.md (`JAK_ENHANCED_*`) switch effects for a test run.

## Legal
Use your own legally owned copy of the game. Jak 3 is © Sony Interactive Entertainment / Naughty Dog.
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
