# Jak 3 Enhanced (branch `jak3-enhanced`)

OpenGOAL's Jak 3 with a modern post-processing chain and a few gameplay changes, inspired by
Ratchet & Clank Enhanced. Everything is code: the game data comes from your own copy (PAL here).

## Build and run (Windows)

- VS 2022 with Clang (clang-cl), CMake and Ninja; NASM (`winget install NASM.NASM`, on the PATH).
- The tools find the project by a folder named `jak-project` in their path: work through a
  junction (`mklink /J ...\jak-project ...\jak-enhanced`) or name the checkout that way.
- `cmake --preset=Release-windows-clang` then `cmake --build out\build\Release`.
- ISO data in `iso_data/jak3_pal` (the custom test level still reads `iso_data/jak3`: junctions
  to the same folders work), `goal_src/user/<name>/repl-config.json` with
  `{"jak3": {"gameVersionFolder": "jak3_pal"}}` and `goal_src/user/user.txt` = `<name>`.
- Levels: `decompiler.exe ./decompiler/config/jak3/jak3_config.jsonc ./iso_data ./decompiler_out
  --version pal --config-override "{\"decompile_code\": false, \"levels_extract\": true}"`.
- Code: `goalc --user-auto --game jak3 --cmd "(mi)"` (this branch makes `--cmd` honor the REPL
  config, so the PAL folder is used).
- Play: `gk -v --game jak3 -- -boot -fakeiso -debug`.

## Graphics (game/graphics/opengl_renderer/EnhancedPost.*, shaders/enh_*)

All in one pass on the finished 3D scene, between the 3D buckets and the HUD (so minimap and text stay untouched); sliders under
"Renderer Debug" → "Enhanced Post" in the ImGui menu. About 0-2 fps at 2560x1600 on an RTX 5070
Laptop (the game stays at its 60 fps cap).

| Effect | How |
| --- | --- |
| Ambient occlusion | Depth buffer → view-space positions with the game's own projection (saved in `make_new_cam_mat`), Alchemy AO, depth-aware blur |
| Sun shadows | `pc-enhanced-set-sun` hands the time-of-day key light to the renderer; screen-space rays toward it, 3 m |
| Sun shadow map | Every frame the static geometry of the loaded levels (tfrag and tie, most detailed LOD; their vertex buffers are already in world space) is drawn depth-only from the sun into a 4096x4096 orthographic map over 160 m around the camera, snapped to whole texels; the passages cut into mhcitya/ctyport are cut here too. The composite darkens sun-facing surfaces the map says are blocked (normal offset, 4x4 hardware PCF, faded at the edge; 35 %), the screen-space sun shadows stay on for the characters, and the volumetric mist uses it for crisp light shafts. Characters (merc) don't cast into it: Jak keeps the game's own shadow |
| Dynamic lights | The levels' own lamps (their light spheres, with the time-of-day palette: night lamps switch on at night) sent by the game each frame (`pc-enhanced-set-lights`, within 80 m of the camera), plus the glow sprites (lamps, torches, shots, explosions) recorded by the sprite renderer: up to 32 point lights, nearest and brightest first, shaded in the composite with normals rebuilt from depth and a smooth falloff. No shadows from these lights |
| God rays | Bright sky smeared toward the sun's screen position (half resolution) |
| Depth of field | Light disk blur from 70 m to 350 m; sky, HUD and near objects stay sharp |
| Bloom | Soft-knee bright pass, 6-level dual-filter blur |
| Grading | Contrast-adaptive sharpen, highlight roll-off, saturation, S-curve, vignette |
| Anti-aliasing | TAA: the 3D world's shaders get a Halton(2,3) sub-pixel jitter per frame (wrapped `main()` in `Shader.cpp`, not the HUD or offscreen passes); `enh_taa` blends the frame into the history reprojected with the camera (depth for the world, rotation only for the sky), Catmull-Rom history fetch, variance clipping in YCoCg against trails, less history on fast pixels. FXAA (Lottes' compact version) when TAA is off |
| Fog and heat haze | Distance + height fog in the game's fog color, thicker below the camera; shimmer over the far ground |
| Volumetric mist | `enh_volume`, half resolution: 24 steps along each view ray up to the depth (120 m), ground mist thicker below the camera and drifting as 3D noise, lit by the fog color and the sun (Henyey-Greenstein forward scattering); each step tests the sun through the depth buffer (8 steps toward it), so walls darken the mist behind them. Jittered per frame (the TAA smooths it), depth-aware upsample in the composite. The shafts stay soft: crisp ones need real sun shadow maps, which would replace the screen-space test |
| Motion blur | Camera only: each pixel reprojected into the previous frame's camera; sharp within 10 m (Jak), skipped on cuts |
| Sea reflections | Screen-space rays on the far sea (depth kept right after OCEAN_MID_FAR) and the near sea (OceanNear made to write its surface, then the depth restored); ripples, Fresnel, sky fallback |
| Desert sand | `scripts/enhanced/desert_sand.py`: the jak3-ue5 Tripo sand tile, color-matched to the Wang atlas, in `out/enhanced/desert_sand.png`; the hfrag dunes sample it in world space every 64 m instead of the Wang montage |
| HD textures | `scripts/enhanced/hd_textures.py`: 4x Real-ESRGAN of the opaque textures of Spargus, the desert, vehicles, Jak/Daxter and guns (~2800) into `custom_assets/jak3/texture_replacements` (git-ignored), then extract the levels again (fr3 0.9 → 2.6 GB). Decals/cutouts/glass stay original (their blending shows the color of "transparent" texels); the decompiler skips a replacement that doesn't match its texture scaled back down, because texture names aren't unique |

