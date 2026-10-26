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
| Sun shadow map | Every frame the static geometry of the loaded levels (tfrag and tie, most detailed LOD; their vertex buffers are already in world space) is drawn depth-only from the sun into a 4096x4096 orthographic map over 160 m around the camera, snapped to whole texels; the passages cut into mhcitya/ctyport are cut here too. The composite darkens sun-facing surfaces the map says are blocked (normal offset, 4x4 hardware PCF, faded at the edge; 35 %), the screen-space sun shadows stay on for the characters, and the volumetric mist uses it for crisp light shafts. Characters and wind-blown trees cast into it too: Merc2 hands over each flush's bones (camera space) and opaque draws, the shadow pass skins them again (`enh_shadow_merc`) and puts them back in the world with the camera; the wind ties' instances are baked into world positions once per level (still matrices); alpha-tested textures cut out leaves and hair. Menu: "Ombre di personaggi e alberi" |
| Dynamic lights | The levels' own lamps (their light spheres, with the time-of-day palette: night lamps switch on at night) sent by the game each frame (`pc-enhanced-set-lights`, within 80 m of the camera), plus the glow sprites (lamps, torches, shots, explosions) recorded by the sprite renderer: up to 32 point lights, nearest and brightest first, shaded in the composite with normals rebuilt from depth and a smooth falloff. No shadows from these lights |
| Fires | A warm glow (orange to yellow, flame sized) that keeps glowing at the same spot for a second, in most frames, is a fire (guns fired from where a guard stands glow there too, but in bursts; shots move on). Its light flickers on top of the game's own, and for the 4 nearest within 45 m the composite bends what's seen through the hot air above the flame and draws 18 embers each, rising, swaying and fading, hidden behind the scene. Menu "Fuochi (calore, braci)", `JAK_ENHANCED_FIRE=<scale>` (0 = off) |
| Wakes | Jak's recent path (a point every 0.1 s while he moves, every 0.5 s standing, kept 2 s): each point near the water's height (his root is his feet: up to 2.5 m under swimming) starts a ring in the water's normals that spreads at 1.4 m/s and fades, so swimming and riding leave trails of rings. Debug slider, `JAK_ENHANCED_WAKE=<strength>` (0 = off) |
| Explosions | `explosion-init-by-other` tells the renderer (`pc-enhanced-explosion`: position, radius): a warm flash of light (the strongest light of the frame, fading in half a second) and a shockwave ring racing out from it that bends the picture. Debug slider "Explosions", `JAK_ENHANCED_BLAST=<strength>` (0 = off) |
| Dynamic light shadows | The 8 strongest dynamic lights cast screen-space shadows: 8 jittered steps through the depth buffer toward the light, up to 3 m and short of the light (lamps sit inside their housing), blocked where the depth is in front of the ray by less than 1.5 m. A fire in a barrel leaves the ground around it dark, Jak blocks a torch. Menu "Ombre delle luci", `JAK_ENHANCED_LIGHT_SHADOWS=<0-1>` |
| God rays | Bright sky smeared toward the sun's screen position (half resolution) |
| Depth of field | Light disk blur from 70 m to 350 m; sky, HUD and near objects stay sharp |
| Bloom | Soft-knee bright pass, 6-level dual-filter blur |
| Grading | Contrast-adaptive sharpen, highlight roll-off, saturation, S-curve, vignette |
| Anti-aliasing | TAA: the 3D world's shaders get a Halton(2,3) sub-pixel jitter per frame (wrapped `main()` in `Shader.cpp`, not the HUD or offscreen passes); `enh_taa` blends the frame into the history reprojected with the camera (depth for the world, rotation only for the sky), Catmull-Rom history fetch, variance clipping in YCoCg against trails, less history on fast pixels. FXAA (Lottes' compact version) when TAA is off |
| Fog and heat haze | Distance + height fog in the game's fog color, thicker below the camera; shimmer over the far ground |
| Volumetric mist | `enh_volume`, half resolution: 24 steps along each view ray up to the depth (120 m), ground mist thicker below the camera and drifting as 3D noise, lit by the fog color and the sun (Henyey-Greenstein forward scattering); each step tests the sun through the depth buffer (8 steps toward it), so walls darken the mist behind them. Jittered per frame (the TAA smooths it), depth-aware upsample in the composite. The shafts stay soft: crisp ones need real sun shadow maps, which would replace the screen-space test |
| Vegetation density | `loader/EnhancedVegetation.cpp`: when a level loads, its grass and small plants (cutout shrubs of crossed cards, under 1.6 m, standing on the floor) get copies around them, turned, scaled and set down on the floor (the level's upward tfrag/tie triangles on a 2D grid; the floor must continue halfway, so no copies over edges or through walls). Then tufts: on the floor textures the game already grows grass on (under 3+ plants), crossed pairs of the nearest plant's own upright cards, up to 6 / m2 next to the plants and fading out within 4.5 m (patches get fuller, sand and paths stay bare), evenly thinned to a budget per level. Copies and tufts are extra shrub draws with a tier: Classic / Moderate (+1 per plant, a third of the tufts) / Lush (+3, all), switched live in the menu |
| Climbing plants | `loader/EnhancedVegetation.cpp`: the level's own wall vines (shrubs with a `vine` texture lying flat on a wall: `city-slum-vine` in the slums, the farms' and the forest's) get copies on the other walls within 30 m, fewer further out: turned to face out of the new wall, as high above the floor in front of it as the original, a few cm off it. The wall is the upright tfrag/tie triangles in one plane chained up and down, so a vine only goes where the whole wall is tall enough (pipes and pillars too); one per 2 x 2 x 3 m cell, not next to an original, a budget of 4000 per level. Extra shrub draws: a third in Moderate, all in Lush; `JAK_ENHANCED_VINES=0` turns them off for A/B tests |
| Grass that bends | The game sends Jak's position every frame (`pc-enhanced-set-player`) and up to 8 enemies, passers-by and vehicles within 30 m of him (`enhanced-update-pushers` in nav-enemy.gc: `enemy` 1.1 m, `rigid-body-object` 2.6 m); grass and leaves (cutout shrubs) near them lean away and press down, tips most, feet not at all, on top of the wind |
| Footprints and ruts | The game sends whether Jak stands on soft ground (the ground's collision material: sand, dirt, snow, swamp) or drives (`pc-enhanced-set-player`, w); the renderer drops a print every 0.7 m of walking, left and right in turn, or a rut piece every 0.5 m of driving (two tracks 1.7 m apart), keeps 48 for 30 s, and the composite presses them into the flat, non-green ground around them: darker inside (the over-bright sand is brought to 1 there first), a lighter rim. Menu: "Impronte nella sabbia"; `JAK_ENHANCED_PRINTS=0` off, `JAK_ENHANCED_PRINTS_TEST=1` lays a row of each beside Jak every 10 s |
| Lit smoke | The 8 strongest dynamic lights of the last frame go back to world space (`g_enhanced_smoke_lights`); the particle renderer (Sprite3) brightens and tints the normally blended particles (smoke, dust, steam) near them per vertex, with the post pass's falloff: smoke over a fire glows orange, an explosion's lights its own cloud, lamps tint the steam at night. Menu: "Fumo illuminato"; `JAK_ENHANCED_LIT_SMOKE=<strength>` (0.8, 0 = off) |
| Far view | The background (tfrag, tie) no longer skips what the game's precomputed visibility leaves out for the camera's spot (coarse cells made for the PS2: buildings popped in round corners and over rooftops); everything in the view frustum is drawn, depth-tested as usual. Actors were already drawn at their best LOD with no distance limit on PC. About the same cost here (renderer ~3 ms/frame either way). Menu: "Distanza visiva estesa"; `JAK_ENHANCED_FAR_VIEW=0` for the game's culling |
| Metal and glass | `loader/EnhancedMaterials.h`: each level texture gets a material from its name (metal: metal, steel, pipe, chrome, iron, grate, gold, copper...; glossy: glass, window panes, marble, crystal, wet...; rusty, old or dirty ones stay plain). TFragment/Tie3 set it per draw (`enh_material`), the background shaders write it in the normal target's 2-bit alpha (1 plain, 2/3 metal, 1/3 glossy). The composite gives metal a reflection of its surroundings (a short screen-space march, else the screen where the ray leaves) tinted by its own color plus a broad sun highlight, glossy surfaces an untinted Fresnel reflection and a tight highlight; faded out from 60 to 120 m. Menu: "Metallo e vetro riflettenti"; `JAK_ENHANCED_MATERIALS=<strength>` (0 off, -1 shows the map: metal red, glossy blue; 2 left half only) |
| Surface relief | The textures' light and dark read as small bumps (bright = raised): the composite tilts the surface normal along the brightness slope (screen-space tangents from the depth, not across depth edges) and shades it by the sun, within 30 m. No tessellation: the PS2 meshes are too coarse to displace, and the post pass can't move geometry. Menu: "Rilievo delle superfici"; `JAK_ENHANCED_RELIEF=<strength>` (0 off, 2 left half only) |
| Level normals | The levels have no normals (their light is baked into the vertex colors): the loader computes smooth ones when it unpacks tfrag, tie and shrub (vertices welded by position, faces more than 60 degrees apart kept out of each other's average; envmapped ties keep the game's own). The level shaders write them to a second color target of the game's framebuffer (opaque, depth-writing draws only), taking the triangle's own face normal where the smooth one strays from it (hard edges). The post pass uses them for sun shadows, dynamic lights, the sun relight, SSAO and SSGI, and still rebuilds normals from depth for water and particles. Characters (merc) write theirs too, the same way (their smooth normals are in view space: turned to world, the face's where they stray) |
| Motion blur | Camera only: each pixel reprojected into the previous frame's camera; sharp within 10 m (Jak), skipped on cuts |
| Sea reflections | Screen-space rays on the far sea (depth kept right after OCEAN_MID_FAR) and the near sea (OceanNear made to write its surface, then the depth restored); ripples plus a long open-sea swell (normals only, calm in the shallows), Fresnel, sky fallback; in shallow water the sea bed is bent by the waves (refraction, only where the bent ray still lands under water) and lit by drifting caustics. The flat see-through discs the game draws a few cm over the water (splashes, ripples) stay water, and the game's own color shows through wherever it is brighter than ours (the foam) |
| Desert sand | `scripts/enhanced/desert_sand.py`: the jak3-ue5 Tripo sand tile, color-matched to the Wang atlas, in `out/enhanced/desert_sand.png`; the hfrag dunes sample it in world space every 64 m instead of the Wang montage |
| HD textures | `scripts/enhanced/hd_textures.py`: 4x Real-ESRGAN of the opaque textures of Spargus, the desert, vehicles, Jak/Daxter and guns (~2800) into `custom_assets/jak3/texture_replacements` (git-ignored), then extract the levels again (fr3 0.9 → 2.6 GB). Decals/cutouts/glass stay original (their blending shows the color of "transparent" texels); the decompiler skips a replacement that doesn't match its texture scaled back down, because texture names aren't unique |

**In-game menu: `\`** opens and closes it (EnhancedPost::draw_menu): every effect on/off, lamp
strength, population and the minimap (hidden unless switched on: `pc-enhanced-minimap?` in
hud-map's update-values!, `JAK_ENHANCED_MINIMAP=1`), the time of day (six hours, a slider, a frozen
clock), a five-stop Showcase (request 9 `<hour> <population or -1> <continue point>`: forest,
Spargus, desert at sunset, the port's war zone, the port at night) and quick travel to nine continue
points (the piazza, port, Hip Hog, slums, industrial zone, central sector, Metal Head city, Spargus,
desert). The renderer queues one request, the game polls it each frame
(`pc-enhanced-poll-request`, `enhanced-menu-update` in logic-target.gc). The cursor is freed from
the mouse camera while it's open.
Three presets at the top (Classic: the game as it was; Medium: the costly passes off, i.e. mist,
SSGI, depth of field, god rays, motion blur, the lights' shadows, refraction and caustics;
Maximum: everything). The menu is in English. The menu's choices (switches, vegetation, population, minimap) are saved to
`enhanced-settings.json` in the game's settings folder whenever they change and loaded at the next
start; the environment switches below still win over them, and are only written to the file if the
player then changes something in the menu.

Environment switches: `JAK_ENHANCED=0` (vanilla look), `JAK_ENHANCED_TAA=0` (FXAA instead of TAA), `JAK_ENHANCED_SHADOWS=<strength>` (0 = screen-space shadows only), `JAK_ENHANCED_SHADOW_CASTERS=0` (only the static level geometry in the shadow map), `JAK_ENHANCED_MIST=<density /m>` (0 = off) and `JAK_ENHANCED_MIST_SUN=<x>`, `JAK_ENHANCED_LIGHTS=<strength>` (0 = no dynamic lights), `JAK_ENHANCED_HOUR=<0-23>` (set the time of day after boot, for testing), `JAK_ENHANCED_ENEMIES=0` (debug boots: keep the start-of-story city; by default they get the late-game factions, Krimson Guard and Metal Heads out in the streets), `JAK_ENHANCED_WATER_SIM=0` (no swell, refraction, caustics), `JAK_ENHANCED_TP=<x,y,z>[,yaw]` (test boots: move Jak there, meters/degrees), `JAK_ENHANCED_VEGETATION=0|1|2` (classic, moderate, lush: default), `JAK_ENHANCED_VINES=0` (no extra wall vines), `JAK_ENHANCED_POPULATION=0|1|2|3` (Haven's passers-by, traffic, guards and Metal Heads: half, the game's, double: default, 3.5x; the enemies, Metal Heads and KG robots, 6x and 10x instead; at most 40 of a kind, guards 16; also in the menu, live. The traffic engine keeps 40 inactive slots a kind (was 20) and 254 active per tracker (was 126), its spatial hash 509 objects; nav meshes take twice their `nav-max-users`, and a citizen whose mesh is full goes back inactive), `JAK_ENHANCED_CENSUS=1` (any game, also the full one: log every 5 s how many guards, Metal Heads and citizens are alive and where Jak is; `2` also every process type), `JAK_ENHANCED_GRASS_PUSH=<m>` (the radius grass bends away from Jak in, 0 = off), `JAK_ENHANCED_NORMALS=0` (normals from depth only; `2` shows them: left the ones used, level normals in color, right the depth-rebuilt ones), `JAK_ENHANCED_MENU=1` or `<kind>:<value>` (menu open from the start, plus one queued request: `1:23` hour, `2:<continue point>`, `3:x,y,z,yaw`, `4:0` frozen clock, `5:1` on the jetboard, `6:<0-3>` population, `7:<frames>` a harmless test explosion in front of Jak every so many frames, `8:<type>` into a vehicle), `JAK_ENHANCED_SUN_YAW=<deg>` (turn the sun around the up axis, for testing), `JAK_ENHANCED_AO_DEBUG=1` (occlusion and
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

City sounds: Haven's sound banks share six half-slots (`city-sound-expand-want-list`); the Krimson
Guard's and the Metal Heads' banks (`citykgf`, `citymhf`) take two each, the vehicles' (`citycarh`:
engines, ignition, thrust, crashes), the Freedom League's, the passers-by's and the district's one
each. With both factions out in the streets the vehicles' bank was the one left out, so stolen cars
and bikes were silent: `update-sound-info` (cty-borrow-manager.gc) now puts it first. Names the game
plays that no bank has (`guard-hit`, `damage-pops`...) are just leftovers, not missing sounds.
`JAK_ENHANCED_MENU=8:<vehicle type>` puts Jak in a new vehicle (0-2 bikes, needs ctycarb; 3-5 cars,
ctycara; 7 Hellcat) once its level is in, for testing.

## Known issues

- Water and PBR (metal/glass) reflections and refraction: fewer artifacts than before, but still
  shimmer and smears where the screen-space rays leave the picture, and refraction at the shoreline.
- Extreme population crashes in Haven's war zone near the port: `nav-mesh::new-nav-control: too many
  users` even with twice the users (some caller other than the citizens' `traffic-activate` doesn't
  check for a full mesh). High is fine; the Showcase's war-zone stop uses High.

## Not done (yet)

- Ray tracing (OpenGL has no hardware RT: it would need a compute-shader
  BVH or Vulkan interop).
- Life in the Jak 2 plaza (`piazza`): passers-by, traffic, guards and Metal Heads come from the
  traffic engine, which spawns them along the city nav graph's branches (`activate-one-citizen` /
  `activate-one-vehicle` in traffic-engine.gc, over the segments of its vis cells) and gives each the
  nav mesh of its node (`nav-mesh-id` -> `get-nav-mesh`); enemies need that mesh to walk at all. The
  plaza has neither: the jak3 level builder doesn't write `nav-meshes` (commented out in
  `goalc/build_level/jak3/LevelFile.cpp`). Needed, in order: (1) an `entity-nav-mesh` for the plaza,
  written by the builder (polys of up to 4 vertices, max 255, adjacency, a 2-bit next-hop route table,
  the `poly-hash` grid prebuilt) or built in GOAL at login like `init-from-entity` does; (2) plaza nodes
  and branches in the city's `nav-graph` (assigned to that mesh), linked to the nodes at the two
  passages; (3) the traffic engine's segments for them, so the spawner sees the plaza.
