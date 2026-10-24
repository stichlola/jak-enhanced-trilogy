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
| God rays | Bright sky smeared toward the sun's screen position (half resolution) |
| Depth of field | Light disk blur from 70 m to 350 m; sky, HUD and near objects stay sharp |
| Bloom | Soft-knee bright pass, 6-level dual-filter blur |
| Grading | Contrast-adaptive sharpen, highlight roll-off, saturation, S-curve, vignette |
| Anti-aliasing | FXAA (Lottes' compact version) |
| Fog and heat haze | Distance + height fog in the game's fog color, thicker below the camera; shimmer over the far ground |
| Motion blur | Camera only: each pixel reprojected into the previous frame's camera; sharp within 10 m (Jak), skipped on cuts |
| Sea reflections | Screen-space rays on the far sea (depth kept right after OCEAN_MID_FAR) and the near sea (OceanNear made to write its surface, then the depth restored); ripples, Fresnel, sky fallback |
| Desert sand | `scripts/enhanced/desert_sand.py`: the jak3-ue5 Tripo sand tile, color-matched to the Wang atlas, in `out/enhanced/desert_sand.png`; the hfrag dunes sample it in world space every 64 m instead of the Wang montage |
| HD textures | `scripts/enhanced/hd_textures.py`: 4x Real-ESRGAN of the opaque textures of Spargus, the desert, vehicles, Jak/Daxter and guns (~2800) into `custom_assets/jak3/texture_replacements` (git-ignored), then extract the levels again (fr3 0.9 → 2.6 GB). Decals/cutouts/glass stay original (their blending shows the color of "transparent" texels); the decompiler skips a replacement that doesn't match its texture scaled back down, because texture names aren't unique |

Environment switches: `JAK_ENHANCED=0` (vanilla look), `JAK_ENHANCED_AO_DEBUG=1` (occlusion and
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

- Real shadow maps / normals for the baked levels (the levels have no normals; lighting is in
  the vertex colors), and ray tracing (OpenGL has no hardware RT: it would need a compute-shader
  BVH or Vulkan interop).