Environment switches: `JAK_ENHANCED=0` (vanilla look), `JAK_ENHANCED_TAA=0` (FXAA instead of TAA), `JAK_ENHANCED_SHADOWS=<strength>` (0 = screen-space shadows only), `JAK_ENHANCED_MIST=<density /m>` (0 = off) and `JAK_ENHANCED_MIST_SUN=<x>`, `JAK_ENHANCED_LIGHTS=<strength>` (0 = no dynamic lights), `JAK_ENHANCED_HOUR=<0-23>` (set the time of day after boot, for testing), `JAK_ENHANCED_ENEMIES=0` (debug boots: keep the start-of-story city; by default they get the late-game factions, Krimson Guard and Metal Heads out in the streets), `JAK_ENHANCED_SUN_YAW=<deg>` (turn the sun around the up axis, for testing), `JAK_ENHANCED_AO_DEBUG=1` (occlusion and
shadows alone, logs the sun), `JAK_ENHANCED_SUN_FLIP=1` (mirror the sun, for testing). The renderer
logs `[enhanced] average N fps` every 5 s.

## Gameplay (GOAL, live from the REPL)

| Change | Global | Default |
| --- | --- | --- |
| Jetboard top speed and acceleration everywhere | `*enhanced-board-speed*` | 1.35 |
| Guns fire faster | `*enhanced-gun-fire-rate*` | 1.5 |
| Ammo capacity | `*enhanced-ammo-mult*` | 2.0 |
| Desert vehicle driving modes | `(enhanced-drive-mode 'classic / 'sport / 'turbo)` | sport (1.35x torque) |

With `gk` running: `goalc --user-auto --game jak3`, then `(lt)`, `(mi)` (loads the project's types),
then e.g. `(set! *enhanced-board-speed* 1.6)`. `scripts/enhanced/og_repl.py "<form>"` sends forms to
the REPL's nREPL port (8181) from scripts; `scripts/enhanced/capture_window.ps1` screenshots the game
window. Useful forms: `(send-event (ppointer->process *time-of-day*) 'change 'hour 7)`,
`(start 'play (get-continue-by-name *game-info* "wascitya-top-of-stairs"))`.

## Not done (yet)

- Normals for the baked levels (they have none; lighting is in the vertex colors, the dynamic
  lights use normals rebuilt from depth), shadows from the dynamic lights, characters and wind-swayed props in the shadow map, and ray tracing (OpenGL has no hardware RT: it would need a compute-shader
  BVH or Vulkan interop).
