# Contributing to Jak Enhanced Trilogy

Jak Enhanced Trilogy is a community remaster: fork it, improve it, send the good parts back. This page is
the short version of how the project works. The long version of every feature is in
[ENHANCED.md](../ENHANCED.md).

> This is **not** the OpenGOAL repository. Don't send Enhanced changes or Enhanced bugs to
> OpenGOAL: their project has its own rules ([AI policy](../AI_POLICY.md), upstream
> [contributing guidelines](https://github.com/open-goal/jak-project/blob/master/.github/CONTRIBUTING.md)).
> A bug that also happens with the Classic preset or `JAK_ENHANCED=0` belongs to them; check first.

## The three rules

1. **Never commit game data.** No ISO, no extracted or decompiled files, no textures, models or
   audio from the game, not even upscaled ones. `.gitignore` already covers `iso_data/`,
   `decompiler_out/`, `out/` and the texture replacements; look at `git status` before every commit.
2. **Every change can be turned off.** A menu switch (in `EnhancedPost::draw_menu`, saved in
   `enhanced-settings.json`) or at least a `JAK_ENHANCED_*` environment variable, so anyone can
   compare with the original and find what broke.
3. **Write down how it works** in ENHANCED.md: a row in the graphics table or a paragraph, with its
   switches. The next person (or their AI assistant) starts from there.

## Where things are

| What | Where |
| --- | --- |
| Post-processing passes, the `\` menu, saved settings | `game/graphics/opengl_renderer/EnhancedPost.cpp` |
| Their shaders | `game/graphics/opengl/shaders/enh_*` |
| Level-load extras (vegetation, vines, normals, materials) | `game/graphics/opengl_renderer/loader/Enhanced*` |
| Game ↔ renderer bridge (`pc-enhanced-*` functions) | `game/kernel/common/kmachine.cpp`, `game/graphics/enhanced_state.h`, `goal_src/jak3/kernel-defs.gc` |
| Game code changes (population, menu requests, gameplay) | `goal_src/jak3/...`, marked with `Enhanced` comments |
| Tools (setup, HD textures, sand) | `scripts/enhanced/` |

## Workflow

1. Fork, then clone your fork into a folder named `jak-project` and run
   `scripts\enhanced\setup.ps1 -Iso "<your ISO>"` (see the README).
2. A branch per change. Build with `setup.ps1 -SkipTools -SkipExtract` (C++), recompile the game code
   with `goalc --user-auto --game jak3 --cmd "(mi)"`.
3. Test in the game. The Showcase stops in the menu are handy fixed places; `JAK_ENHANCED_HOUR`,
   `JAK_ENHANCED_POPULATION`, `JAK_ENHANCED_CENSUS` and the others in ENHANCED.md help repeat a test.
4. Open a pull request with before/after screenshots (the template has a short checklist).

## AI assistants

Most of this project was written with Claude Code, and you're welcome to use one too: point it at
README.md and ENHANCED.md first. Say in the pull request which tool you used and how much, and make
sure you understand what it changed: you're the one who answers questions about it.

## Credit

If your fork or video builds on this project, please credit it (see "Credits" in the README).
Keep the credits of the people before you, and add yourself.
