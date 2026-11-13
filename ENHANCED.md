# Jak Enhanced Trilogy

OpenGOAL's Jak and Daxter trilogy (Jak 1, 2 and 3) with a modern post-processing chain and a few gameplay changes, inspired by
Ratchet & Clank Enhanced. Everything is code: the game data comes from your own copies (PAL here).

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

## Fast dev loop (scripts/enhanced/dev_*.ps1, Jak 3)

Run each script with `& ([scriptblock]::Create([IO.File]::ReadAllText('<repo>\scripts\enhanced\<name>.ps1'))) <args>`.

- `dev_launch.ps1`: opens the game from a **shadow copy of the bin folder** (`dev_bin.ps1`:
  `local\run\<stamp>-<tag>\bin`, 63 MB in 0.05-0.3 s, idle copies pruned after an hour or beyond the
  last 5), so `build.bat` can always relink `out\build\Release\bin` while games run. `--proj-path`
  points at the main tree. Shader hot reload (`JAK_ENHANCED_SHADER_RELOAD=1`) is on. It also starts the
  instance's dev goalc. With no arguments it is the user's game (instance 0, own config); agents use
  `-Instance n` (n >= 1: ports +10n, a test config in the run dir, windowed 640x360), `-Headless`
  (`--no-display`), `-Start <continue>`, `-Env @{...}`, `-Wait`, `-Minimized`. `multi_run.ps1` uses a
  shadow copy too, and accepts an instance whose goalc port is held by that instance's dev goalc.
- `dev_goalc.ps1 -Instance n [-Wait|-Status|-Eval '<form>'|-Stop]` runs `dev_goalc.py`: one goalc per
  instance, kept running (the cold `(mi)` is paid once, 30-50 s, in the background; then `(lt)`).
  It answers on 127.0.0.1:8185+10n (JSON lines): each form goes through goalc's nREPL followed by a
  marker form that fails on purpose, whose error print flushes goalc's block-buffered stdout, so the
  answer is the text in between. goalc sits in a job object (dies with the daemon) and its stdin is an
  open pipe (no 100% core spin). Log: `local\run\goalc_i<n>.log`.
- `dev_reload.ps1 -Instance n [-Files <.gc>...] [-All] [-Force] [-Mi] [-Lt]`: `(ml "<file>")` into the
  running game for each file that differs from HEAD (or untracked) and changed since the last
  `(mi)`/`(ml)`, reverted files included. Prints `OK <file> <ms>ms loaded`, `ERROR <file>:<line> <msg>`,
  `NEEDS <file> mi+restart (...)` (headers `*-h.gc`, `.gp`/`.gs`, a changed `deftype`/`defenum`; `-Force`
  loads them anyway), then `RESULT ok= error= needs= ms=`; `$LASTEXITCODE` 0/1. `-Mi` queues a
  background `(mi)` so the next boot has the change (an engine file cascades: ~30 s).
- Measured (Jak 3, headless instance): one-file change to running code went from 35-90 s ((mi) +
  gk restart) to **0.45-0.6 s** for `engine/target/logic-target.gc` (4859 lines, compile+load
  0.28-0.40 s) and **0.2 s** for a small file (0.12 s compile+load); a compile error comes back in
  0.37 s with file:line; a no-change call 0.17 s. Cold start (gk + dev goalc in parallel, bound by the cold `(mi)`): 34-46 s, once.
- Caveats: a hot load runs the file's top-level forms again (its `define`d globals are re-created); a
  process already inside a state keeps the old code until it enters the state again; type layouts can't
  change in a running game. **gk accepts one goalc connection per run** (`Deci2Server::accept_thread_func`
  returns after the first client): after a goalc restart, restart the game too. Keep the dev goalc and
  restart only gk instead; the dev goalc reconnects (`(lt)`) on the next reload.
- `dev_build.ps1 [-Targets auto|all|gk,goalc] [-Refresh]`: the C++ build without `build.bat`'s
  overhead. It runs ninja directly with the vcvars64 environment cached in `local\run\vcvars.env`
  (calling `vcvars64.bat` costs ~1.6 s per build), builds `gk` only unless `goalc/`, `common/` or
  `decompiler/` differ from HEAD (or goalc.exe is older than a goalc/ source), and prints the slowest
  steps from `.ninja_log`: `STEP <ms> <output>`, then `BUILD OK|FAIL targets= ms= steps= compile=`
  (on failure the compiler's error lines first). Games of the dev loop run from shadow bins, so the
  link never waits for a running game.
- C++ build time: `EnhancedPost.cpp` (7250 lines) is split, by moving whole functions only, into
  `EnhancedPost.cpp` (setup, auto quality, ocean, fires, debug window), `EnhancedPostScene.cpp`
  (`apply_to_scene`), `EnhancedPostLight.cpp` (grade and weather, focus, exposure, shafts, GI, shadow
  maps, mist, clouds), `EnhancedPostPasses.cpp` (SSAO, resize, prepare, ray traced cover, the composite's
  uniforms) and `EnhancedPostMenu.cpp` (the menus, settings, presets); what they share (world.json,
  the menu's widgets, the hand-placed lights, the sun) is in `EnhancedPostInternal.h`, inline in
  `namespace enh_post`, so each global is still one object. The dev build dir also precompiles the
  runtime's common headers (`game/enhanced_pch.h`: STL, fmt, json, FileUtil with windows.h), CMake
  option `ENHANCED_PCH`, off by default so release builds compile exactly as before; turn it on once
  with `cmake -DENHANCED_PCH=ON out/build/Release` (a full rebuild, ~3 min). `common/versions/revision.h`
  is rewritten only when the commit changes, so a CMake re-run (a CONFIGURE_DEPENDS glob seeing a new
  file) costs ~5 s instead of relinking everything.
- Measured (one-line change, quiet machine): `EnhancedPost.cpp` touched, `build.bat` 7.3-8.2 s
  (compile 5.4-6.2 s, of it 2.4 s parse) → `dev_build.ps1` **2.3-3.1 s** (compile 1.6-2.4 s for any of the
  five files); `EnhancedNotes.cpp` compile 3.0 → 2.1 s; no-op build 2.5 → 0.3 s. Not done: sccache (a
  compiler cache only helps rebuilds of unchanged content: branch switches, touch, worktrees; it is not
  installed, `winget install Mozilla.sccache` then `-DCMAKE_CXX_COMPILER_LAUNCHER=sccache`).
- `dev_test.ps1 -Scenario <name> | -Start <continue> [-Env @{...}] [-Headless|-Windowed] [-Expect <regex>...]
  [-Forbid <regex>...] [-Ready <regex>] [-Timeout s] [-Instance n] [-Keep] [-StopGoalc]`: one in-game
  check asserted on log lines. It boots a test game (dev_launch: shadow bin, instance >= 1, minimized)
  straight into the scenario, waits for the ready line (with a start point the first `[camera]` line with
  the camera placed: test boots log it every 2 s, headless too), runs the scenario's variants in the
  same game, then checks the regexes and closes the game. Scenarios are `scripts\enhanced\scenarios\*.json`
  (`desert-boot`, `tune-cycle`, `repl-cycle`; the keys are in the script's header): a variant sends a
  GOAL form through the dev goalc (`eval`, its output checked with `answer`), writes
  `local\tuning_i<n>.json` (`tune`, removed afterwards), expects log lines after it, takes a
  screenshot (`shot`, windowed, at most 4); `wait` lets game time pass before the variant acts, `settle`
  after it. A launch right after a game on the same instance waits up to 15 s for the gk port. Output: `PASS|FAIL <check> t=<s>`, then `RESULT pass= fail=`;
  `$LASTEXITCODE` 0/1. `-Headless` (`gk --no-display`) works for Jak 3: logic only (states, census,
  AI, REPL forms), no renderer, so no tuning, shaders, fps lines or shots.
- Measured: `desert-boot` headless 7.5 s from launch to RESULT; `tune-cycle` windowed (two tuning
  variants in one run) 18 s; `repl-cycle` headless ~55 s with a cold dev goalc (its first `(mi)`, by
  then idle Jak is in `target-stance-ambient`, which the check accepts), 9 s once it is warm. Before: a windowed boot per variant (14-20 s each) and a screenshot
  looked at by eye.

## Release gate (scripts/enhanced/release_gate.ps1, Jak 3)

Before a release, `release_gate.ps1` (run through `[scriptblock]::Create`, `-Repo <tree>`) boots the
built game once per scenario row and judges each row from the outside. Rows that only need log lines
run **headless** (`gk --no-display`, ~240 MB, `-Parallel 4` side by side); rows that need the renderer
(fps, weather, the menu, pictures, streamed cutscenes) or load a level after the boot run in a small
windowed test instance (`-Instances 1`; a headless gk logs "Adding level X" for a level asked mid-run
and never finishes loading it). Instance numbers stay in 6-12 (`-FirstInstance/-LastInstance`), each
row has its own config dir under `local\gate\<time>\cfg-<row>`, so the user's saves and settings are
never touched. Checks: gk alive at the end, no Application event 1000 for its PID, the levels in order
(the row's `Levels`, else its start point's level), frames coming (windowed: `[enhanced] average N fps
... in <level>`, average >= 20; headless: the test boot's `[camera] ... in <level>` every 2 s),
screenshots not black, and the row's `Expect` lines in order, each with its value (`(?<v>...)`) checked
against a range where the row gives one (hour reached ~23, nap hours ~6 from `nap start/end at <hour>`,
Ashelin's 37 shots with arm-along-shot >= 0.9, the reload and `reloaded 20`, Sig's gun-along-shot,
the orbs counted, the bright floor in the arena 0.8 % against 1.18 % measured). A row ends early
once all its checks are met for `-Settle` s (`-NoEarly`: full length). The test switches
`JAK_ENHANCED_MENU=<kind>:<value>`, `JAK_ENHANCED_MENU_SCRIPT` and `JAK_ENHANCED_NAP` run on the game's
thread (kmachine.cpp, `pc_enhanced_set_scene`), so they work headless too.

The rows (`-List`; `-Only a,b` picks some, `-Games` keeps room for Jak 1/II): boot at desert-start and
Spargus, the menu's hour picks at boot and in a burst, the nap and its window, the parked Hellcat
flown, desert to Spargus on foot and in the Hellcat, Spargus to the desert, the air train's cutscene
and the train itself, the arena's training orbs, the weather script in the desert and Spargus, the
menu open (log line only: the baseline compare is the picture check), a save loaded by a second boot,
and the playable characters: `player-jak|ashelin|sig` (`JAK_ENHANCED_PLAYER_TEST=run`: walk, run,
run-jump, jump, double jump, square, circle; the `[player-test] state <name> at <frame>` changes must
show target-walk, target-jump, target-double-jump, target-running-attack, target-attack (and the
characters' own moves over them: `[enhanced-move] ashelin-front-kick`, `sig-elbow`, `sig-spin-punch`);
and `[player-test] body-jump
run|jumps|attacks max <m>`: the drawn body's main joint against its root, most per frame, <= 0.4 m for
the skins, measured 0.07-0.16; for Jak himself data only, his main joint leaves his root by up to 3 m in
the walk/run blends) and their guns headless (`player-jak-gun`, `player-ashelin-gun`, `player-sig-gun`).

The gk tested: `-Bin <dir>`, else `$env:JAK_RELEASE_BIN`, else `out\build\Release\bin` (copied into the
run's folder). `release.ps1` passes `-Bin out\build\Release-static\bin` (the exe that ships), `-Version`
and `-MakeBaseline`, so every release adds its shots to its own baseline and the next one compares with it.

Screenshots against a baseline: `-MakeBaseline` adds the run's 320 px shots to
`local\gate\baseline\<version>\` (`-Version`, default `$env:JAK_RELEASE_VERSION` or git describe) as one
more rerun sample (the newest `-BaselineKeep` 4 kept; FAILed rows not added); run it 2-3 times on one
build to give the rows real limits. `-Baseline <dir|version|none>` (default: the newest other baseline)
compares each shot at 64x36 (mean luma difference, SSIM over 8x8 windows) with every sample and keeps the
best match, and a change past the limits makes the row **WARN** (it passes), with baseline and new side by
side in report.html. Limits per row and shot: from the samples (each one's best match among the others,
the worst of those, less a margin that shrinks with more samples: sim -0.10/-0.07/-0.06 and diff
+4/+3/+2.7 for 2/3/4 samples, clamped to 0.5-0.97 / 3-30; worked out at compare time), else carried over from the
baseline the one-sample baseline was made beside, else the row's `MinSim`/`MaxDiff` (default 0.85 / 6;
air train and desert-to-Spargus rows wider). Measured at 64x36: reruns of one build diff 0.2-5.8 / sim
0.81-1.00 (the minimap shown or not at 14 s, ground decals, pose; the air train scene at 10 s has two
looks, 0.63-0.99; after desert-to-Spargus Jak and the camera stop elsewhere each run, 0.59-0.99); a
magenta box over 20 % of the frame diff 7.4-9.4 / sim 0.76-0.79; the desert from another camera 0.50;
another level 0.15. The player shots can't tell Jak from Sig (0.90): their log lines check who is played.
A shot with no baseline counterpart gets an info line. Output: `GATE <row>
PASS|WARN|FAIL <fails; warns; every matched check with its value; info>`, `GATE RESULT pass= warn= fail=
minutes=`, report.txt (one line per matched Expect with its log line) and report.html in
`local\gate\<time>\`; numbers with dot decimals; exit code 1 on any FAIL. A full run: 15 min (15 windowed rows one
at a time beside 10 headless, 4 at once, most rows ending early), was ~40 min (25 windowed rows one by
one, full length).

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
| Fires | A warm glow (orange to yellow, flame sized) that keeps glowing at the same spot for a second, in most frames, is a fire (guns fired from where a guard stands glow there too, but in bursts; shots move on). Its light flickers on top of the game's own, and for the 4 nearest within 45 m the composite bends what's seen through the hot air above the flame and draws 18 embers each, rising, swaying and fading, hidden behind the scene. Menu "Fuochi (calore, braci)", `JAK_ENHANCED_FIRE=<scale>` (0 = off). A kept flame's hue is put back once more at the end of the composite, at the brightness the picture has there: after the first restore the lamps' glow in the air, the eye's adaptation and the lights still lay over it, and a torch's flame came out pale cream with lavender tips at night and under a roof (Spargus' wall torches); now the game's orange and yellow, never less saturated than the game's |
| Wakes | Jak's recent path (a point every 0.1 s while he moves, every 0.5 s standing, kept 2 s): each point near the water's height (his root is his feet: up to 2.5 m under swimming) starts a ring in the water's normals that spreads at 1.4 m/s and fades, so swimming and riding leave trails of rings. Debug slider, `JAK_ENHANCED_WAKE=<strength>` (0 = off) |
| Ride puffs off hover vehicles | Dust, wet-ground spray, water plumes and ruts come only off wheeled vehicles (the desert's `wvehicle`) and the board. A hover vehicle (zoomers, hover cars, the gunship, the Hellcat) leaves none: `logic-target.gc` `enhanced-piloting-hover?` sets player flag 8 (+4096), `EnhancedPostScene.cpp` skips its puffs and ruts. Before, a zoomer in Haven's rain trailed two big white-blue blobs (the wet-ground spray meant for wheels). The wake on the water stays. Off: tuning `water.hover_puffs` 1 (old plumes and ruts). Haven slums 14h rain, zoomer, `JAK_ENHANCED_PUFF_TEST`: composite 7.43 ms old vs 7.19 new (1280x720, 60 fps). No rain on them either: `Merc2.cpp` flags as DRY the draws of hover vehicles (bikea/b/c, newbike, cara/b/c, hellcat, kg-robot-transport: piloted, parked or in traffic) and, while Jak pilots one (flag 8), of Jak and Daxter: `enh_wet` -1 for that draw (no wet look in `merc2.frag`) and the normal tagged 0.36 instead of 0.45, which the composite reads as "no rain" (no scenery wet look, no `rain_chars`); other characters stay wet. Off: tuning `water.hover_dry` 0. Haven 14h rain: zoomer, parked zoomer and hover car back in their own colors, Jak on foot still wet |
| Puddles in real dips | The wet ground's puddles gather where the ground is lower than around it: `ground_hollow` reads the depth buffer on two rings (0.6 and 1.4 m, 12 taps) around each level wet pixel nearer than 70 m and averages the neighbors' height over it (hidden neighbors and walls over 25 cm count as level). A 1 cm dip is a full puddle: the foot of steps and curbs, the joints between flagstones, real hollows; level ground only in the noise's highest spots, more in a thunderstorm (the noise now only rags the rims). Past 50-70 m the noise as before. Ripples, splashes and reflections unchanged. Off: `shaders.tune_water_dbg` 42 (noise puddles everywhere, as before); 43 paints the puddles blue. Haven slums 14h rain: composite 8.59 ms old vs 8.79 new (1280x720, 4 interleaved pairs, 60 fps) |
| Lightning shadows | The bolt's light on the scene (`tune_bolt_scene`) now casts shadows: while a bolt flashes (`weather2.w` > 0.04, about 0.4 s; the same bolt event schedules the game's thunder, `pc-enhanced-thunder`) its direct share stops where the depth buffer has something between the point and the bolt's side of the sky (`light_blocked_by`, 6 m, 12 steps, faded out 60-120 m). Jak, posts and railings throw a hard shadow for the flash's instant. Off: `shaders.tune_water_dbg` 41; 44 paints the shadows red. Haven slums 21h thunderstorm, bolt held (`JAK_ENHANCED_BOLT_TEST=0.1`): composite 8.98 ms old vs 9.10 new, only while it flashes (60 fps) |
| Explosions | `explosion-init-by-other` tells the renderer (`pc-enhanced-explosion`: position, radius): a warm flash of light (the strongest light of the frame, fading in half a second) and a shockwave ring racing out from it that bends the picture; then 40 sparks fly out (mostly up, 7-20 m/s, drag and gravity, 0.5-1.4 s), drawn as short streaks along their motion from white-hot to orange, hidden behind the scene, and the hot air over the blast wobbles for a second. Debug slider "Explosions", `JAK_ENHANCED_BLAST=<strength>` (0 = off); menu "Explosion sparks", `JAK_ENHANCED_SPARKS=<strength>`. By day (`look.blast_day`, 0 = off) the fireball hides more of what is behind it, its core stays orange-yellow, its light within the highlights' room and its soot darker, and the flash added after the highlights' squeeze only fills the room the pixel has left (the rest as the fire's hue): no white blot over noon sand. `look.blast_punch` (0 = off): an orange-yellow fireball core (no lemon-white), its soot thinner and gone by 1 s, a darker, smaller, shorter dust ring; the game's grey puffs within reach of the last 4 explosions turned to darker, thinner soot that clears over `look.blast_soot_life` (4.5 s; `shaders.tune_soot_dark` / `tune_soot_thin`), their additive sprites (`shaders.tune_soot_flash`) and the game's glow (`look.blast_glow`: the near-white star with rays, worst by night) kept in the fire's orange; lit smoke brightens a puff at most `shaders.tune_smoke_soot_cap` (1.3x, the flash had taken it to 2.6x: milky white; `tune_smoke_soot` 0 = as before). Cost: a few ALU ops per sprite vertex, a loop over 4 blasts per glow on the CPU. With `look.blast_punch` the fireball is drawn past the grade (before it, the day's stop-down and the desert grade took it for lit sand: a see-through ring in the sand's color under the game's sprites): a solid ball standing on the ground, as thick as the ball itself (not as the slice of the ray over the ground), yellow-hot, then deep orange with darker folds, a red lumpy rim, then soot; finer folds as it grows (a vehicle's 10 m ball); its brightest channel held at 1, so no cream core by night. The dust ring stays before the grade. The flash's light dies at `look.blast_flash_fade` (11 /s; at 5 it stood past the ceiling for 0.3 s: a flat orange pool over the ground by day) |
| Gun shot ribbons | Each shot of Jak's guns leaves a ribbon along its path (red grenade streak, thin crackling blue, long yellow tracer, wide wavering dark), drawn with the sprites, depth tested, plus sparks where it hits. Jak 2/3's camera gives w < 0 in front, so the ribbon shader takes the sign the game's sprite shader uses (before, every ribbon was clipped away). By day (`look.shot_day`, 0 = off) a ribbon covers up to 85% of what's behind it with its color and is 1.6x wider, so it reads by hue over pale sand instead of clipping to white; at night it is added light, as before. Test: `JAK_ENHANCED_SHOT_TEST=1` fires a shot ahead of Jak every 0.6 s, each gun in turn |
| Dynamic light shadows | The 8 strongest dynamic lights cast screen-space shadows: 8 jittered steps through the depth buffer toward the light, up to 3 m and short of the light (lamps sit inside their housing), blocked where the depth is in front of the ray by less than 1.5 m. A fire in a barrel leaves the ground around it dark, Jak blocks a torch. Menu "Ombre delle luci", `JAK_ENHANCED_LIGHT_SHADOWS=<0-1>`. Shadow maps for lamps (tuning `lights.shadow_maps` 0-8, default 8: every shadow-casting lamp; the arrays hold that many cubes, 6 MB each, two arrays; two lights in one place share a map; only where the lights' shadows are on, the High preset): the strongest still lamps (`lights.shadow_still_frames` 30 within `lights.shadow_still_m` 0.5 of where first seen: flames jitter ~20 cm; a lamp with a map ranks `lights.shadow_keep_rank` 2 places higher, so flickering fires don't swap slots) get a cube map each (one 512² cube map array, `EnhancedPost::do_lamp_shadows`); the levels' geometry (the sun map's casters) is drawn once per lamp, 2 faces a frame, and kept; each frame only the faces with a character within `lights.shadow_chars_m` (12 m), or within `lights.shadow_chars_cam_m` (15 m) of the camera and in the lamp's reach (Jak under a far brazier), are copied back and get the characters (merc replay). Near plane `lights.shadow_near_m` 0.5 (the lamp's own housing doesn't cast). The composite reads them for those lights instead of the depth march (4 taps, `tune_lamp_map_soft`; `tune_lamp_map_dbg` shows map red / march green, `tune_lamp_map_dbg_slot` one slot only). 8 maps instead of 4: the temple's courtyard braziers (ranked 5th-8th behind lamps up the stairs) now get maps, and the wall's arches and pillars shade the firelight on it instead of a flat wash; slums 7 of 8 lamps, port 5 of 5; cost the same (0.04-0.25 ms GPU, 0.05-0.18 ms CPU, 60 fps). Port 21h: Jak and Daxter cast their real shape instead of a blur; 0.10 ms GPU, 0.09 ms CPU. Spargus 21h, temple braziers, slums 21h (414 character draws): no artifacts, 0.04-0.12 ms GPU, 0.06-0.12 ms CPU, 60 fps either way |
| God rays | Bright sky smeared toward the sun's screen position (half resolution) |
| Depth of field | Light disk blur from 70 m to 350 m; sky, HUD and near objects stay sharp |
| Cutscene depth of field | The game says when a cutscene plays (`pc-enhanced-set-scene`, `movie?`): nine depth samples around the middle of the screen go into a pixel buffer and are read a frame later (no stall); the focus eases toward the nearest of them (log space). The composite gathers 24 samples over up to 9 px (1080p) with each sample's own blur (thin-lens style, from its distance to the focus): a sample counts where its blur reaches this pixel, something behind only as far as this pixel's own blur, so a sharp face doesn't smear over the background. Replaces the far depth of field while it runs. Menu "Cutscene depth of field"; `JAK_ENHANCED_CINE_TEST=1` turns it on outside cutscenes |
| Bloom | Soft-knee bright pass, 6-level dual-filter blur |
| Grading | Contrast-adaptive sharpen, highlight roll-off, saturation, S-curve, vignette. Per area: the game sends the level Jak is in and the hour every frame (`pc-enhanced-set-scene` from `update-time-of-day`); the level's name picks an area (Haven, desert and Spargus, forest, Metal Heads, underground, volcano, temples, `zone_of`), each with a day and a night look (shadows lifted toward a tint, highlights tinted, saturation, contrast, warmth; `kZoneLooks`), night from 19-21 h to 5-7 h; eased over 1.5 s. Menu "Color grade per area", `JAK_ENHANCED_GRADE=0`. A zone may also have an `"evening"` look (Haven: violet shade, amber light), eased in as the sun goes down (16:30-18:30) and out into the night; tuning `grade.evening` (0 = off) |
| Clouds | By day the sky is ours (`sky_color`: the fog's color at the horizon deepening to blue overhead, the sun's glow and disk, one grey when overcast), blended over the game's; the game's painted cloud cards kilometers out above the horizon (Haven) count as sky and go. Over it a cloud volume (`cloud_volume`): a slab 1100-2000 m up marched in 18 jittered steps through 3D noise (flat bottoms, heaped tops, cover from the weather), each step lit by the sun through two samples toward it, bright forward scattering, undersides in the sky's color, lit from inside by lightning, melting into the haze far out. Clear days (`look.sky_extras`, 0 = off): deeper overhead, a pale haze band hugging the horizon (warmer toward the sun, thicker in the desert) and the sun's aureole; the sun's disk stays the brightest of the sky (warm white, as far as the clouds let it through), not the dark disk it drew when the sky was brought under 0.9 |
| Night sky lights | By night the game's lights drawn over its own sky (Haven's tower beacon, the port towers' searchlights, lamps) are kept over our sky as drawn (game picture minus the game's sky), over its open sky and over its far cloud cards (z > 2.8 km). Before, a card pixel took the beam away with the card: the searchlights were slabs cut clean at the cards' edges, floating mid-air (the user's port shot: on main too, not lane2's WIP). Weighed by the brightest such light on two rings, 1.2% and 3.5% of the screen height (16 taps, sky and card pixels only), not by the pixel's own: per pixel the beams' soft edges and fading tips were squared away (dc * w(dc)). A fixed 10 px reach was too short for a close beam and drew a faint box at its edge. The stars are dimmed only behind the light itself. Faint light alone (rain streaks, card seams) is still let go. Where the level covered a light of the game's sky (the moon behind a card) the sky 3.9% of the height around is the reference. Also at dusk and dawn (the searchlights burn 19:00-7:00): the keep ran only in the game's share of the sky (0 at 19:00, full at 21:00), so the beams faded in over two hours, and at dawn the daytime sharpness keep put the game's pixel back (beam over its still night-dark sky): dark hatched slabs over our pale morning sky. While the sun is low (`dusk`) light drawn over the game's sky is added over ours too (minus what the sprites drew, already put back with the effects) instead of replacing the pixel. Off: `tune_sky_keep_night 0`; `2` = by night only (before the dusk fix); `-1` = per pixel, open sky only (old). Cost: composite 9.2-9.4 ms for -1 / 1 / 0 alike (Haven port under the tower, 22h, rain, 1920x1200); the dusk fix within noise (3.3-5.3 ms for 2 / 1 / 0 at 19.2h, 20h, 6.5h, 1280x720); at 17.8h (no searchlights) the frame diff new vs old is 6.2, under the moving clouds' own 6.9 |
| Sky glows' faint edge | Over our daytime sky the glow sprites' faintest light (their quad's edge) is let go by a soft gate (`smoothstep(0.5k, 1.5k)`, k = `tune_fx_sky_edge` 0.02), not by taking k off all of it: that thinned every halo (sun, moon, beacons) by as much, their middle too. `tune_fx_sky_edge` < 0 = the old subtraction, 0 = all of it kept. No square at the quad's edge (Haven port 16.5h, sun halo crop) |
| Weather | Menu "Weather" (`JAK_ENHANCED_WEATHER=0..7`): clear; changing (a four-minute cycle per area, `JAK_ENHANCED_WEATHER_PHASE=<s>` to start into it: in Haven and the forest sunny for the first minute, clouds build, rain at 80 s with a thunderstorm 110-150 s, the sun back out on the wet ground at 160 s, sunny again at 220; fog on early mornings; in the desert light dust that thickens into a storm, on top of the game's own); rain; sandstorm; thunderstorm; after the rain; morning fog; sunny (Jak 1's Sandover: saturation x1.38, warm gain, shadows lifted toward blue, a deep blue sky gradient where no cloud is, few clouds). Everything eases in over 4 s; the ground soaks in 6 s and dries in 90. Rain: drops in the world (a 2D DDA walk of grid cells along the view ray, each holding a column of falling streaks; three grids for near, mid and far), splashes on wet level ground, puddles in the dips of a world-space noise with raindrop rings, level ground reflects its surroundings (most in the puddles and at grazing angles), the dynamic lights glint on it and glow with halos in the wet air; characters darker with a tight highlight and the sky on their grazing sides (merc2, `enh_wet`). Thunderstorm (work in progress): darker clouds, a bolt every 1.5-5 s in the sky only (the leader steps down in 0.09 s, then the return stroke, two restrikes, branches), drawn and flashed after the grade so it stays white; the game's own thunder sounds (`thunder-a/b/c`) from the bolt's direction a moment later (`pc-enhanced-thunder`). Tests: `JAK_ENHANCED_BOLT_TEST=<age s>` holds a bolt ahead, `JAK_ENHANCED_RAINBOW_TEST=1` puts the sun behind the camera. After the rain: broken clouds, stronger sun shafts, richer colors, steam lying on the ground, a rainbow opposite the sun (primary and secondary bows). Ground fog: the height-fog integral along the ray, thickest just under the camera. Dust: the game's `desert-dust-storm` sends its intensity and wind every frame (`pc-enhanced-set-dust`); the composite marches 12 steps (jittered) through a volume of 3D noise stretched along that wind and carried by it (rolling clouds, low streamers near the camera, thicker near the ground), then the rest at the average; lit by the sun, brighter toward it |
| Soft shadows | PCSS on the sun shadow map: the same map bound again through a sampler without comparison (`m_shadow_raw_sampler`, unit 13), 12 taps over 1.5 m find the average depth of what's in the way; the penumbra is that gap x 0.02 (0.05 to 1.2 m), filtered with 16 hardware-compared taps on a Vogel disk turned per pixel and frame (the TAA smooths it), the bias growing with the filter. Menu "Soft shadows", `JAK_ENHANCED_SOFT_SHADOWS=<penumbra per m>` (0 = the 4x4 filter) |
| Anti-aliasing | TAA: the 3D world's shaders get a Halton(2,3) sub-pixel jitter per frame (wrapped `main()` in `Shader.cpp`, not the HUD or offscreen passes); `enh_taa` blends the frame into the history reprojected with the camera (depth for the world, rotation only for the sky), Catmull-Rom history fetch, variance clipping in YCoCg against trails, less history on fast pixels. Characters and vehicles (merc) carry their own motion vectors (`enh_motion`, `EnhancedPost::do_motion`, tuning `taa.motion`, default 1): each model of the frame (`EnhancedCasters::instances`) is found again in the last one by name and place (bones' mean, the camera's move taken out, within 3 m), skinned with both frames' bones into an RG16F uv offset where it is the front surface; the TAA takes the nearest pixel's offset instead of the camera's reprojection, so a running Jak or a flying Hellcat keeps its own history instead of smearing (0.09 ms at 2560x1600; `taa.dbg 1` shows them red, `taa.motion_log 1` lists the models). FXAA (Lottes' compact version) when TAA is off |
| Fog and heat haze | Distance + height fog in the game's fog color, thicker below the camera; shimmer over the far ground |
| Volumetric mist | `enh_volume`, half resolution: 24 steps along each view ray up to the depth (120 m), ground mist thicker below the camera and drifting as 3D noise, lit by the fog color and the sun (Henyey-Greenstein forward scattering); each step tests the sun through the depth buffer (8 steps toward it), so walls darken the mist behind them. Jittered per frame (the TAA smooths it), depth-aware upsample in the composite. The shafts stay soft: crisp ones need real sun shadow maps, which would replace the screen-space test. Lamps and fires glow in the air (`volume.local_lights`, on; 0 = off): the 8 nearest lights within 40 m scatter in a thin air of their own (`volume.light_air` 0.06/m, `volume.light_gain` 1), a soft halo around a torch at night. The halo is integrated along the ray in closed form (`volume.light_shape` 1): a2/(a2+d2) minus its value at the halo's reach (`volume.light_core_m` 1, `volume.light_halo_m` 6), clipped to the sphere, so it is exactly 0 from 6 m out and the dark between the lamps keeps its black; with no mist there is no march at all. `volume.light_shape` 0 = the old marched lobe (with `light_halo_m` 8), which summed to a pale veil over a lamp-lit alley |
| Vegetation density | `loader/EnhancedVegetation.cpp`: when a level loads, its grass and small plants (cutout shrubs of crossed cards, under 1.6 m, standing on the floor) get copies around them, turned, scaled and set down on the floor (the level's upward tfrag/tie triangles on a 2D grid; the floor must continue halfway, so no copies over edges or through walls). Then tufts: on the floor textures the game already grows grass on (under 3+ plants), crossed pairs of the nearest plant's own upright cards, up to 6 / m2 next to the plants and fading out within 4.5 m (patches get fuller, sand and paths stay bare), evenly thinned to a budget per level. Copies and tufts are extra shrub draws with a tier: Classic / Moderate (+1 per plant, a third of the tufts) / Lush (+3, all), switched live in the menu. The desert's height field (kept from the level that has it) counts as another level's ground only if at least half of that level's own small plants over it stand on it (desertb 85%, Spargus' wascitya 4%: it runs on above Spargus' streets, where the game never draws it, and its strewn stones and grass floated up to 8 m over them past the garage), and stones are strewn on it only where the level's own floor right there isn't 0.15-8 m above or below it |
| Climbing plants | `loader/EnhancedVegetation.cpp`: the level's own wall vines (shrubs with a `vine` texture lying flat on a wall: `city-slum-vine` in the slums, the farms' and the forest's) get copies on the other walls within 30 m, fewer further out: turned to face out of the new wall, as high above the floor in front of it as the original, a few cm off it. The wall is the upright tfrag/tie triangles in one plane chained up and down, so a vine only goes where the whole wall is tall enough (pipes and pillars too); one per 2 x 2 x 3 m cell, not next to an original, a budget of 4000 per level. Extra shrub draws: a third in Moderate, all in Lush; `JAK_ENHANCED_VINES=0` turns them off for A/B tests |
| Smoother characters | `loader/EnhancedSmoothMerc.cpp`: when a level loads, the listed models (Jak, Daxter, major allies, common enemies; Jak 3, lod0 and cutscene versions) get one pass of PN-triangle-style subdivision: each edge gets a midpoint curved by its two ends' normals, with UVs and colors per side and the bones of both ends (the 3 strongest), so the game's animations still apply. One midpoint per edge between welded vertices (same place, same bones): UV seams and hard edges stay closed; hard edges and sharp turns stay straight. The draws stay 1:1. Vertices the game moves on its own (`mod_draw`: blend-shape faces, ripple) and eyes keep their triangles, and the edges they share with the rest aren't split, so the join doesn't crack. Menu "Smoother characters" (after a restart), `JAK_ENHANCED_SMOOTH_CHARS=1`, saved as `smooth_chars`; off by default |
| Replacement characters (engine support) | `decompiler/level_extractor/merc_replacement.cpp`, `extract_merc.cpp`, `Merc2.cpp`: a rigged glb in `custom_assets/<game>/merc_replacements/` replaces one merc model at extraction, with `<model>.json` as its switch: joints matched by name to the game's skeleton, the game's eyes and blend-shape face kept on request (now also when no blend shape plays), lower lods sharing it. In Jak 3 such a model gets all its bones and isn't hidden by the old model's effect switches. Faces and animation fidelity are the risk: [docs/character-models.md](docs/character-models.md). Vehicles: [docs/vehicle-models.md](docs/vehicle-models.md). No model ships; untested with one |
| Detailed scenery on TIE | `loader/EnhancedScenery.cpp`: TIE protos (crates, barrels, lamps, small rocks) can be listed in `custom_assets/<game>/scenery/scenery.json` like shrub protos: the model takes each copy's place and turn, fitted into its height, its foot on the proto's foot, tinted toward the proto's texture and lit by the copy's time of day color; instanced and culled per frame, with far versions and impostors if present, dithered against the proto (flagged in the normal's 2 spare bits, `tfrag3.frag`), hidden when the game hides the proto. Not for protos in the wind, with an environment map, or in water. Collision and sun shadows stay the proto's. `JAK_ENHANCED_SCENERY_LIST=1` logs the level's TIE protos too; menu "Detailed scenery", `JAK_ENHANCED_SCENERY=<m>` |
| Desert props | Four original models (tree, bush, boulder, scavenger cart) strewn around the desert start as placed objects (`placed.json`, group `desert_props`; models in `local/editor-kit/jak3/scenery/desprop_*`, not shipped yet): `scripts/enhanced/desert_props.py` cleans and sizes each one in Blender (merged doubles, degenerate triangles, specks; base color only, 1-2K), writes its wind weight in COLOR_0 (tree: leaves 0.6 + height 0.4; bush: height) and a `<model>_col.glb` stand-in collision (trunk cylinder, hull, box); `scenery_import.py` makes the far version and impostor; `desert_props_place.py` sets them on the dunes' height field (hfrag), away from the levels' geometry and the start, tilted a little with the slope. A placed model with its own weights sways by them (`shrub_inst.vert`, a few cm, the weather's wind: more in a sandstorm); placed objects cast sun shadows (their far versions, `enhanced_placed_shadow_draw`) |
| Palms and trees in the wind | Trunks nearly still, fronds and leaves swing, more in a sandstorm or a storm (the weather's wind, `EnhancedWind::now`, 0.5 calm to 3). Desert palms (desertb/e/h) are static TIE, Spargus' palms a static TIE trunk under a game wind tie (crown, fronds, beard: its origin up in the crown), Haven's slum trees game wind ties; static TIE and tfrag both get weights now. At load `EnhancedTieWind.cpp` classes each texture: trunk = trunk/bark/root/stump; leaves = a cutout with a plant's name, or any other part of a tree or palm (des-palm-top: the crown and frond stems, so the blades don't tear off them). A leaf vertex stores its height (dm) and its **reach**: meters to the nearest trunk point of its plant (trunk points thinned to 0.5 m, hashed in 2 m cells), or with no trunk near its height capped at 1.5 m (bushes, planters, hanging vines; they used to lean up to ~30 cm). `tfrag3.vert` (TIE and tfrag; TFragment now sets `enh_tie_wind`): the trunk leans `tune_tie_trunk` 0.0015 m per meter of height (a 10 m palm: ~1 cm calm, ~4.5 cm in a sandstorm; was 0.03), a leaf adds `tune_tie_frond` 0.02 m per meter of reach (downwind push and a bob, phased per plant: a 4 m frond tip ~6 cm calm, ~25 cm in a sandstorm) plus the old 3.5 cm flutter; `tie_wind.vert` the same on the wind ties' leaves, reach = distance from the instance's axis, the wind turned into the instance's own axes (`enh_tie_frond`); their shear uses `tune_tie_trunk` too. Switches: `shaders.tune_tie_wind 0` still (as the game), `tune_tie_trunk 0.03` + `tune_tie_frond 0` = the old whole-tree lean; 10 paints the weights (trunks green by height, leaves blue by reach), >= 20 paints how far each vertex moves (red = 30 cm) at strength tune-19 (22 = a sandstorm's). `JAK_ENHANCED_TIE_WIND_LOG=1` logs classes, spots and wind ties per level. Verified at the desert-start oasis (displacement view, new vs old, calm and x3): trunks stay dark, crowns move; old: whole palms red. Load: desertb 90 ms. Cost at 1600x900: GPU 6.85 vs 6.80 ms off, 60 fps Wind ties (Spargus palm crowns on static trunks, Haven slum tree tops): one sway phase per tree (from its origin, so fronds don't stretch); a crown on a static trunk also moves at its origin by that trunk top's lean (the origin's height over the plants' foot field, 0-12 m, kept in `TieWindInstance::enh_lean_h`, not saved; `tie.crown_lean 0` off); `tie_wind.vert` paints like `tfrag3.vert` at tune_tie_wind 10 and >= 20 (`enh_tie_paint`). Verified with a held photo-mode camera (tuning `debug.photo 1` + `debug.view [x,y,z,yaw,pitch]`, game still, so frame pairs 0.5 s apart differ only by the wind): Spargus palm (wascityb 1770/-332) crown pixels changed 0% / 2.3% / 9.1% at tune 0 / 1 / 3, trunk 0.00-0.01%; Haven slum tree (ctyslumb 631/-252) crown 0.2% / 5.2% / 12.8%; the trunk/crown joint at x3 shows no gap (the beard covers it), with or without crown_lean. `tree.wind_leaf` marks both wind draws of wascityb (palm-leaf-worn, palm-beard) and ctyslumb/c (tree-top, flowerbed-flowers). Weather wind (`[enhanced] wind now` log): calm 0.71, desert sandstorm spell ~2.0 (not 3: speed/9 + 1.4 x dust, so tune_tie_wind 3 at calm ~ a sandstorm); Spargus streets and Haven get no sandstorm (town_dust 0, no dust in Haven), only rain/storm raise it there (0.96 seen). Sandstorm fps at 1280x720 with two other games running: desert 57-60, Spargus 59-60, Haven 57-58, wind on vs off within noise (GPU 16.5 vs 16.7, 15.5 vs 13.4-17.0 ms). |
| Grass that bends | The game sends Jak's position every frame (`pc-enhanced-set-player`) and up to 8 enemies, passers-by and vehicles within 30 m of him (`enhanced-update-pushers` in nav-enemy.gc: `enemy` 1.1 m, `rigid-body-object` 2.6 m); grass and leaves (cutout shrubs) near them lean away and press down, tips most, feet not at all, on top of the wind |
| Footprints and ruts | The game sends whether Jak stands on soft ground (the ground's collision material: sand, dirt, snow, swamp) or drives (`pc-enhanced-set-player`, w); the renderer drops a print every 0.7 m of walking, left and right in turn, or a rut piece every 0.5 m of driving (two tracks 1.7 m apart), keeps 48 for 30 s, and the composite presses them into the flat, non-green ground around them: darker inside (the over-bright sand is brought to 1 there first), a lighter rim. Menu: "Impronte nella sabbia"; `JAK_ENHANCED_PRINTS=0` off, `JAK_ENHANCED_PRINTS_TEST=1` lays a row of each beside Jak every 10 s |
| Lit smoke | The 8 strongest dynamic lights of the last frame go back to world space (`g_enhanced_smoke_lights`); the particle renderer (Sprite3) brightens and tints the normally blended particles (smoke, dust, steam) near them per vertex, with the post pass's falloff: smoke over a fire glows orange, an explosion's lights its own cloud, lamps tint the steam at night. Menu: "Fumo illuminato"; `JAK_ENHANCED_LIT_SMOKE=<strength>` (0.8, 0 = off) |
| Far view | The background (tfrag, tie) no longer skips what the game's precomputed visibility leaves out for the camera's spot (coarse cells made for the PS2: buildings popped in round corners and over rooftops); everything in the view frustum is drawn, depth-tested as usual. Actors were already drawn at their best LOD with no distance limit on PC. About the same cost here (renderer ~3 ms/frame either way). Menu: "Distanza visiva estesa"; `JAK_ENHANCED_FAR_VIEW=0` for the game's culling |
| Softer load-in | A level that starts being drawn (loaded, or back after half a second away) dithers in over 0.4 s instead of popping in whole: its tfrag, TIE, shrubs, the detailed scenery and impostors on them, its grass fields and its characters and props (`background_common.cpp` `enhanced_level_fade_in`, the same 4x4 dither in `tfrag3.frag`, `shrub.frag`, `shrub_imp.frag`, `merc2.frag`). Only a gap of `bg.fade_gap` frames (30) restarts it: with 2 a level that skipped a few frames while Haven loaded dithered again and again. The placed objects have their own fade (they come back near a tree, `placed.fade_in` 0.4 s), and the last tenth of their 400 m reach fades them out instead of a cut (`placed.fade_edge` 0.1). Off: tuning `bg.fade_in 0` (all), `fg.fade_in 0` (characters), `bg.fade_scenery 0` (scenery and grass), `placed.fade_in 0`, `placed.fade_edge 0` |
| Load doors wait for the renderer | The game's load doors (`com-airlock` and its kinds: Spargus garage <-> desert, Spargus' airlocks, Haven's) open only when the game's levels behind them are loaded AND the renderer has them (`airlock.gc` `enhanced-door-render-ready?`, called from `destination-loaded?`): `pc-enhanced-level-ready "<bsp>"` (kmachine.cpp, from `Loader::publish_ready` once a frame) is 1 uploaded (all loader stages: geometry, textures with their HD and PBR maps, detailed scenery), 2 drawn and still fading in, 4 placed objects' models still loading. Door levels resolve aliases (`desert-game`) and borrowed levels. At most 8 s (`*enhanced-door-wait-max*`) past the game's own condition, then the soft fade stays as the fallback. Levels a door opened onto are shown whole, not dithered in (`pc-enhanced-level-whole`, `enhanced_level_fade_skip`). Log: "[enhanced] load door <name> ready after <s> s" (on a timeout also the level waited on). `*enhanced-door-wait*` #f turns it off. Measured (Spargus garage <-> desert, on foot via debug.route): 5.1 s extra wait on the first cold entry into Spargus, 0.0 s once warm; no dither after the door opens. |
| Test pad and routes (test runs) | Automated runs that really travel instead of teleporting, so levels stream in as in play (`game/sce/libpad.cpp`, pad 0 only, nothing when the keys are absent). Tuning `debug.pad` = [left x, left y, right x, right y, buttons]: sticks -1..1 (left y -1 forward), buttons a bit mask (X 16384, Triangle 4096), pressure full (the cars' throttle is X's pressure). `debug.route` = [x0, z0, x1, z1, ...] m: the left stick steers toward each point from the camera's view (on foot camera-relative, in a car the camera sits behind it), `debug.pad`'s buttons held meanwhile (X = gas); `debug.route_mode` 0 on foot (a jump when stuck 1.5 s), 1 vehicle; `debug.route_reach` 10 m; `debug.route_flip` 1 the other side. The log has "[enhanced] route ..." once a second and per point. With `debug.vehicle` 13 (Sand Shark) a desert drive from desert-start out to z 1760 m and back into the wasdoors garage, then a walk into Spargus. Placed objects log "[enhanced] placed objects fade in" each time their fade starts. |
| Metal and glass | `loader/EnhancedMaterials.h`: each level texture gets a material from its name (metal: metal, steel, pipe, chrome, iron, grate, gold, copper...; glossy: glass, window panes, marble, crystal, wet...; old or dirty ones stay plain; rougher classes in a step of the normal length that the composite still reads as plain scenery: wood and walls with less of a wet sheen, rusty metal without reflections, painted metal as a coat, tile softer than glass; `JAK_ENHANCED_MATERIALS=-1` paints the classes). TFragment/Tie3 set it per draw (`enh_material`), the background shaders write it in the normal target's 2-bit alpha (1 plain, 2/3 metal, 1/3 glossy). The composite gives metal a reflection of its surroundings (a short screen-space march, else the screen where the ray leaves) tinted by its own color plus a broad sun highlight, glossy surfaces an untinted Fresnel reflection and a tight highlight; faded out from 60 to 120 m. A screen hit on a surface's back (its level normal along the ray) is rejected and the ray goes on; the fallback (the screen where the ray vanishes) is trusted only where that pixel is sky or 40-150 m off, and an untrusted reflection keeps 0.3 of its weight (no smeared copy of Jak or a near wall, no flat fog color on walls facing the camera). The water's screen-space reflection (Hi-Z trace and its march) rejects back-face hits the same way (a dock's or crate's top seen from below); its fallback is unchanged. With ray-traced reflections on, the trust is 1 (unchanged). `shaders.tune_hiz_dbg` 3 = the old reflections (metal, glass and water), 4 = the trust in grey. Checked: ctyinda with Jak in front of the metal rail (the old fallback put a red copy of his scarf on it), dusk 18.6h and night 22h, the slums window panes at 12h and 19h, Haven port water at 18h, RT on; the cost is within noise (Haven port 18h 1600x900: composite 6.64 vs 6.66 ms, steady 60 fps). Menu: "Metallo e vetro riflettenti"; `JAK_ENHANCED_MATERIALS=<strength>` (0 off, -1 shows the map: metal red, glossy blue; 2 left half only) |
| Ray-traced reflections | `loader/EnhancedRayTrace.cpp`: when a level loads (loader thread), its static tfrag and tie geometry (most detailed LOD, world space; no water, glass or shrubs) goes into a bounding volume hierarchy (binned SAH, 4 triangles a leaf; Spargus 160k triangles in 0.06 s, 12 MB). Each triangle keeps its texture (a 64x64 layer of a texture array, mipmapped, sampled with its UVs; mean color as a fallback) and its first vertex's time-of-day color. The post pass uploads each level's hierarchy once and copies the levels in use into three shader storage buffers (OpenGL 4.3); the composite (`rt_trace` in enh_composite.frag) traces a ray through them where the screen-space reflection misses (off the screen, behind something): water within 200 m (faded out from 150 m, up to 400 m away), metal and glass (60 m). A hit is shaded like the game's background (time-of-day color at the current hour x 2 x the texture's mean color) with distance haze, so reflections are dark at night. Characters and actors aren't in it. Off by default for now (still has artifacts); menu "Ray-traced reflections"; `JAK_ENHANCED_RT_REFLECTIONS=0|1|2|3` (off, on, traced only, the traced color alone on the water with misses in magenta), `JAK_ENHANCED_RT=0` doesn't build the hierarchies. No hardware ray tracing: OpenGL has no access to it, so this is a software tracer in the fragment shader |
| Ray-traced ambient occlusion | `enh_ssao.frag` (`rt_occlusion`): 6 cosine-weighted rays of 3 m from each pixel within 80 m through the same hierarchies, a hit counting more the closer it is; the result is the darker of it and the screen-space occlusion (characters aren't traced, so their contact shadows stay screen-space), then the usual depth-aware blur. Off by default for now (too slow in play); menu "Ray-traced ambient occlusion", `JAK_ENHANCED_RT_AO=0`, `JAK_ENHANCED_RT_AO_LENGTH=<m>`, sliders in the debug window |
| Surface relief | The textures' light and dark read as small bumps (bright = raised): the composite tilts the surface normal along the brightness slope (screen-space tangents from the depth, not across depth edges) and shades it by the sun, within 30 m. No tessellation: the PS2 meshes are too coarse to displace, and the post pass can't move geometry. Menu: "Rilievo delle superfici"; `JAK_ENHANCED_RELIEF=<strength>` (0 off, 2 left half only) |
| Parallax relief | Rock and brick or stone walls (by texture name; solid ones, never cutouts, blended draws or decals) really stand out near the camera: the background shader marches the view ray through the texture's brightness as height (4-8 steps, full to 4 m, gone by 20 m), the picture moves, depth doesn't. Jak 3 for now. Slider "Relief (parallax)"; `JAK_ENHANCED_PARALLAX=<strength>` (0 off) |
| Walls repeat less | The terrain's anti-tile sampling for walls whose texture really repeats (uvs past one tile, per axis, checked when the level comes up), out to 60 m. In the background shader, not the composite: the post pass only has the finished picture. Offsets in quarter tiles, so courses of bricks stay in line. Jak 3 for now. "Walls repeat less"; `JAK_ENHANCED_WALL_NOTILE=0` off |
| Character light | `merc2.frag`: a rim of the key light around the silhouette and a soft highlight; light wrapped past the terminator, warm (skin and cloth scatter it), and a cool rim from the ambient light on the shadow side. Menu "Character lighting" |
| Level normals | The levels have no normals (their light is baked into the vertex colors): the loader computes smooth ones when it unpacks tfrag, tie and shrub (vertices welded by position, faces more than 60 degrees apart kept out of each other's average; envmapped ties keep the game's own). The level shaders write them to a second color target of the game's framebuffer (opaque, depth-writing draws only), taking the triangle's own face normal where the smooth one strays from it (hard edges). The post pass uses them for sun shadows, dynamic lights, the sun relight, SSAO and SSGI, and still rebuilds normals from depth for water and particles. Characters (merc) write theirs too, the same way (their smooth normals are in view space: turned to world, the face's where they stray) |
| True PBR | `scripts/enhanced/pbr_maps.py` (run by `hd_setup.py` right after the 4x upscale, CPU, parallel, cached by key; 8 processes; Jak 3: 5644 maps in 30 s, 333 MB) makes a companion for every HD texture from its 4x picture: half its size, R G a tangent-space normal (height = the brightness high-passed from a texel to a sixteenth of the picture, set to a common contrast, Sobel), B a roughness offset (rougher where busy and in the hollows), indexed by `<tpage>/<name>` in `out/enhanced/pbr/<game>/index.txt`. `loader/EnhancedPbr.cpp` reads them on the loader thread with the level (kept only when half the HD texture's size: a texture whose HD version the decompiler refused isn't its) and uploads them with its textures; none installed, nothing changes. The background shader (`tfrag3.frag`, unit 11) tilts the level normal by the map with a cotangent frame from screen-space derivatives (the levels have no tangents), strength and roughness by material class (`PBR_K/PBR_R/PBR_RR`: rock most and polished, sand soft and rough, walls, wood matte, metal, glass none), and writes the roughness as 6 bits in the albedo target's spare low bits (10-bit target, 8-bit textures; albedo alpha 1 = coded, the other level shaders write under 2/3). The composite (constants only, `PBR_*`: no uniforms there) shades the map's bumps against the surface's broad facing (replacing the luminance relief there, fading 60-150 m), adds the sun's GGX highlight (Smith, Schlick) to the plain classes with the sun's strength read back from the picture (lit color over the texture's own), takes the Fresnel's share out of the diffuse, all through the sun shadow map and AO, out to 300 m; the lamps' highlights take the roughness (GGX's Blinn-Phong equivalent); metal and glass keep their reflections with the map's roughness, faded from 80 m (was 60). Characters (merc2, mode 3 of `enh_relief`): the map's normal and a GGX key-light highlight per part (skin, cloth, leather, metal tinted, hair). Menu "True PBR (normal and roughness maps)", `JAK_ENHANCED_PBR=0` (no maps read: as before), tuning `pbr.on 0` (off live). The albedo target (now 10-bit RGB10_A2) is on whenever PBR is, even with `lights.albedo` off (`albedo_on` 0.25 = PBR only); like the level normals it is filled only when SSAO runs, so the Performance preset (SSAO off) shows no PBR on the levels |
| Rock near and far alike | Desert rock and rocky ground (wascity-ground-01: Spargus outskirts by the torch bunker, desert-start) read the same as you walk toward them: before, far off a uniform pale yellow, close up covered in black speckles and cracks. Measured on rocky-ground pixels in distance bands, high-frequency detail over vanilla's: close up 3.6-6.4x, 20-40 m 0.8-1.3x. Causes, in order: (1) the PBR relief: the half-size maps' bumps lose ~5x of their slope to the mips by 40 m (RMS 0.28 at mip 0, 0.18 at 2, 0.11 at 3, 0.055 at 4), and close up the composite's shade, clamped at -0.5 x `PBR_RELIEF` 1.6, turned a pit facing away from a low sun to a fifth of its light; (2) the ground's sun relief, its slope taken over 6 cm, so each pebble texel shaded as a dark pit; (3) its pebble grain at full strength near, gone by 15-50 m. Fix: `tfrag3.frag` `enh_pbr_gain`: the map's relief x`tune_pbr_near` 0.5 at full size, (lost slope)^`tune_pbr_keep` 0.6 given back as the mip coarsens (anisotropic lod, slope capped at ~70 deg); `enh_composite.frag` (constants only, no uniforms): `PBR_RELIEF_DARK` 0.15 (the relief's dark side rolls off: at most ~24% darker), the ground's slope over at least two pixels, `GROUND_RELIEF_DARK` 0.7 (was 0.55), `GROUND_PEBBLE_NEAR` 0.4, `GROUND_DARK_SOFT` 0.4 (the grain's and the plates' dark side, near and far alike), its plates out to `GROUND_PLATES_FAR` 60-140 m and onto the far color, which also keeps `GROUND_FAR_GRADE` 0.7 of the near grade's darkening (`GROUND_MOTTLE_FAR` 0: the near mottling not carried far). Result, walking toward the same slab (Jak at ~45/25/12/3 m, cloud shadows off, detail over vanilla's): noon slab 0.82/2.21/2.24/2.08x -> 0.76/1.27/1.20/1.18x (near/far 2.5x -> 1.55x), its mean 0.67/0.62/0.56/0.57 of vanilla (3 m vs 25 m -8%), dark pixels at 3 m 23.5% -> 14.9%; noon ground 2.04/1.10/1.98/2.38x -> 1.48/0.83/1.04/1.41x, mean 0.65/0.63/0.67/0.70; 8:00 (low sun on the slab) slab 1.26/1.26/1.02/0.98x -> 1.24/1.09/0.76/0.74x, ground 2.29/1.18/2.25/2.49x -> 1.58/0.95/1.53/1.82x, ground mean 0.59/0.59/0.55/0.54. Light and shadow still read. Off: tuning `shaders.tune_pbr_even 0` (the maps as before; 2 = a one-frame A/B test, right half only, assumes a 1280 wide window), `shaders.tune_ground_fine -1.8` (the composite part as before): together they match the pre-change shaders (frame difference at noise level). Cost: none measurable, 60 fps at 1280x720 windowed, GPU 9.7-10.2 ms on vs 10.7 off, post 6.4-6.5 vs 7.0-7.2 ms. At 8:00 the slab close up is 24% darker than at 25 m in ours and in the old path: the sun shadow map marks its face unlit close up (`sun_lit` ~0.02) while vanilla lights it, not this change. Lane1's stronger `PBR_K` constants multiply with this gain: check them with it on |
| Sand detail material | A tileable PBR detail laid in world space over the desert sand: the game's HD sand keeps its color, the material adds grains and small ripples (normal), a roughness offset and a little AO. `scripts/enhanced/detail_materials.py` makes `game/assets/enhanced/materials/sand_detail.png` (512 RGBA: R G normal along world X/Z, B roughness offset, A AO; from our own seamless sand art, wrap-pad seam test, height = luminance band-passed at grain and ripple scale, Sobel) and `detail.json` says where it goes (rules by texture name and class, e.g. `des-sand-grass-01` on tfrag/TIE, and `"hfrag"`: the desert's dunes). 0.9 m tile plus a copy 2.7x as big turned 37 degrees, their weights varied by a slow noise; slopes added UDN-style to the level normal (tfrag3.frag after the PBR map's, hfrag.frag); full to 5 m, gone by 45 m (`shaders.tune_detail_fade`), mips flatten it farther. The dunes shade the fine normal themselves by the sun, its height capped at 55 degrees so the grain still reads at noon. Off: menu "Sand detail material", tuning `detail.on` 0, `JAK_ENHANCED_DETAIL=0` at boot (no file read); off in the Performance preset. Strength `shaders.tune_detail`, `tune_detail_shade`. |
| Rocky ground detail (desert-start's slab) | The big flat slab at desert-start (wascity-ground-01, rocky ground) read as a soft, blurry plane: its remade 1024 picture (`texture_remade/_all`) is smooth, and its PBR map (256) was made from the old HD picture, so no relief matched it; the composite's ground grain only shades brightness. A second detail material, `ground` (`ground_detail.png`, 512, from the composite's own generated rocky-ground art: stone plates, cracks, gravel; `detail_materials.py --only ground`, soft AO: min 0.8, else cracks read as black specks and the slab went 10% darker), is laid on it in world space through `detail.json` (rule by name + `"class": "ground"`; rules now take `rock` and `ground` classes), 2.2 m tile, normal 1.0, rough 0.25, ao 0.3. At desert-start, slab crop (high-frequency \|dx\| / mean luma, on vs off): noon 2.58 vs 2.05 (+26%), mean 108 vs 113; 8:00 3.07 vs 2.37 (+30%), 72 vs 78; at 16:30-18:30 the slab lies in the hill's shade (no change, as it should). Cost: two texture reads on that texture's draws only, 60 fps held; GPU on/off at 1600x900 within noise (another test game on the GPU). Off live: `shaders.tune_detail_rock 0` (the sand's material kept); with the sand's switches too. |
| Motion blur | Camera only: each pixel reprojected into the previous frame's camera; sharp within 10 m (Jak), skipped on cuts |
| Sea reflections | Screen-space rays on the far sea (depth kept right after OCEAN_MID_FAR) and the near sea (OceanNear made to write its surface, then the depth restored); ripples plus a long open-sea swell (normals only, calm in the shallows), Fresnel, sky fallback; in shallow water the sea bed is bent by the waves (refraction, only where the bent ray still lands under water) and lit by drifting caustics. The flat see-through discs the game draws a few cm over the water (splashes, ripples) stay water, and the game's own color shows through wherever it is brighter than ours. Splashes: the game's sprites (foam, drops, ripple rings) are put back over our water from the copies around the sprite buckets (T23 before, T24 after, its alpha 1 - the blended sprites' coverage, capped by the color change): texture, alpha falloff and soft edges as drawn, only the foam lit like the wake's foam (dimmer at dusk: what the blended sprites covered and the pale, low-saturation spray), coloured light over the water (shots, their flash, eco, lamp glows, light beams) at the game's strength, out of the grade at gain 1 like the other effects over water. The splash's ripple mesh (`skel-generic-ripples`, warp bucket, drawn after T24, writes depth) is recognised where the picture changed since T24 near the water: it stays water (above the surface up to 3 m, under it with the depth before the near sea as the bed) and shows the picture before it, so the flat pale-blue hexagon of the game's water and the tan slab over a swimming Jak are gone. A jump into the water (falling > 5 m/s, then stopped) leaves a wake point marked as a splash (w + 20): a wide ring racing out with a thinner one behind and a broken foam line. Off: `tune_water_dbg 35` (shader, as before), tuning `water.splash_ring 0` (the ring); `tune_water_dbg 36` sprites only, `37` warp/water/disc mask, `38` every sprite dimmed (first version), `39` foam weight. From ~20.8h the dusk path fades into the old rule (copies stop at full night), no jump. Cost: composite 3.18 vs 3.19 ms at 1280x720 (Spargus sea), 3.34 vs 3.32 ms (Haven canal 18h). Shore foam (the sea's foam along the waterline) is lit like the wake's: full white in the noon sun, dimmer at dusk, night-dimmed, darker in the sun's shadow, never darker than the water under it (the old fixed white glowed along every shore after sunset). Off: `tune_water_dbg 40` (fixed white), `32` no shore foam. Spargus shallows (standing camera, 12h): foam cells +8 luma over none old, +4..7 new; composite 3.24 vs 3.26 ms (19.5h, 1280x720) |
| Desert sand | `scripts/enhanced/desert_sand.py`: the jak3-ue5 Tripo sand tile, color-matched to the Wang atlas, in `out/enhanced/desert_sand.png`; the hfrag dunes sample it in world space every 64 m instead of the Wang montage |
| HD textures | Made by each player from their own disc, never shipped: the launcher asks once per game and runs `scripts/enhanced/hd_setup.py` (pip, Real-ESRGAN downloaded into `out/tools`, texture dump, `hd_textures.py` 4x into `custom_assets/<game>/texture_replacements`, `--hud` and `minimap_maps.py` into `out/enhanced/hud`, levels extracted again). Decals/cutouts/glass keep their original alpha, and their colors are upscaled from an opaque copy (Real-ESRGAN blacks out the color under alpha 0, and many of them are drawn opaque with something else in their alpha: a face's gloss, eyelids, hair. Daxter's cutscene face was black); an update that changes how they are made redoes them once (`hd_setup.py --refresh`, the launcher after an update; `out/<game>/hd-version.txt`); the decompiler skips a replacement that doesn't match its texture scaled back down (texture names aren't unique). HUD and menu art is loaded at runtime with its texel scale (`EnhancedMapTextures.cpp`), so it needs no new extraction. The Tripo icons are new drawings and live in the repo (`game/assets/enhanced/hud`); their cells not redrawn are see-through magenta, filled from the game's own atlas |

**In-game menu: `\`** opens and closes it (EnhancedPost::draw_menu): every effect on/off, lamp
strength, population and the minimap (shown by default, `JAK_ENHANCED_MINIMAP=0` hides it:
`pc-enhanced-minimap?` in hud-map's update-values!; its size, `pc-enhanced-minimap-size`), the time of day (six hours, a slider, a frozen
clock) and quick travel to nine continue
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

Environment switches: `JAK_ENHANCED=0` (vanilla look; in a running game tuning `debug.classic` 1 applies the Classic preset and 0 puts the settings back, for ON/OFF shots in one run; `debug.hour` <0-23> holds that time of day while it is set, `debug.blast_every` <frames> sets off the test explosion in front of Jak every that many frames (0 stops), `debug.blast_radius` <m> draws every explosion at that size), `JAK_ENHANCED_TAA=0` (FXAA instead of TAA), `JAK_ENHANCED_SHADOWS=<strength>` (0 = screen-space shadows only), `JAK_ENHANCED_SHADOW_CASTERS=0` (only the static level geometry in the shadow map), `JAK_ENHANCED_MIST=<density /m>` (0 = off) and `JAK_ENHANCED_MIST_SUN=<x>`, `JAK_ENHANCED_LIGHTS=<strength>` (0 = no dynamic lights), `JAK_ENHANCED_HOUR=<0-23>` (set the time of day after boot, for testing), `JAK_ENHANCED_ENEMIES=0` (debug boots: keep the start-of-story city; by default they get the late-game factions, Krimson Guard and Metal Heads out in the streets), `JAK_ENHANCED_WATER_SIM=0` (no swell, refraction, caustics), `JAK_ENHANCED_TP=<x,y,z>[,yaw]` (test boots: move Jak there, meters/degrees), `JAK_ENHANCED_VEGETATION=0|1|2` (classic, moderate, lush: default), `JAK_ENHANCED_VINES=0` (no extra wall vines), `JAK_ENHANCED_POPULATION=0|1|2|3` (Haven's passers-by, traffic, guards and Metal Heads: half, the game's, double: default, 3.5x; the enemies, Metal Heads and KG robots, 6x and 10x instead; at most 40 of a kind, guards 16; also in the menu, live. The traffic engine keeps 40 inactive slots a kind (was 20) and 254 active per tracker (was 126), its spatial hash 509 objects; nav meshes take twice their `nav-max-users`, and a citizen whose mesh is full goes back inactive), `JAK_ENHANCED_CENSUS=1` (any game, also the full one: log every 5 s how many guards, Metal Heads and citizens are alive and where Jak is; `2` also every process type), `JAK_ENHANCED_GRASS_PUSH=<m>` (the radius grass bends away from Jak in, 0 = off), `JAK_ENHANCED_NORMALS=0` (normals from depth only; `2` shows them: left the ones used, level normals in color, right the depth-rebuilt ones), `JAK_ENHANCED_MENU=1` or `<kind>:<value>` (menu open from the start, plus one queued request: `1:23` hour, `2:<continue point>`, `3:x,y,z,yaw`, `4:0` frozen clock, `5:1` on the jetboard, `6:<0-3>` population, `7:<frames>` a harmless test explosion in front of Jak every so many frames, `8:<type>` into a vehicle), `JAK_ENHANCED_SUN_YAW=<deg>` (turn the sun around the up axis, for testing), `JAK_ENHANCED_AO_DEBUG=1` (occlusion and
shadows alone, logs the sun), `JAK_ENHANCED_SUN_FLIP=1` (mirror the sun, for testing). The renderer
logs `[enhanced] average N fps` every 5 s.

Rock lying on the dunes (the desert's flat tfrag rock patches, their rims painted as the sand): tfrag3.frag
reads the hfrag's height grid (Hfrag.cpp hands it over each frame with the dunes' mean time of day light)
and tags rock pixels within ~0.6 m of the dunes, facing up and lit like the dunes as "rock on sand"
(normal tag 0.756-0.82); the composite then grades them as sand, so no pale polygons ring the patches at
golden hour while the dark middles keep the rock grade. Off: `shaders.tune_rock_on_sand 0`; 2 shows the
weight (red rock, green sand), 3 the baked light (R 0.5 = as the dunes) and texture (G), with
`tune_raw_view 1`. Thresholds: `tune_rock_sand_h` (m), `tune_rock_sand_up`, `tune_rock_sand_lit`.
4 shows its three factors (R height, G facing up, B light), 5 the raw measures, 6/7 the same with the
other level draws hidden, 8 the blended draws by class, 9 the sand cover's alpha.

The pale wedges that still ringed those slabs were the game's blended sand layer painted over the rock
(sand in the game's picture): blended draws don't write the normal target, so the pixels kept the rock's
tag and the composite lit them as rock. Blended sand-class tfrag/tie draws (Jak 3) now blend their normal
and tag into the normal target by their coverage (rgb only, the class alpha kept; EnhancedMaterials.h
sets the mask and blend, tfrag3.frag's `enh_blended` 2 writes the coverage): those pixels grade as sand.
Off: `shaders.tune_sand_cover [0, 0]` (default `[0, 1]`: the alpha range from none to all). The composite's
rock-on-sand tag window reaches 0.85 for the blended tags.

Thin dark line along a ledge's edge (desert-start's slab at noon): the rim vertices' normals lean out over
the edge with the wall below; seen almost edge-on from above they crossed the view plane, the composite
(which turns normals toward the camera by their own sign) turned them down and the sun's light left 2-4 px
dark. tfrag3.frag keeps the written normal on its face's seen side. Off: `shaders.tune_normal_side 0`;
`tune_shadow_dbg 9` / `10` show the world normal / the meter-wide one the sun's light uses.

Parallel test games: `JAK_ENHANCED_INSTANCE=<n>` (1-99) moves gk's listener and goalc's nREPL/debug
ports up by 10*n (jak3 instance 2: gk 8134, nREPL 8201) and names the logs `jak3_i<n>.*.log` /
`compiler_i<n>.*.log`; `og_repl.py --instance n` talks to that goalc. `scripts/enhanced/multi_run.ps1
-Count 3 -Game jak3 -Start desert-start -Seconds 60` starts that many small windowed games, each with
its own config dir made from `local\testcfg`, and prints alive/crashed, fps and RAM for each.

Crashes: `scripts/enhanced/crash_collect.ps1 [-Hours 24] [-Out <dir>] [-Watch [-ProcessId <gk pid>]]`
(run through `[scriptblock]::Create`) lists gk crashes from the Application events 1000/1001 (faulting
module+offset; `unknown` = GOAL code, the offset is then the address), matches each to its
`log\jak3[_i<n>].*.log` by the process start time and to `%LOCALAPPDATA%\CrashDumps\gk.exe.<pid>.dmp`,
and prints time, instance, last level lines, `[enhanced]` lines, asserts and a 40-line log tail; `-Out`
also writes a markdown report and a zip (logs, dumps unless `-NoDumps`). `-Watch` waits for the next
crash. Read-only: it never starts or stops a game. gk keeps only ~10 logs per name, so collect soon.

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
ctycara; 8 Hellcat, in the desert only) once its level is in, for testing.

Desert Hellcat: the Hellcat Jak flies in the War Factory's sky battle (task `factory-sky-battle`, type
`h-warf`, vehicle type 8, levels/factory/h-warf.gc and warf-projectile.gc, LFACTORY.DGO) is parked on the open sand by
the Spargus wall (2544.8, 222.4 m, nose toward the canyon, on the ground probed there); Jak gets in
and out with triangle. The menu's "Hellcat in the desert" (City section, Jak 3, saved as `desert_hellcat`,
`pc-enhanced-desert-hellcat?`) turns it off: no deshcat, none parked; one Jak is flying goes once he's out. It's the mission's, handling, guns, thrusters and
sounds: it takes off with the mission's start (a second at 50 m/s) and never stops, flying at 37.5 to
50 m/s (x throttle, square brake); left stick x turns it, banking hard; left stick y climbs (back) and dives
(forward); r1 fires `gun-warf-shot` from the nose, homing on what's ahead (four at once, then one a
second), l1 the `warf-projectile` missile and its blast (8, one back every 20 s); circle lands it: it stops and
comes down level (its own leveling torque in place of the mission's, which pitches it along its path) to rest
hovering on the ground with Jak in it, flying as the empty one meanwhile (none of the mission's thrust, which
never lets it go under 37.5 m/s); x or the stick back takes off again with the mission's start
(`[hellcat] landed by circle at <y> ground <g>` in the log). Its thruster flames are fac-part.gc's 2761/2762
and the missile's trail and muzzle flash warf-projectile.gc's, as GAME copies (parts 5450-5460, group 1690,
logic-target.gc), and drawn so they show: the game launches them as 2D sprites, and those drew nothing here
(day or night; 3D sprites and glows do), so the flame is a 3D sprite facing back along the exhaust
(`*particle-quat*`, birth-func-set-quat), the blue around it and a glow on the missile (the factory's unused
2909, 5461) use the glow flag, and a 3D streak (the gun's beam, 5462) follows the missile; its blast also sets
off the KG robots' explosion (group 219), the hemisplosion sphere's art not being drawn here. Its sounds, `hellcat-engine`,
`-boost`, `-bank`, `-fire`, `-firealt`, damage loops and explosion, all in `factory2` (the engine, boost and bank loops
lost their ids each time the ship went back to waiting, h-warf's vehicle-method-106, and stayed silent: parked first,
ours always did; they get new ones), which player-control
swaps in for `desert2` through the vehicle manager's `extra-bank` while Jak flies (dflut, firetorch,
cactus, waterfall sounds are out meanwhile). What the factory's arena had and the desert hasn't is changed in
deshcat's copy of it only (desert-hellcat.gc; LFACTORY.DGO loads its own): its altitudes are absolute (it
can't fly below 314 m, climbing fades out from 400 m, `ascention-atten!`, minalt and maxalt), here they're the
same band over the ground under it (floor 8 m up, ceiling ~120 m over that); its arena (290 m around the
factory, `*h-warf-center-of-level*`) is gone; Jak can get out (vehicle flag #x400 keeps him in, in the
mission), even in flight, and the empty Hellcat comes down to the ground and stays there (the mission's has no
gravity of its own at low speed and blows up when it stops against something); the missile's blast sphere
gets deshcat as its level (factoryb, in the factory). Its code and art are the `deshcat` level
(`dgos/deshcat.gd`: the hover-vehicle code as HGA.DGO carries it, warf-projectile.o, h-warf.o, the
Hellcat's art and the blast sphere's (`hemisplosion`, FACB's), `ctycarc`'s bsp, so `ctycarc.fr3` draws the
Hellcat; the sphere isn't in it, so only its particles show), loaded into wasall's borrow slot 0 (now 496 KB,
was 405) while no task wants that slot (`enhanced-hellcat-update`, logic-target.gc) and Spargus isn't coming in: once one
of its levels (waswide, wascitya, wascityb, wasleapr) is wanted or in, deshcat isn't asked for, and the
Hellcat goes first (`deshcat-release`: Jak out of it if he's flying, banks back, its shots gone), before
deshcat unloads (asking for it again at the gate, in the city's level swaps, crashed the game). `want-levels`
(load-state.gc) keeps deshcat wanted: the desert's streaming lists dropped it, and now and then the loader discarded
it before it was asked for again, the Hellcat gone from under Jak.
`JAK_ENHANCED_HELLCAT_TEST=1` (test boots): Jak walks up, gets in, climbs by the stick, turns, fires both
guns, lands with circle and takes off, dives down to the floor and gets out in flight by himself, with `[hellcat-test]` log lines for screenshots (the test stick is
`*enhanced-pad-leftx/lefty*`, drawable.gc; pads in other windows are ignored). `=2` and `=3`: into Spargus
from the desert in the Hellcat or on foot (the gate's level swap, then carried to wascitya-start), with
`[spargus-test]` lines.

Ambient sounds (`game/sound/common/enhanced_ambience.*`): ten loops in
`game/assets/enhanced/sounds` (light rain, heavy rain, dripping, night wind, storm wind, crickets,
birds, frogs, waves, a city's hum; 48 kHz stereo WAV) mixed on top of the game's sound in `Player::Tick` (989snd, all three games).
`EnhancedPost::update_scene` sets each one's level every frame: the rain from the weather's rain
and storm, the dripping once the rain stops while the puddles last, the storm wind from the
thunderstorm and the sandstorms, the zone's `"ambience"` in world.json (`crickets` and `wind_night`
at night, `birds` by day, `frogs` mostly at night and in the wet, `wind`, `waves` and `city`
always), a little lower in cutscenes. The audio thread fades them (2.5 s), follows
the game's sfx volume (group 0) and goes quiet while the sfx group is paused (the pause menu). Menu:
"Ambient sounds" (volume, 0 = off). The loops were made with ElevenLabs' sound effects and leveled
by `scripts/enhanced/ambient_sounds.py` (`--convert` redoes the leveling from the downloads; the
last half second of each is faded into its start, so the seam doesn't click).

3D audio and area reverb (`game/sound/common/enhanced_spatial.*`, 989snd): 989snd's pan table only
knows left and right, so a sound behind Jak sounded like one in front. Each sound effect's voice gets a
head shadow from its pan (0 ahead, 90 right, 180 behind): a one-pole low-pass (~2.5 kHz) blended into the
far ear and, for what's behind, into both (`Voice::Run`; music, group 1, untouched). The voices of
everything but the music also go to one stereo reverb (6 combs + 2 allpasses a side, a pre-delay and one
early echo), mixed in `Player::Tick`; its preset comes from the zone (world.json `"reverb"`: desert
`dry`, Haven `street` with a slap off the walls at 70 ms, `under`/`temple`/`metalhead`/`volcano` `cave`
with a 2.6 s tail; no key: dry outdoors, cave indoors; a roof over Jak outdoors: `room`), the presets
easing into each other over ~1.5 s. Menu (Sound and subtitles): "3D audio", "Area reverb";
`JAK_ENHANCED_AUDIO3D=0|1`, `JAK_ENHANCED_REVERB=0|1` win over it. Log: `[audio] reverb <preset> (on|off)
3d on|off sends <voices>` every 20 s and on each change (measured: desert `dry`, ctyport `street` with
7-9 voices sending).

Smooth animation (Jak 3, `process-request!` in joint.gc): the game's logic and skeletons already run
every rendered frame at any frame rate (OpenGOAL), and a fractional frame already blends two keys; what
stepped is the blend itself: straight lines from key to key, so a joint keeps one speed between keys and
changes it at once on each (Jak's idle hand: the same speed for 8 frames, then another; 16 at 120 fps).
Now Catmull-Rom over the key before and after the pair too (the two middle keys first, so the quaternion
flip test lines the outer two up with them); it still passes through every key, and costs nothing when
an animation sits on a whole frame. Measured on the same hand: speeds ramp (`... 4 4 3 2 1 0 1 0 2 ...`)
instead of plateaus with jumps (`6 6 6 7 7 2 2 2 ...`). Menu "Smooth animation" (City section),
`JAK_ENHANCED_SMOOTH_ANIM=0|1` (`pc-enhanced-smooth-anim?`).

Feet on slopes (Jak 3, `enhanced-foot-ik` in logic-target.gc): the game's own leg IK (`leg-ik`,
`target-update-ik`) probes the ground under each foot, puts the foot on it, tilts it to the ground's
normal and blends off when the foot is lifted, but it can only bend the leg: a foot over lower ground
(downhill, the step below) hung in the air. The drawn body (`cspace-offset`, which Ashelin's and Sig's
skins follow too) now drops to the lower foot's ground, at most 35 cm, eased at 1.5 m/s; full standing
and walking, gone by 7 m/s, and off wherever the game turns the leg IK off (air, ledges, vehicles, the
board). It is taken back before the collision step (`pre-collide-setup`, which eases `cspace-offset`)
and forgotten if a state set `cspace-offset` meanwhile, so it never builds up. Measured on Spargus'
stairs (`wascitya-top-of-stairs`, two steps down): lower foot 22.9 cm under Jak's ground point, body
dropped 22.9 cm. Menu "Feet on slopes", `JAK_ENHANCED_FOOT_IK=0|1` (`pc-enhanced-foot-ik?`).

Subtitles: smaller than the PS2's by default (bank scale 0.65 in Jak II and 3, 0.38 in Jak 1, in
`subtitle*-h.gc` / `pc/subtitle.gc`); the menu's "Subtitle size" (Small, Medium, Large = the PS2
size, Extra large) multiplies it (`pc-enhanced-subtitle-size`, percent), and the Jak II/3 subtitle
box grows with it.

Minimap (Jak II and 3): drawn at 4x (512 x 512, `ProgressRenderer::kMinimapScale`, mipmapped),
and the district maps (`map-...` in `...-minimap` pages, 4-bit) redrawn at 4x as they load
(`loader/EnhancedMapTextures.cpp`: each output pixel takes the color covering most of it, from a
blurred mask of each color, so the edges are smooth and the colors stay flat). The game still
addresses both in its own texels: `enhanced_texel_scale` gives the direct renderer the factor
(`tex_px_scale` in the direct shaders).
Smaller by default: the menu's "Minimap size %" (70, `pc-enhanced-minimap-size`) scales the map, its
icons, Jak's arrow and the ring together (`enhanced-minimap-scale` in `minimap.gc`, hud-map's draw
in `hud-classes.gc`), the bottom right corner kept in place.
HD art, loaded instead of the game's when present, a whole multiple of the texture's size:
`out/enhanced/hud/<game>/<tpage>/<name>.png` (made on the player's machine from the games' art, not
in git: the district maps, the HUD and menus 4x) or `game/assets/enhanced/hud/...` (new drawings, in
git: the Tripo icons).
`scripts/enhanced/minimap_icons.py` redraws the 47 icons of `mini-map-icons` with Tripo
(image to image, four sheets of 12 with a description of each; 8x atlas), and
`scripts/enhanced/minimap_maps.py` redraws every district map at 4x in a modern style (the same
shapes; calmer palette, raised blocks with a lit bevel and a shadow on the streets, outlines, deeper
water with a light rim; the painted maps of the desert enlarged with Real-ESRGAN).

Ashelin and Sig (Jak 3, played in Jak's place). Ashelin's pistol holds 20 rounds, then reloads (1.1 s; the
HUD shows `n/20`). Their attacks are their own moves (`enhanced-moves.gc`): Jak's target stays in his
punch and spin-kick states (physics, momentum, hit spheres, sounds), the character's bones are aimed along
a pose table instead of taking Jak's pose. Ashelin, square: a karate front snap kick; with her pistol out:
its grip brought down from over her head. Sig, square: a forward elbow strike
(`JAK_ENHANCED_SIG_BOXER=1`: a jab and a straight); with his Peacemaker out: its butt thrust forward;
circle: a spinning backfist. The tables (`enhanced-move-tables.gc`) are made by
`scripts/enhanced/char_moves.py` from generated motion clips (Higgsfield Kimodo, original motion, kept in
`local/higgsfield/motion`, not in git): key poses on a stylized timeline, as rig-independent directions.
Hit and dying, Ashelin plays her own clips (thrown back and kip-up, dying on the spot). In vehicles and
on ledges the characters' hands and feet are put where Jak's are (two-bone IK).
`scripts/enhanced/chars_review` measures their animations against Jak's in a running test game.

## Known issues

- Water and PBR (metal/glass) reflections and refraction: fewer artifacts than before, but still
  shimmer and smears where the screen-space rays leave the picture, and refraction at the shoreline.
- Extreme population in the war zone fills the navigation meshes: fewer characters appear than
  asked for at the busiest moments (it used to crash, see below).

Fixed: the Extreme population crash in Haven's war zone (`nav-mesh::new-nav-control: too many
users`). Every citizen, even the inactive ones waiting in the traffic pool, holds a user slot of the
static `*default-nav-mesh*` (128) from its init, and the raised population outgrew it: the next one
got no nav and crashed. `citizen-spawn` now stops while that mesh has fewer than 4 free slots
(`enhanced-nav-mesh-room?` in nav-mesh.gc), the traffic engine leaves 8 free on the street meshes, a
citizen refuses `traffic-activate` (from the missions and the war zone too) on a full mesh, and
nothing touches a missing nav after `go-inactive`.

Fixed: black flashes, the whole world black under the sky for one frame. Each time the auto quality
changed the render scale (levels 4 and 5, `FBO Setup` in the log: mostly in the first seconds after a
save is loaded, then now and then) the clouds, their rays and the half-size sun shadows made their
textures again while the composite's scene was bound on the active unit 0, and unbound it: that
frame's composite had no scene, only its own sky, glows and fog. `enh_post::KeepTexture2D` keeps the
unit's texture across such blocks (also the occlusion's and the indirect light's histories, which lost
their depth the same way, and the ocean's buffers), and the scene is bound again right before the
composite's draw. Found with the black-frame probe (`EnhancedFrameProbe`): tuning
`"debug": {"black_probe": 1}` logs `[enhanced] probe f=.. scene=.. post=.. final=.. window=..` rows
(mean brightness / share of black samples of the 3D scene, the post's result, the finished frame and
the window) around every frame that flashes dark (`DARK-FRAME`, `BLACK-FRAME`), every render target
change (`fbo-WxH`) and every frame the game sent late (`no-chain`); 2 also saves those windows as PNGs
in `JAK_ENHANCED_DUMP_DIR`. `"debug": {"auto_level": n}` holds the auto quality level (a test steps
the render scale with it), `"debug": {"drop_chain": n}` leaves every n-th frame undrawn.

### The nap's animations

Jak lies down on his back, sleeps with a big cartoon breath, gets up and stretches: four Mixamo
clips (X Bot, FBX) retargeted onto Jak's skeleton by `scripts/enhanced/nap_anims.py` (Blender
headless; each Jak joint takes its Mixamo joint's rotation from the T-pose, worked out in glTF space
from the decompiler's `jakb-lod0.glb`). The glb holds only the skeleton (no `align`: build-actor
adds it), a one-triangle mesh and the animations; `(build-actor "jak-nap" :master-art-group
"jakb-ag")` makes `jak-nap-ag.go`, which goes in GAME.CGO after Jak's other sub-groups and fills free
slots of `jakb-ag` as it loads. `target-enhanced-nap` finds them by name (`jakb-nap-lie`, `-sleep`,
`-getup`, `-stretch`) and falls back on the trip and painful-land animations without them.
Jak 1 (`eichar-lod0.glb`, ripped with `"rip_levels": true`, `"dgo_names": ["CGO/GAME.CGO"]`; the
glb lands in `decompiler_out/jak1/levels/common` whatever the output folder) gets the same four as
`eichar-nap-...`; Jak 1 links only level art groups into Jak's, so `relocate` keeps `jak-nap` in
`*enhanced-nap-ag*` and target2.gc plays them from there. Its nap window is drawn with sprites and
the pause text (main.gc): the screen is 512 x 224 there, `middle` centers on x, there is no font
scale, and the large font has only capitals and the button pictures.
Jak II (`jakb-lod0.glb` from its GAME.CGO, master `jakb-ag`) links only art groups that start with
an animation, so it keeps `jak-nap` in `*enhanced-nap-ag*` the same way (joint.gc).
A GAME.CGO-only rip also rewrites `out/<game>/fr3/GAME.fr3` without the levels' animated textures
(Jak II then dies on "no texture named security-env-dest"): rip into a copy, or run the full
decompiler afterwards.
Every joint needs all three channels on every frame (build-actor fills missing ones with identity,
not the rest pose); the mesh needs indices, a material and u8 JOINTS_0 or build-actor crashes.

## Releases and updates

`scripts/enhanced/make_release.py` packs the files tracked by git, plus `HANDMADE`: the folders made by
hand with Tripo, out of git and copied from the disk (it stops if one is missing). What is computed from
the game's own files stays out: `texture_replacements`, `scenery/game_*`,
`texture_remade/level-default-pris` (`repaint_jak.py`). It warns about uncommitted changes (it copies the
working tree, not HEAD) and about programs older than their sources, and leaves out of `placed.json` the
objects whose model doesn't ship.

What the player can edit (`SHIPPED`: `placed.json`, `lights.json`, `grass_paint/`, the terrain's pictures)
isn't in its place in the trilogy's release but in `data/shipped`, so nothing that copies a release over a
folder can overwrite it, not even an older release's `update.ps1`. The launcher runs `apply_shipped.ps1`
once per version: a file equal to what was last put there (`shipped/applied.json`) or to any earlier version
(`manifest.json`, from git's history) is replaced, anything else is the player's and stays. In Enhanced
Light these files stay in their place: OpenGOAL's launcher deletes a mod's folder at each version.

An update only compiles the code, and replacement models are read when levels are extracted. So the
launcher runs `refresh_models.ps1` once per version: it compares `custom_assets/<game>/merc_replacements`
with `out/<game>/merc-stamp.json` and, for what changed, extracts again the levels whose list in
`goal_src/<game>/dgos/*.gd` names the model's art group, with the decompiler and a narrowed `inputs_file`
(as `hd_setup.py`, so `texture_replacements` stays baked in). GAME.fr3 is saved and put back, since a
partial extraction rewrites it without the other levels' animated textures; a model of GAME.CGO itself
makes it a whole extraction. No `iso_data`: from the disc image if there is one, else skipped until the
next start, with the old models in the meantime.

`update.ps1` closes what runs from the folder before copying (it asks first; the game's window is asked
to close, then it is forced), and writes `data/VERSION` last, so an update that stops halfway is offered
again. The installer does the same with `-CheckOnly` and `-CloseOnly`.

## Releasing

`scripts/enhanced/release.ps1` does a whole release, run from the tree to release (it never uses
`-ExecutionPolicy Bypass`):

```powershell
$rel = [scriptblock]::Create([IO.File]::ReadAllText("$PWD\scripts\enhanced\release.ps1"))
& $rel -Version v0.14.8                   # dry run: commands printed, safe checks only
& $rel -Version v0.14.8 -Publish          # the release
& $rel -Version v0.14.8 -Publish -Resume  # a stopped run goes on from the first step not done
```

The steps:

1. **clean**: `git status --porcelain` is empty (`make_release.py` copies the working tree, not HEAD), the
   tools are there (Python, gh, Inno Setup), and so are the `HANDMADE` folders.
2. **release**: no GitHub release with this tag yet. A draft left behind is resumed, never doubled; a published
   one stops it.
3. **gate**: `scripts/enhanced/release_gate.ps1`, if the tree has it, runs in its own PowerShell
   (`JAK_RELEASE_VERSION` set). A non-zero exit stops the release. `-SkipGate` skips it.
4. **build**: Release-static (`gk goalc extractor decompiler`, `-Jobs 12`). The first time it configures
   `out/build/Release-static` (Ninja, clang, `STATICALLY_LINK`), which is slow. It wants `BUILD EXIT 0` and
   the 4 programs no older than the last commit to their sources. `-SkipBuild` only checks them.
5. **notes**: `out/release/notes-<version>.md`, UTF-8. When it is missing, a draft is written: the head of the
   newest notes, the download table, empty New/Fixed sections and the commit subjects since the last
   published release, under a `<!-- DRAFT` marker. Rewrite it in plain player language and delete the
   markers. `-Publish` stops while a marker is there, or when the patches the table names aren't the
   ones being made.
6. **full / light**: `make_release.py --installer --patch-from <3 ZIPs>`, then `--light`, with `--out` set
   (the shader check always runs). It stops on a non-zero exit, on a `WARNING:` about uncommitted files
   or old programs (the `placed.json` one is only shown), and on sizes that make no sense. The patches
   come from the 3 newest full ZIPs before this version, by version number, found in the out folder and
   in the main tree's `out/release`, which is only read. `-PatchFrom v0.14.6,v0.14.0` chooses them instead.
7. **push**: `git push enhanced HEAD:main` (fast-forward only), after listing what goes out: it asks you
   to type the commit's first 7 characters, or takes `-ConfirmPush <sha>`. Other agents' WIP on main
   would go out too, so look at the list. The draft targets that exact commit.
8. **draft / upload / publish / light-upload**: a draft, then `metadata.json` and the patches, then Setup,
   then the ZIP. Each file gets up to 15 tries 30 s apart (`gh release upload --clobber`). A file is up
   when its asset is `uploaded` with the local size. Then `--draft=false --latest`, and Light last.
9. **verify**: published, every asset `uploaded` and as big as the local file.
10. **mods / mods-push**: once the Light asset answers 200, a new first entry in `mods.json`
    (`jak-enhanced-light`, json load/dump, the file's own format) and `lastUpdated`. That file is committed
    alone. It is pushed without asking only when it is the one commit going out.

State goes to `local/release/<version>/state.json`, with the commit the packages came from. `-Resume`
skips the steps done there and refuses a HEAD that isn't that commit (or that plus the mods.json commit).
The script stops by throwing, never `exit`, and ends with a summary of each step and failed check, and
`$LASTEXITCODE` 0 or 1. A release from a worktree needs the `HANDMADE` folders in it (they are out of git:
copy them, or make junctions to the main tree's) and `-OutDir` if the packages should go elsewhere.

## Not done (yet)

Next, the small ones first (the big ones below wait until these are done):

- Rain on roofs and metal when Jak is under cover (needs a "covered" test: the sky hidden above
  Jak).
- The big map of the pause menu (Jak 3: `bigmap.gc`, a scrolling image from `WOMAP.STR`
  uploaded 256 x 416 at a time) in the same style as the minimap's district maps; a modern frame for
  the minimap.
- A photo mode: the game held still, a free camera, filters, the HUD hidden.
- Weather by zone and by season: each zone gets its own set of possible weathers that it cycles
  through (the desert's sandstorms and clear nights, Haven's rain, the forest's fog...) instead of one
  pool for every open area; then seasons that shift those sets (wetter, colder, drier) over time.

New art made with Tripo (`scripts/enhanced/minimap_icons.py` is the model: references from the game
only as a guide, the result drawn new, so it goes in git; ask before spending credits):

- Icons: the pause map's icons (Jak 3 `bigmap`, Jak II `map`), Jak 1's HUD counters, Enhanced-menu
  icons (weather, time of day, presets), an app/installer icon (`SetupIconFile`, gk.exe resource).
- Images: the installer's wizard pictures (`WizardImageFile` 164x314, `WizardSmallImageFile`), launcher
  and game-picker covers, a README banner.
- Painted maps: the Jak 3 `WOMAP.STR` world map and Jak II's big map redrawn whole (served through
  `enhanced_hd_screen_texture` or a replacement of the 256x416 windows), the minimap frame/ring.
- Tileable PBR materials: Tripo makes the color image (seamless prompt, then a wrap-pad check), normal,
  height and roughness are derived locally (Sobel on a height from luminance + a roughness curve), into
  `game/assets/enhanced/materials/`: puddle and rain-ripple normals, wet asphalt, snow, sand ripples,
  mud for footprints and tire ruts, close-up detail maps for rock, grass and sand.
- Later, models (Tripo's text/image to 3D with PBR, under a face budget): grass clumps, flowers,
  bushes, city clutter for the vegetation and population, new passers-by. Needs a path for custom
  meshes in jak2/jak3 (custom actors from glb, as Jak 1's custom levels do).

The big ones (after the above):

- The bridge levels and the continuous trilogy (TRILOGIA.md, phase 4); one menu and save, Daxter and
  the trilogy's total time.
- Life in the Jak 2 plaza (below).
- Linux / Steam Deck releases.
- Hardware ray tracing (OpenGL has no access to it: Vulkan interop). The reflections are traced in
  software (above); ray-traced shadows and indirect light could use the same hierarchies.
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
