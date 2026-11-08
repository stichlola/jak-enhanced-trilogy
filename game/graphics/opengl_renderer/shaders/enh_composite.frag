#version 430 core

// Jak Enhanced Trilogy: composite of the 3D scene, drawn back over it before the HUD buckets.
// In order: heat haze, camera motion blur or FXAA, light depth of field, contrast-adaptive
// sharpen, ambient occlusion and sun shadows, distance/height fog, bloom and god rays, highlight
// roll-off, grading and vignette.

uniform sampler2D tex_T0;  // scene
uniform sampler2D tex_T1;  // bloom
uniform sampler2D tex_T2;  // ambient occlusion and sun shadows
uniform sampler2D tex_T3;  // depth (black when no 3D scene this frame)
uniform sampler2D tex_T4;  // god rays
uniform sampler2D tex_T7;  // indirect light (SSGI)
uniform sampler2D tex_T8;  // volumetric mist, half resolution (rgb scattered, a transmittance)
uniform int volume;
uniform float volume_fade;  // (1; down to 0 as the auto quality sets it aside)
uniform float vol_lights_only;  // 1: the volume is only the lamps' light in the air (no mist)
uniform sampler2DShadow tex_T9;  // sun shadow map
uniform float shadow_strength;   // 0 = no shadow map this frame
uniform mat4 light_mvp;          // world (game units) -> shadow map
uniform sampler2DShadow tex_T22;  // the far sun shadow map (2 x 1.2 km: mountains, big rocks)
uniform mat4 light_mvp_far;
uniform float shadow_far_on;
uniform sampler2DShadow tex_T26;  // the middle sun shadow map (2 x 320 m, 2048: houses, rocks around)
uniform mat4 light_mvp_mid;
uniform float shadow_mid_on;
uniform float shadow_mid_span;
uniform mat3 view_to_world;
uniform int light_count;        // dynamic lights (the game's glows), view space
uniform vec4 light_pos[32];     // xyz, radius (game units)
uniform vec4 light_color[32];
uniform float light_shadow;     // how dark what blocks a dynamic light makes it (0 = no shadows)
uniform int wake_count;         // Jak's recent path: world xyz (game units), age (s)
uniform vec4 wake[16];
uniform float wake_strength;
uniform int print_count;        // footprints and ruts: world xyz (game units), heading (rad)
uniform vec4 prints[96];
uniform vec2 print_info[96];    // age (s), kind (0/1 left/right foot, 2 ruts)
uniform int splash_count;       // Jak's last steps and landings: world xyz (game units),
uniform vec4 splash[16];         // w: age (s) + 10 * strength (1 step, 2 landing)
uniform float prints_strength;
uniform int puff_count;         // dust behind a vehicle or the board, spray off the water:
uniform vec4 puff[32];          // world xyz (game units), age (s)
uniform vec4 puff_info[32];     // size (m), kind (0 dust, 1 spray, 2 snow, 3 smoke), seed, life (s)
uniform float relief;           // relief from the textures' shading under the sun (0 = off; 2 = left half)
uniform float materials;        // metal and glossy surfaces: reflections and highlights (0 = off,
                                // < 0: show them, metal red, glossy blue; 2: left half only)
uniform int blast_count;        // explosions: view space + radius (game units), age (s)
uniform vec4 blast_pos[4];
uniform float blast_age[4];
uniform float blast_strength;
uniform int fire_count;         // the nearest fires: view space, flame size (game units)
uniform vec4 fire_pos[4];
uniform float fire_seed[4];
uniform vec3 fire;              // hot air strength, embers strength, time (s)
uniform int mote_count;         // fireflies or seeds drifting around Jak: view space (game units),
uniform vec4 mote[24];          // w: seed
uniform vec3 mote_style;        // kind (0 fireflies, 1 seeds and pollen), strength, time (s)
uniform vec2 vol_texel;
uniform float gi_strength;
uniform int ao_debug;
uniform sampler2D tex_T11;  // the level geometry's world normals (a = 0: none, use depth)

// ray-traced reflections: the loaded levels' bounding volume hierarchies (EnhancedRayTrace.h),
// world space in game units. Nodes: 2 vec4 (min, w: right child or first triangle; max, w: 0 or
// the leaf's triangle count). Triangles: 5 vec4 (v0, w: texture mean color; edge 1, w: time-of-day
// color; edge 2, w: texture layer or ~0; uv0, uv1; uv2). Palettes: 8 RGBA8 per time-of-day color.
layout(std430, binding = 0) readonly buffer RtNodes { vec4 rt_nodes[]; };
layout(std430, binding = 1) readonly buffer RtTris { vec4 rt_tris[]; };
layout(std430, binding = 2) readonly buffer RtPalettes { uint rt_pal[]; };
uniform int rt_mode;          // 0 off, 1 where the screen-space reflection misses, 2 traced only,
                              // 3 debug: the traced color alone on the water (magenta: missed)
uniform int rt_levels;
uniform ivec4 rt_base[16];    // per level: node, triangle, palette, texture layer base (-1: none)
uniform sampler2DArray tex_T12;  // the levels' textures, small
uniform vec4 rt_tod[8];       // the time-of-day palettes' weights (per channel)
uniform int normal_debug;
uniform vec4 proj;  // a, b, fx, fy (see enh_ssao.frag)
uniform vec2 texel;

uniform int fxaa;
uniform vec3 dof;  // start, end (game units), max radius (pixels)
uniform float sharpen;
uniform float bloom_strength;
uniform float shafts_strength;
uniform float saturation;
uniform float contrast;
uniform float warmth;
uniform float sun_haze;         // the area's warm haze lit by the sun over the distance (0..1)
uniform float golden;           // the late afternoon's amber look (0..1, the area's times the hour)
uniform float dusk;             // the sun going down by the clock (0..1: 16:30 to 18:30, out by 20:48)
uniform float enclosed;         // under a roof (world.json "enclosed", indoor zones): 1
uniform float noon;             // the hours around noon (0..1)
uniform float morning;          // the desert's morning (0..1): clear blue sky, peach toward the low sun
uniform float sky_morning;      // the area's morning sky (0..1): pink and peach toward the low sun
uniform float sky_day;          // the area's day sky (0..1, dawn to sunset): clear blue, its clouds lit to match
uniform float rock_sun;         // the sun's own direction lighting rock and cliffs (0..1, the area's)
uniform float wall_sun;         // and, milder, the walls and plain scenery (0..1, the area's)
uniform float day_look;         // the desert's day (0..1, morning to late afternoon): the morning's colors
uniform float haze_shadow;      // 1: the god rays texture's alpha has the haze's shadow
uniform float reflect_amount;   // how much metal and gloss reflect (times; world.json's tuning)
uniform float reflect_rough;    // and how rough they are (1 = as made below)
uniform float rock_detail;      // the area's rock picture over rock and cliffs (0 = off)
uniform float rock_detail_avg;  // that picture's average brightness
uniform sampler2D tex_T10;      // the rock picture (tiling, mipmapped)
uniform float ground_detail;    // 1: a picture of rocky ground (stone, gravel) for the rocky ground
uniform vec3 ground_detail_avg; // its average color
uniform sampler2D tex_T16;      // that picture (tiling, mipmapped)
uniform float sand;             // the area's olive and dull greens pushed toward golden sand (0..1)
uniform float sand_set;         // 1: the sand's pictures (fine rippled, coarse gravel, cracked crust)
uniform vec3 sand_set_avg[3];   // their average colors
uniform sampler2D tex_T17;      // fine sand (tiling, mipmapped)
uniform sampler2D tex_T18;      // coarse sand with gravel
uniform sampler2D tex_T19;      // a cracked crust
uniform vec4 grass_ground;      // a dense grass field drawn: its middle x, z, radius (m), density
uniform float grass_water;      // and its water's height (m): no grass, no darkening, below
uniform sampler2D tex_T21;      // and how lush it is (g), lying at grass_field: corner x, z, side (m)
uniform vec3 grass_field;
uniform float vignette;

uniform vec3 up_view;   // world up in view space
uniform vec4 fog;       // density (1/game unit), height falloff (1/game unit), max amount, -
uniform vec3 fog_color;
uniform vec4 haze;      // strength (uv), start, end (game units), time (s)
uniform mat3 to_prev;   // view -> previous frame's view
uniform vec3 to_prev_offset;
uniform vec4 motion;    // previous fx, fy, strength (0 = off), max length (uv)
uniform vec2 motion_keep;  // sharp until, full blur from (game units)

uniform sampler2D tex_T5;  // depth with the near ocean's surface (black when none)
uniform sampler2D tex_T6;  // depth right after the mid/far ocean (black when none)
uniform sampler2D tex_T14; // depth right before the near ocean (black when none)
uniform float water_strength;
uniform vec3 right_view;  // world axes in view space
uniform vec3 fwd_view;
uniform vec3 cam_pos;     // world
uniform vec4 sun_dir;     // toward the sun in view space, w: glint strength (0 = none)
uniform vec3 water_deep_color;
uniform vec3 water_shallow_color;
uniform float water_unify;
uniform float water_swell_strength;  // open-sea swell (0 = ripples only)
uniform float water_refraction;      // how far shallow water bends what's under it (uv per slope)
uniform float water_caustics;        // caustics on the shallow sea bed
uniform int pond;                    // the water here is a murky pond (Jak 3's desert oases)
uniform float relight;      // sun relight strength (0 = off)  // how much of our water color replaces the game's

uniform vec4 color_mult;
uniform vec4 color_add;
uniform vec3 grade_lift;        // the area's grade: added in the shadows
uniform vec3 grade_gain;        // and multiplying the highlights
uniform sampler2D tex_T13;      // the sun shadow map as plain depths (soft shadows' blocker search)
uniform vec4 shadow_params;     // depth span (m), map side (m), penumbra per meter, soft (1) or not
uniform vec4 sky_sun;           // toward the sun (or moon) in view space, day or night; w: its height
uniform vec4 clouds;            // amount (0 = off), sky cover, time (s), night (0..1)
uniform int cloud_pass;         // 1: this draw only marches the clouds, a pixel for every 2x2
uniform sampler2D tex_T15;      // the clouds at a quarter of the pixels (last frame's in the pass)
uniform vec4 cloud_info;        // the clouds texture is ready, history to blend in, how much of it to keep
uniform float tune_spill_sat = 0.45;  // the light spill's color at most this saturated (1: off)
uniform float tune_dust_chr = 0.7;    // less sandstorm dust on characters close by (1: none)
uniform float tune_spill_chr = 0.4;   // the light spill on characters (times; 1: as on the rest)
uniform float tune_water_hour = 1.0;  // the water's body dims and tints with a low sun (0: fixed)
uniform float light_spill;      // at night the bright pixels light the surfaces near them
uniform float lens_flare;       // the sun in the lens (0 = off)
uniform float cloud_shadows;    // the clouds shade the ground (0 = off)
uniform vec2 night_dark;        // dark nights: how dark (0..1), the lights' gain
uniform vec4 player_view;       // Jak (view space, game units), w: valid
uniform int spot_count;         // vehicles' headlights: view space + strength, the way they point
uniform vec4 spot_pos[8];
uniform vec4 spot_dir[8];
uniform vec4 lantern;           // Jak's lantern in the desert at night: view space, strength
uniform vec4 town_glow;         // a town lighting the night sky over it: world x, z, strength
uniform int beacon_count;       // far glows drawn as points of light: view space + size
uniform vec4 beacon_pos[64];
uniform vec4 beacon_color[64];  // rgb, strength
uniform float city_glow;        // a lit city at night (0..1): its glow on the low sky and the haze
uniform float pale_ground;      // 1 in Haven's slums (pale flagstones): the night's pale-ground press
uniform vec4 weather;           // rain, dust (0..1), time (s), wet ground (0..1)
uniform float lens_drops;       // raindrops on the lens in a downpour (0 = off)
uniform float chroma;           // colors split at the screen's edges: a hit, Dark Jak (0..1)
uniform float dawn_fog;         // the morning's low fog
uniform float cloud_rays;       // the low sun's rays through the gaps in the clouds
uniform vec4 jak_fx;            // Jak in view space; w: how wet he is (0..1)
uniform float eye_adapt;        // the eye's adaptation: the picture lifted in the dark, lowered in glare
uniform vec4 brightness;        // the player's: exposure - 1 (by the hour), shadow lift, night fill, 0
// the light effects (Jak 3: sprites, glows, generic shots, trails): the scene right before the
// sprite buckets (T23) and right after them (T24). What they added is kept out of the day's grade
// (the desert's stop-down, the shoulders, the eye's adaptation squeezed it into the bright ground)
// and added back after it with a gain. fx_day: 0 at night (the night's own look, unchanged)
uniform sampler2D tex_T23;
uniform sampler2D tex_T24;
uniform float fx_day;
uniform float tune_fx_gain = 2.2;    // the effects' light by day, over the scene graded without them
uniform float tune_fx_bright = 0.8;  // more over bright ground (by the scene's brightness under them)
uniform float tune_fx_sat = 1.0;  // their color (>1: more saturated)
uniform float tune_gold_fill = 0.75;  // late afternoon: sand/rock never under this share of the game's color (0: off)
uniform float tune_gold_shadow = 1.0; // ...but not inside a cast shadow on ground facing the sun (0: everywhere, old)
uniform float tune_fx_dark = 0.2;    // the gain's share over dark ground (1: as over lit ground)
uniform float tune_fx_dark0 = 0.04;  // the scene's luma under the effect: dark up to here...
uniform float tune_fx_dark1 = 0.22;  // ...lit from here
uniform float tune_fx_knee = 0.78;   // the shoulder their sum rolls off from, hue kept
uniform float tune_fx_min = 0.02;    // what counts as an effect: the faint changes are smoke, dither
uniform float tune_fx_area_frac = 6.0;  // the surroundings an effect must stand out from: 1/x screen
uniform float tune_fx_area0 = 0.3;  // the mean around / the pixel: up to here a compact effect,
uniform float tune_fx_area1 = 0.7;  // from here a wash of light (stays in the grade)
uniform float tune_fx_wash0 = 0.03;  // that mean itself (and over 2^x wider): from here on a wash too
uniform float tune_fx_wash1 = 0.08;
uniform float tune_fx_wash_lod = 1.5;
uniform float shadow_far_span = 6000.0;  // the far sun map's depth span (m)
uniform float tune_shadow_far_bias_m = 0.45;  // its depth bias (m along the light)
uniform float tune_night_horizon = 0.05;  // the night sky's glow along the horizon
uniform float tune_sky_keep = 1.0; // the game's lines drawn over its sky (windows) kept over ours
uniform float tune_sky_keep_lo = 0.05;  // by how much and how sharply the levels changed it: from here
uniform float tune_sky_keep_hi = 0.14;  // to here, all of the game's pixel
uniform float tune_sky_enclosed = 1.0;  // under a roof the sky through the windows is the game's (0: ours)
uniform float tune_adapt_hi = 1.0;  // the eye's adaptation lifting a dark room: not its bright parts (0: off)
uniform float tune_adapt_hi_lo = 0.5;  // from this brightness (max channel) to 0.9
uniform float tune_sky_enclosed_keep = 1.0;  // under a roof the windows' sky as the game drew it, at the end (0: off)
uniform float tune_sky_enclosed_gain = 1.1;
uniform float tune_smoke_floor = 1.0;  // under a roof: blended smoke's added light back, up to the game's colour (0: off)
uniform float tune_pane_keep = 1.0;  // under a roof the blended window panes (over-long normal) as the game drew them
uniform float tune_lawn = 1.0;             // lawns' macro variation and moss at their occluded feet (0 = off)
uniform float tune_lawn_var = 0.12;        // its light and dark
uniform float tune_lawn_ao = 1.2;          // the moss-dark dirt where the occlusion is
uniform float tune_night_win = 1.0;       // lit window panes at night: their light (0 = off)
uniform float tune_night_win_share = 0.4;  // how many of the panes' cells are lit
uniform float tune_night_win_cell = 2.5;   // a lit cell's size, m (its height 1.25x)
uniform float tune_sky_keep_night = 1.0;  // the same by night: lights drawn over the game's sky (beacons, beams)
uniform float tune_sky_night_lo = 0.01;   // by how much brighter than the sky: from here
uniform float tune_sky_night_hi = 0.2;    // to here, all of the game's pixel (0.04: the port's mist orb a lilac diamond, its halo cut)
uniform sampler2D tex_T25;  // the game's sky before the levels (capture_sky)
uniform float sky_copy;     // 0: none, 1: compare the scene with it, 2: the scene before the sprites
uniform float tune_shadow_dbg = 0.0; // test: 1 no sun shadows, 2 shadow (r) sunlight (g) tag (b), 5 no middle map
uniform float tune_chr_shade = 0.85;  // characters in a shadow: how much of the sun they lose
uniform float tune_chr_gap_m = 1.6;   // ...from casters at least this far toward the sun (0: old 70 cm lookup)
uniform float tune_chr_tint = 1.0;    // their shade's tint: 1 neutral-cool, 0 the land's (desert lilac)
uniform float tune_chr_bounce = 0.5;  // shade taken off them under a high sun (light back from the ground)
uniform float tune_chr_dusk = 0.3;    // extra darkening in shade as the sun gets low (on tune_chr_dark)
uniform float tune_rock_sat = 0.42;   // desert rock by day: saturation cap (0: off)
uniform float tune_lamp_jak_m = 2.5;  // lamps' shadows let go within this far of Jak's feet (m; 0: off)
uniform float tune_chr_pool = 0.12;   // bloom luma beside a character from which, with a low sun, it keeps the game's light in shade (0: off)
uniform float tune_chr_dusk_desat = 0.2;  // and color taken off them then
uniform float tune_chr_dusk_warm = 1.0;    // that shade toward brown (0: cool grey-blue)
uniform float tune_shoulder_knee = 0.85;  // the whole picture's shoulder (>= 1: off, a hard clip)
uniform float tune_shoulder_white = 1.0;  // over 1 by this much: toward white (60%)
uniform float tune_chr_rim = 0.6;     // characters' moon/sky rim at night (0: off)
uniform float tune_shaft_keep = 1.0;  // the game's translucent light columns (arena shaft) kept as drawn (0: off)
uniform float tune_shaft_lo = 0.08;  // the columns kept from this luma of the game's picture on walls (floors: 0.3)
uniform float tune_shaft_n0 = 1.04;
uniform float tune_shaft_n1 = 1.3;
uniform float tune_shaft_spill = 0.8;  // the column's light on the wall beside it (0: off)
uniform float tune_shaft_spill_r = 3.0;
uniform float tune_spec_occ = 1.0;  // reflections taken away by the AO (specular occlusion; 0: off)
uniform float tune_ao_view = 0.0;   // test: 1 = the SSAO debug view (as JAK_ENHANCED_AO_DEBUG=1)
uniform float tune_chr_dark = 0.5;   // and how much darker they get after the grade, in full shade
uniform float fx_mark;  // 1: tex_T24's alpha is 1 - the blended dust's coverage (capture_fx)
uniform float tune_fx_dust0 = 0.03;   // that coverage from here...
uniform float tune_fx_dust1 = 0.25;   // ...to here: not a light effect (dust, smoke)
uniform float tune_beacon_soft = 1.0;   // far lamps' columns on water: hidden behind walls, rolled off (0: old)
uniform float tune_beacon_water = 0.25;  // and how bright they get at most
uniform float tune_shadow_mid_bias_m = 0.35;  // the middle map's depth bias along the light, meters
uniform float tune_ao_multibounce = 1.0;  // the occlusion per channel by the surface's color (0: grey multiply)
uniform float tune_ao_mb_luma = 1.0;      // ...its color only, at the grey multiply's brightness (0: the fit's own, brighter)
uniform float tune_fx_sky_edge = 0.02;  // the sky glows' faintest light let go (no square at the quad's edge)
uniform float tune_fx_sky_core = 1.6;   // their clipped-white core: at least this bright over our sky (0: off)
uniform float tune_fx_sky = 1.0;  // 1: the sky's glows (sun, moon) kept out of the effects' gain
uniform float tune_spec_aa = 0.25;  // highlights widened by the normal's spread in the pixel (specular AA; 0: off)
uniform float tune_spec_aa_max = 0.18;  // at most this much roughness^2 added
uniform float tune_spec_clamp = 4.0;  // a lamp's highlight never past this many times the lit surface (0: off)
// (light tiles, lights.tiles: per 32x18 screen tile a mask of the lights that may reach it, in a small
// integer texture: as 144 uniform vectors they pushed the composite past NVIDIA's 1024 constant
// registers ("C6020"), and the earlier "0.00 ms gained" was read off the composite's own timer, which
// stops before its fragment work: the tail-span after it holds that)
#define ENH_LIGHT_TILES
#ifdef ENH_LIGHT_TILES
uniform highp usampler2D tex_T28;  // 32x18, R32UI: bit i = light i may reach the tile
uniform float light_tiles_on;      // 1: tex_T28 is filled (lights.tiles)
#endif
uniform float tune_flash = 2.5;  // explosions' and shots' flash light (light_color.a 2), past the lamps' ceiling (0: as a lamp)
uniform float tune_dry_glints = 0.5;   // the lights' highlights on dry polished floors and metal (0: off)
uniform float tune_glint_stone = 0.15;  // ...and how much plain stone gets of them
uniform float tune_low_sheen = 0.5;     // level stone's grazing sheen looking into a low sun (0: off)
uniform float tune_chr_contact = 0.55;   // Jak's contact shadow under his feet (0: off)
uniform sampler2D tex_T29;  // the characters' lowest points around Jak, from below (do_chr_occlusion)
uniform mat4 chr_occ_mvp;   // world -> that map
uniform float chr_occ_on;   // its height span (m); 0: none this frame
uniform float tune_chr_occ = 0.7;     // characters' ground occlusion: darkening right under them (0: off)
uniform float tune_chr_occ_r = 0.45;   // ...its soft reach around their silhouette (m)
uniform float tune_chr_occ_h = 1.2;    // ...and the height gap over which it fades (m)
uniform float tune_water_dbg = 0.0;  // test: 1 no lamp columns, 2 no beacons, 3 no reflection, 4 water mask
uniform float tune_night_keep_raw = 1.0;  // the night spares what's bright in the game's own picture (0: after AO/shadows)
uniform float tune_night_raw_lo = 0.6;    // ...from this luma of it (0.4: as the composite's own)
uniform float tune_night_pale = 1.0;       // (Haven's slums only: pale_ground) night: pale up-facing ground pressed toward the walls (0: off, 2: right half)
uniform float tune_night_pale_knee = 0.09; // ...above this luma (of the final picture)
uniform float tune_night_pale_slope = 0.35; // ...what is left of the luma over the knee
uniform float tune_night_pale_cap = 0.45;   // ...never under this x its luma (-55% at most)
uniform float tune_night_pale_grain = 1.0;  // ...the game's own grain put back on top (planks' gaps; 0: off)
uniform sampler2D tex_T27;  // the level's albedo (lights.albedo; a: baked light / 2, 0 = none)
uniform float albedo_on;
uniform float tune_albedo_dbg = 0.0;   // (test: 1 the albedo target, 2 its baked light, right half)
uniform float tune_albedo_gain = 0.5;  // the albedo as the lamps' surface color: times this
uniform float tune_albedo_mix = 1.0;   // ...instead of the lit color (0: the lit color, as before)
uniform float tune_far_dbg = 0.0;  // (debug: 1-5 skip one far haze/relight term)
uniform float tune_far_lift = 1.2;  // (not at golden hour: it took the glow off the far range) the desert haze lifts far ground at most this x its own luma (0: off)
uniform float tune_haze_layer = 1.0;  // the desert haze as layers over the ground (0 off, 2 right half only)
uniform float tune_dither = 1.0;      // dither the output (in 8-bit steps; 0: off)
uniform float tune_chr_floor = 0.5;   // characters at a low sun: never darker than this x the game's color (0: off)
uniform float tune_chr_floor_any = 0.5;  // that floor's weight with a high sun too (0: low sun only)
uniform float tune_shade_hue = 0.9;  // near sun shade on colored ground keeps its hue (0: the full tint)
uniform float tune_lantern_halo = 0.07;  // Jak's desert lantern: its wide glow (was 0.2)
uniform float tune_fx_debug = 0.0;// 1: what the effects added (x4), 2: the effects' weight, 3/5/6: T24/T0/T23, 9: flame_keep/weight/fx_day
uniform float tune_fx_ab = 0.0;
uniform float tune_warm_shadow = 0.0;  // a warm grade's cool push in the shadows (1: the full split, dusk navy)
uniform float tune_dusk_shade = 1.0;  // the shade's blue tint let go as the sun nears the horizon (not the desert; 0: off)
uniform float tune_cloud_in_shade = 0.3;  // cloud shadows inside the sun's shadow (1: as on sunlit ground, old)
uniform float tune_dusk_floor = 0.7;  // the low sun's shade: never under this share of the game's color (0: off)
uniform float tune_dusk_floor_end = 1.0;  // that floor after the grade, in the game's hue (0: before the grade, old)
uniform float tune_dusk_floor_soft = 0.3;  // how far under the floor before the game's hue takes over fully
uniform float tune_fog_floor_m = 2.0;  // the low fog no denser than this far under the camera (0: off)
uniform float tune_fog_max = 1.0;  // and never more than this (1: no cap)
uniform float tune_night_hue = 0.8;  // night: the dim land's hue toward the game's moonlit one (0: off)
uniform float tune_night_hue_lo = 0.3;
uniform float tune_night_hue_hi = 0.6;
uniform float tune_night_hue_chr = 0.5;  // the characters too, at this share (G3: Jak pink and pasted on the moonlit desert; 0: off)
uniform float tune_chr_gold_rim = 0.7;  // characters at golden hour: a rim of the low sun on their sun-side edges, backlit most (0: off)
uniform float tune_rock_dawn = 1.0;  // and in the morning's low sun (0: off)
uniform float tune_morning_sat_cap = 1.1;  // the morning: the land's saturation at most this times the game's (0: off)
uniform float tune_rock_night = 1.0;  // the rocky ground's warm weathering tint let go at night (0: off)
uniform float tune_night_lamp_cool = 1.0; // night: a lamp pool's faint part toward the moon's hue (0: off)
uniform float tune_night_lamp_lo = 0.15;
uniform float tune_night_lamp_hi = 0.9;
uniform float puddle_level;     // the puddles' water (0..1): outlasts the wet ground, shrinks to the dips
uniform vec4 weather2;          // thunderstorm, ground fog, the sun out after the rain, lightning flash
uniform vec4 dust_wind;         // the wind (world xz, unit), how far it carried the dust (m)
uniform float sheet;            // sheet lightning: the whole sky lit from inside the clouds (0..1)
uniform vec2 snow_heat;          // snowfall, hot air over lava (0..1)
uniform float snow_cover;        // snow lying on the ground (0..1)
uniform vec4 strike;            // a bolt to the ground: its foot's elevation (sine), the cloud base's, its distance (game units), on
uniform vec4 sky_kind;          // the evening's sky: stars shown (0..1), the moon's lit part, red eclipse, red sunset now
uniform float sunny;            // a bright summer day (Jak 1's Sandover): deep blue sky, white clouds
uniform vec4 bolt;              // lightning bolt: azimuth (rad), seed, age (s), strength
uniform float close_up;          // cutscenes: nearer than this (game units) no sun shadow map (0 = off)
uniform vec3 cine_dof;          // cutscenes: focus (view z, game units), aperture, max blur (px; 0 off)
uniform float blast_sparks;     // explosions' sparks and hot air
uniform float blast_seed[4];
uniform float blast_ground[4];  // the ground under it, along up from the blast (<= 0; > 0: not known)
uniform float fx_detail;        // richer explosions, shots and marks (0 = off)
uniform int decal_count;        // lasting marks: world xyz, radius (game units)
uniform vec4 decal[28];
uniform vec2 decal_info[28];    // age (s), kind (0 an explosion's scorch, 1..4 a shot's hit: red, blue, yellow, dark)
uniform vec4 blast_dir[4];      // gun blasts: where it's fired (view space), w: kind (0 explosion,
                                // 1 red gun, 2 red gun charging: only sparks on the gun, 3 a
                                // charged shot: wide, red going fuchsia at its edges)

in vec2 tex_coord;
flat in float flare_vis;     // (enh_composite.vert: once a frame)
flat in vec3 white_balance;
out vec4 color;
float pale_w = 0.0;  // the night's pale ground: how much it's pressed down at the end

// x squared (pow() of a negative base is undefined in GLSL: NaN on some drivers)
float sq(float x) {
  return x * x;
}

float luma(vec3 c) {
  return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// gradient noise, for water that doesn't repeat
// (without sine: plain arithmetic, cheaper, and gnoise calls it four times; Dave Hoskins' hash)
vec2 hash2(vec2 q) {
  vec3 p3 = fract(q.xyx * vec3(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yzx + 33.33);
  return -1.0 + 2.0 * fract((p3.xx + p3.yz) * p3.zy);
}

float gnoise(vec2 x) {
  vec2 i = floor(x);
  vec2 f = fract(x);
  vec2 u = f * f * (3.0 - 2.0 * f);
  return mix(mix(dot(hash2(i), f), dot(hash2(i + vec2(1, 0)), f - vec2(1, 0)), u.x),
             mix(dot(hash2(i + vec2(0, 1)), f - vec2(0, 1)), dot(hash2(i + vec2(1, 1)), f - vec2(1, 1)), u.x),
             u.y);
}

// water height (m) at a world point (m): four octaves moving in different directions, each
// faded out when its waves get smaller than a pixel (foot: meters per pixel) so nothing aliases
float water_height(vec2 w, float t, float foot) {
  float h = 0.0;
  mat2 turn = mat2(0.8, -0.6, 0.6, 0.8);
  vec2 q = w;
  for (int i = 0; i < 4; i++) {
    float freq = i == 0 ? 0.12 : i == 1 ? 0.35 : i == 2 ? 0.95 : 2.6;
    float amp = i == 0 ? 0.045 : i == 1 ? 0.016 : i == 2 ? 0.0055 : 0.0018;
    vec2 drift = i == 0 ? vec2(0.35, 0.2) : i == 1 ? vec2(-0.25, 0.4) : i == 2 ? vec2(0.5, -0.3) : vec2(-0.6, -0.45);
    float fade = 1.0 - smoothstep(0.3, 1.1, foot * freq);
    h += amp * fade * gnoise(q * freq + drift * t * freq * 1.1 + float(i) * 17.3);
    q = turn * q;
  }
  return h;
}

// open-sea swell (m): long waves crossing at an angle, only in deep water (calm in the shallows)
float water_swell(vec2 w, float t) {
  return 0.22 * sin(dot(w, vec2(0.13, 0.07)) + t * 0.8) +
         0.12 * sin(dot(w, vec2(-0.09, 0.17)) + t * 1.1) +
         0.06 * sin(dot(w, vec2(0.27, -0.21)) + t * 1.7);
}

// a rough sea's wind waves (about -1..1): three short trains fanned around the wind, each in
// drifting patches (groups of waves), their crests bent by noise, so nothing lines up in stripes
float wind_sea(vec2 w, vec2 wd, float t) {
  vec2 side = vec2(-wd.y, wd.x);
  float h = 0.0;
  for (int i = 0; i < 3; i++) {
    float ang = i == 0 ? 0.0 : i == 1 ? 0.45 : -0.6;
    float k = i == 0 ? 1.9 : i == 1 ? 2.7 : 3.6;
    vec2 d = wd * cos(ang) + side * sin(ang);
    float group = smoothstep(-0.3, 0.6, gnoise(w * 0.07 + d * t * 0.05 + float(i) * 7.1));
    float bend = 2.0 * gnoise(w * 0.18 + float(i) * 3.3);
    h += group * sin(dot(w, d) * k - t * sqrt(9.8 * k) + bend) / (1.0 + float(i) * 0.6);
  }
  return h * 0.7;
}

// a storm's big waves (about -1..1, the crests sharp, the troughs broad): two long trains near
// the wind's direction, their crests bent and broken by noise
float storm_sea(vec2 w, vec2 wd, float t) {
  vec2 side = vec2(-wd.y, wd.x);
  float h = 0.0;
  for (int i = 0; i < 2; i++) {
    float ang = i == 0 ? 0.12 : -0.35;
    float k = i == 0 ? 0.32 : 0.5;  // 20 m and 12 m waves
    vec2 d = wd * cos(ang) + side * sin(ang);
    float ph = dot(w, d) * k - t * sqrt(9.8 * k) + 2.5 * gnoise(w * 0.025 + float(i) * 4.1);
    float s = 0.5 + 0.5 * sin(ph);
    h += (pow(s, 2.5) * 2.0 - 0.6) * (i == 0 ? 0.65 : 0.35) *
         (0.6 + 0.4 * gnoise(w * 0.03 + d * t * 0.04 + float(i) * 9.0));
  }
  return h;
}

// caustics: the bright net that waves focus on a shallow sea bed (0..1), two drifting layers
float caustics(vec2 w, float t) {
  float a = 1.0 - abs(gnoise(w * 1.3 + vec2(t * 0.30, t * 0.17)));
  float b = 1.0 - abs(gnoise(w * 1.7 * mat2(0.8, -0.6, 0.6, 0.8) - vec2(t * 0.21, -t * 0.26)));
  return pow(a, 6.0) * pow(b, 6.0) * 4.0 + pow(min(a, b), 10.0);
}

float view_z(float d) {
  return d > 1e-7 ? abs(proj.y / (d * 2.0 - 1.0 - proj.x)) : 0.0;
}

// screen-space shadow toward a dynamic light: a few steps through the depth buffer, up to 3 m
// (and short of the light itself, which often sits inside its lamp)
// (reach: how far toward the light the ray looks, game units; steps: how many samples)
float light_blocked_by(vec3 from, vec3 lp, float jitter, float reach, int steps) {
  vec3 dir = lp - from;
  float len = length(dir);
  vec3 stp = dir / len * (reach / float(steps));
  for (int s = 0; s < steps; s++) {
    vec3 q = from + stp * (float(s) + jitter);
    if (q.z < 0.3 * 4096.0) {
      break;
    }
    vec2 quv = vec2(q.x * proj.z / q.z, q.y * proj.w / q.z) * 0.5 + 0.5;
    if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
      break;
    }
    float sd = texture(tex_T3, quv).r;
    if (sd > 1e-7) {
      float sz = view_z(sd);
      // something in front of the ray (but not a whole building's depth: then it's beside it)
      float gap = q.z - sz;
      if (gap > 0.04 * 4096.0 + q.z * 0.004 && gap < 1.5 * 4096.0) {
        return 1.0;
      }
    }
  }
  return 0.0;
}

float light_blocked(vec3 from, vec3 lp, float jitter) {
  return light_blocked_by(from, lp, jitter, min(length(lp - from) * 0.85, 3.0 * 4096.0), 8);
}

// a contact shadow toward the light: only what's close in depth to the ray counts (a thin thing
// standing in front of it isn't its shadow), softer the farther along the ray it's met
float pixel_angle();

float contact_shadow(vec3 from, vec3 dir) {
  const int STEPS = 10;
  const float reach = 0.5 * 4096.0;
  // (steps offset per pixel and frame, as light_blocked's: fixed ones left 5 cm stair bands at the
  // feet that the TAA couldn't average away)
  float jit = fract(pixel_angle() * 0.1591549);
  for (int s = 0; s < STEPS; s++) {
    float t = (float(s) + jit) / float(STEPS);
    vec3 q = from + dir * reach * t;
    if (q.z < 0.3 * 4096.0) {
      break;
    }
    vec2 quv = vec2(q.x * proj.z / q.z, q.y * proj.w / q.z) * 0.5 + 0.5;
    if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
      break;
    }
    float sd = texture(tex_T3, quv).r;
    if (sd > 1e-7) {
      float gap = q.z - view_z(sd);
      if (gap > 0.02 * 4096.0 + q.z * 0.002 && gap < 0.25 * 4096.0) {
        return 1.0 - t * t;
      }
    }
  }
  return 0.0;
}

vec3 view_pos(vec2 uv, float d) {
  float z = proj.y / (d * 2.0 - 1.0 - proj.x);
  vec2 ndc = uv * 2.0 - 1.0;
  return vec3(ndc.x * z / proj.z, ndc.y * z / proj.w, z);
}

// the normal target, "none" where it holds no normal: a draw that blended into it (the arena's
// light shaft) left a vector longer than any material's tag (1.0): AO went black under it
vec4 gbuf(vec2 uv) {
  vec4 g = texture(tex_T11, uv);
  return dot(g.rgb * 2.0 - 1.0, g.rgb * 2.0 - 1.0) > 1.04 ? vec4(0.0) : g;
}

// how much of the sun the shadow map says is blocked at a world point (game units), 0..1

// the surface normal in view space, toward the camera: the level's own normal where it wrote one,
// else rebuilt from the depth buffer (the neighbor on each axis with the smaller depth jump)
vec3 surface_normal(vec2 uv, vec3 p) {
  vec3 nn;
  vec4 g = gbuf(uv);
  if (g.a > 0.17) {
    nn = normalize((g.rgb * 2.0 - 1.0) * view_to_world);  // world -> view (transpose)
  } else {
    vec3 pr = view_pos(uv + vec2(texel.x, 0), texture(tex_T3, uv + vec2(texel.x, 0)).r);
    vec3 pl = view_pos(uv - vec2(texel.x, 0), texture(tex_T3, uv - vec2(texel.x, 0)).r);
    vec3 pu = view_pos(uv + vec2(0, texel.y), texture(tex_T3, uv + vec2(0, texel.y)).r);
    vec3 pd = view_pos(uv - vec2(0, texel.y), texture(tex_T3, uv - vec2(0, texel.y)).r);
    vec3 dx = abs(pr.z - p.z) < abs(p.z - pl.z) ? pr - p : p - pl;
    vec3 dy = abs(pu.z - p.z) < abs(p.z - pd.z) ? pu - p : p - pd;
    nn = normalize(cross(dx, dy));
  }
  return dot(nn, -p) < 0.0 ? -nn : nn;
}

// the nearest level triangle along o + d * t (world, game units, d unit length), up to tmax:
// its color in the game's own baked light at this hour, and how far it is
bool rt_trace(vec3 o, vec3 d, float tmax, out vec3 color, out float dist) {
  vec3 dd = vec3(abs(d.x) < 1e-7 ? 1e-7 : d.x, abs(d.y) < 1e-7 ? 1e-7 : d.y,
                 abs(d.z) < 1e-7 ? 1e-7 : d.z);
  vec3 inv = 1.0 / dd;
  float best = tmax;
  int best_tri = -1;
  int best_level = 0;
  vec2 best_uv = vec2(0.0);
  int stack[32];
  for (int l = 0; l < rt_levels; l++) {
    int nb = rt_base[l].x;
    int tb = rt_base[l].y;
    int sp = 0;
    stack[sp++] = 0;
    int steps = 0;
    while (sp > 0 && steps < 400) {
      steps++;
      int ni = stack[--sp];
      vec4 a = rt_nodes[(nb + ni) * 2];
      vec4 b = rt_nodes[(nb + ni) * 2 + 1];
      vec3 t0 = (a.xyz - o) * inv;
      vec3 t1 = (b.xyz - o) * inv;
      vec3 tlo = min(t0, t1);
      vec3 thi = max(t0, t1);
      float tn = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
      float tf = min(min(thi.x, thi.y), min(thi.z, best));
      if (tn > tf) {
        continue;
      }
      int first = int(floatBitsToUint(a.w));
      int count = int(floatBitsToUint(b.w));
      if (count > 0) {
        for (int k = 0; k < count; k++) {
          int ti = (tb + first + k) * 5;
          vec3 v0 = rt_tris[ti].xyz;
          vec3 e1 = rt_tris[ti + 1].xyz;
          vec3 e2 = rt_tris[ti + 2].xyz;
          vec3 pv = cross(d, e2);
          float det = dot(e1, pv);
          if (abs(det) < 1e-4) {
            continue;
          }
          float id = 1.0 / det;
          vec3 tv = o - v0;
          float u = dot(tv, pv) * id;
          if (u < 0.0 || u > 1.0) {
            continue;
          }
          vec3 qv = cross(tv, e1);
          float v = dot(d, qv) * id;
          if (v < 0.0 || u + v > 1.0) {
            continue;
          }
          float t = dot(e2, qv) * id;
          if (t > 0.0 && t < best) {
            best = t;
            best_tri = ti;
            best_level = l;
            best_uv = vec2(u, v);
          }
        }
      } else if (sp < 31) {
        stack[sp++] = first;   // the right child (the farther one, usually: later)
        stack[sp++] = ni + 1;  // the left child, next
      }
    }
  }
  if (best_tri < 0) {
    return false;
  }
  vec3 tex = unpackUnorm4x8(floatBitsToUint(rt_tris[best_tri].w)).rgb;
  uint layer = floatBitsToUint(rt_tris[best_tri + 2].w);
  if (layer != 0xffffffffu && rt_base[best_level].w >= 0) {
    vec4 a = rt_tris[best_tri + 3];
    vec2 uv2 = rt_tris[best_tri + 4].xy;
    vec2 tuv = a.xy * (1.0 - best_uv.x - best_uv.y) + a.zw * best_uv.x + uv2 * best_uv.y;
    // coarser further away (the ray's footprint grows with the distance)
    float lod = clamp(log2(best / (6.0 * 4096.0)), 0.0, 6.0);
    tex = textureLod(tex_T12, vec3(tuv, float(rt_base[best_level].w + int(layer))), lod).rgb;
  }
  int pi = (rt_base[best_level].z + int(floatBitsToUint(rt_tris[best_tri + 1].w))) * 8;
  vec4 tod = vec4(0.0);
  for (int k = 0; k < 8; k++) {
    tod += unpackUnorm4x8(rt_pal[pi + k]) * rt_tod[k];
  }
  // as the game's background shader: (time-of-day color * 2) * texture
  color = min(tod.rgb, vec3(1.0)) * 2.0 * tex;
  // the distance haze the game would put on it
  color = mix(color, fog_color, smoothstep(40.0 * 4096.0, 450.0 * 4096.0, best) * 0.85);
  dist = best;
  return true;
}

// dark nights on a color taken from the scene (reflections read it before the night darkens it):
// the same darkening, what shines by itself kept
vec3 night_dim(vec3 col) {
  if (night_dark.x <= 0.0) {
    return col;
  }
  float l = luma(col);
  vec3 moon = vec3(l) * mix(vec3(0.7, 0.82, 1.1), vec3(1.1, 0.55, 0.45), sky_kind.z);
  vec3 darkened = mix(col, moon, 0.45 * night_dark.x) * (1.0 - 0.9 * night_dark.x);
  return mix(darkened, col, smoothstep(0.4, 0.85, l));
}

// a reflection: the screen-space one where it found something (ssr_hit), else what the ray meets
// in the levels (rt_mode), else env (the screen where the ray vanishes, or the fog color)
vec3 reflection_with_rt(vec3 p, vec3 r, vec3 env, vec3 ssr, float ssr_hit, float tmax,
                        float amount) {
  if (rt_mode >= 2) {
    ssr_hit = 0.0;
  }
  if (rt_mode == 3) {
    env = vec3(1.0, 0.0, 1.0);
  }
  if (rt_mode > 0 && ssr_hit < 0.99) {
    vec3 o = view_to_world * p + cam_pos;
    vec3 d = normalize(view_to_world * r);
    vec3 rc;
    float rd;
    // start a little off the surface, so it doesn't find itself
    if (amount > 0.0 && rt_trace(o + d * (0.1 * 4096.0), d, tmax, rc, rd)) {
      env = mix(env, rc, amount);
    }
  }
  return night_dim(mix(env, ssr, ssr_hit));
}

// what a surface reflects along r (view space): the scene where the ray meets the depth buffer
// (a short march), else the screen where the ray vanishes (the sky, far hills); hit: how sure
vec3 trace_reflection(vec3 p, vec3 r, float z, out float hit) {
  hit = 0.0;
  vec3 env = fog_color;
  if (r.z > 1e-3) {
    vec2 env_uv = clamp(0.5 * vec2(proj.z * r.x, proj.w * r.y) / r.z + 0.5, vec2(0.002), vec2(0.998));
    env = texture(tex_T0, env_uv).rgb;
  }
  vec3 ssr = env;
  float step_len = 0.15 * 4096.0 + z * 0.01;
  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                    haze.w * 7.13);
  vec3 q = p + r * (0.05 * 4096.0 + z * 0.002 + step_len * jit);
  for (int i = 0; i < 20; i++) {
    float used = step_len;
    q += r * step_len;
    step_len *= 1.15;
    if (q.z <= 0.0) {
      break;
    }
    vec2 quv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
    if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
      break;
    }
    float sd = texture(tex_T3, quv).r;
    if (sd <= 1e-7) {
      continue;
    }
    float scene_z = view_z(sd);
    if (q.z > scene_z && q.z - scene_z < step_len + 2048.0) {
      // halve back and forth between the last step in front of the scene and this one
      vec3 lo = q - r * used;
      vec3 hi = q;
      for (int k = 0; k < 5; k++) {
        vec3 mid = 0.5 * (lo + hi);
        vec2 muv = 0.5 * vec2(proj.z * mid.x, proj.w * mid.y) / max(mid.z, 1.0) + 0.5;
        float md = texture(tex_T3, muv).r;
        if (md > 1e-7 && mid.z > view_z(md)) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      if (hi.z > 0.0) {
        quv = 0.5 * vec2(proj.z * hi.x, proj.w * hi.y) / hi.z + 0.5;
      }
      // (it only passed behind an edge, the depth jumps there: the pixel isn't what it reflects,
      // it would smear a streak over the surface. The ray goes on)
      float hd = texture(tex_T3, quv).r;
      if (hd <= 1e-7 || abs(hi.z - view_z(hd)) > used * 0.25 + 0.1 * 4096.0) {
        continue;
      }
      vec2 edge = min(quv, 1.0 - quv);
      hit = smoothstep(0.0, 0.06, min(edge.x, edge.y));
      ssr = texture(tex_T0, quv).rgb;
      break;
    }
  }
  vec3 refl = reflection_with_rt(p, r, env, ssr, hit, 60.0 * 4096.0, 1.0);
  if (rt_mode > 0) {
    hit = 1.0;  // traced or not, it's the best there is
  }
  return refl;
}

// a per-pixel angle that changes every frame (interleaved gradient noise; the TAA averages it)
float pixel_angle() {
  return 6.2832 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                        haze.w * 7.13);
}

// a surface's reflection direction, not a perfect mirror's: a still fine grain in the surface, and
// each ray spread a little differently every frame (the TAA averages it to a soft, slightly matte
// look). rough: the spread's size (radians, about)
// specular AA (Kaplanyan 2016, Tokuyoshi): a Blinn-Phong exponent widened by the normal's variance
// across the pixel, so a lobe narrower than that spread can't light single pixels the TAA rejects
float aa_exp(float e, float var) {
  float a2 = 2.0 / (e + 2.0);
  a2 = min(a2 + min(var, tune_spec_aa_max), 1.0);
  return max(2.0 / a2 - 2.0, 1.0);
}

vec3 rough_reflection(vec3 p, vec3 v, vec3 n, float z, float rough) {
  vec3 a = abs(n.x) < 0.7 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
  vec3 t = normalize(cross(n, a));
  vec3 b = cross(n, t);
  // the grain sits on the surface (world space; from the plane it faces), fading out with distance
  // before it would shimmer
  vec3 w = (view_to_world * p + cam_pos) / 4096.0;
  vec3 wn = abs(view_to_world * n);
  vec2 g = wn.y > wn.x && wn.y > wn.z ? w.xz : (wn.x > wn.z ? w.yz : w.xy);
  float near = 1.0 - smoothstep(6.0 * 4096.0, 20.0 * 4096.0, z);
  vec2 grain = vec2(gnoise(g * 45.0), gnoise(g * 45.0 + 11.7)) * near * 1.6;
  float ang = pixel_angle();
  float rad = sqrt(fract(ang * 1.618));
  vec2 spread = vec2(cos(ang), sin(ang)) * rad;
  vec3 m = normalize(n + (t * (grain.x + spread.x) + b * (grain.y + spread.y)) * rough * 0.5);
  vec3 r = reflect(v, m);
  // (a tilted micro-surface must not reflect into the surface itself)
  float below = dot(r, n);
  return below < 0.02 ? normalize(r + n * (0.02 - below)) : r;
}

// the i-th of n points spread evenly over the unit disk (Vogel), turned by rot
vec2 vogel(int i, int n, float rot) {
  float r = sqrt((float(i) + 0.5) / float(n));
  float a = float(i) * 2.39996 + rot;
  return r * vec2(cos(a), sin(a));
}

// the normal averaged over about a meter around this pixel (only what lies at about the same depth,
// and not past 40 m, where a meter is a few pixels), so a light's edge doesn't draw the triangles
vec3 soft_normal(vec2 uv, vec3 n, float z) {
  if (z > 40.0 * 4096.0) {
    return n;
  }
  vec2 r = vec2(proj.z, proj.w) * 0.5 * 4096.0 / z;
  float rot = pixel_angle();
  vec3 sum = n;
  for (int i = 0; i < 6; i++) {
    vec2 q = uv + vogel(i, 6, rot) * r;
    vec4 g = gbuf(q);
    if (g.a > 0.17 && abs(view_z(texture(tex_T3, q).r) - z) < 1.5 * 4096.0) {
      vec3 m = normalize((g.rgb * 2.0 - 1.0) * view_to_world);
      sum += dot(m, n) < -0.2 ? -m : m;
    }
  }
  return normalize(sum);
}

// the far map: past the near one, the long shadows of the mountains, big rocks and trees (0.6 m
// texels: a 3x3 filter, no penumbra search)
// the near map's depth bias: 7 cm along the light, whatever its span (shadow.near_depth_m)
#define NEAR_BIAS (0.072 / shadow_params.x)

float sun_shadow_far(vec3 w) {
  if (shadow_far_on <= 0.0 || abs(tune_shadow_dbg - 3.0) < 0.5) {
    return 0.0;
  }
  vec3 s = (light_mvp_far * vec4(w, 1.0)).xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0)))) {
    return 0.0;
  }
  vec2 e = abs(s.xy * 2.0 - 1.0);
  float fade = 1.0 - smoothstep(0.85, 1.0, max(e.x, e.y));
  vec2 t = vec2(1.0 / 4096.0);
  float lit = 0.0;
  // (the bias in meters along the light, about one far texel: the constant it was came to 1.5 m
  // and pulled the rocks' shadows meters off their feet in the low sun)
  float bias = tune_shadow_far_bias_m / max(shadow_far_span, 1.0);
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      lit += texture(tex_T22, vec3(s.xy + vec2(x, y) * t, s.z - bias));
    }
  }
  return (1.0 - lit / 9.0) * fade;
}

// the middle map: from the near one's edge out to ~320 m (31 cm texels: houses, palms and rocks
// keep their shapes there, the far map's 59 cm blurred them to blobs); 16 taps over ~1.5 texels,
// fading into the far map toward its edge
float sun_shadow_mid(vec3 w) {
  if (shadow_mid_on <= 0.0 || abs(tune_shadow_dbg - 5.0) < 0.5) {
    return sun_shadow_far(w);
  }
  vec3 s = (light_mvp_mid * vec4(w, 1.0)).xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0)))) {
    return sun_shadow_far(w);
  }
  vec2 e = abs(s.xy * 2.0 - 1.0);
  float fade = 1.0 - smoothstep(0.8, 1.0, max(e.x, e.y));
  float far = fade < 1.0 ? sun_shadow_far(w) : 0.0;
  float bias = tune_shadow_mid_bias_m / max(shadow_mid_span, 1.0);
  float rot = pixel_angle();
  float r = 1.5 / 2048.0;
  float lit = 0.0;
  for (int i = 0; i < 16; i++) {
    lit += texture(tex_T26, vec3(s.xy + vogel(i, 16, rot) * r, s.z - bias));
  }
  return mix(far, 1.0 - lit / 16.0, fade);
}

// a cheap look: whether this point stands in the shadow of something (four taps, no penumbra)
float sun_shadow_rough(vec3 w) {
  vec3 s = (light_mvp * vec4(w, 1.0)).xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0)))) {
    return sun_shadow_mid(w);
  }
  float r = 0.5 / shadow_params.y;
  float lit = texture(tex_T9, vec3(s.xy + vec2(r, r), s.z - NEAR_BIAS)) +
              texture(tex_T9, vec3(s.xy + vec2(-r, r), s.z - NEAR_BIAS)) +
              texture(tex_T9, vec3(s.xy + vec2(r, -r), s.z - NEAR_BIAS)) +
              texture(tex_T9, vec3(s.xy + vec2(-r, -r), s.z - NEAR_BIAS));
  return 1.0 - lit * 0.25;
}

float sun_shadow(vec3 w) {
  vec4 l = light_mvp * vec4(w, 1.0);
  vec3 s = l.xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0))) || abs(tune_shadow_dbg - 4.0) < 0.5) {
    return sun_shadow_mid(w);
  }
  // fade out toward the edge of the map, into the far one
  vec2 e = abs(s.xy * 2.0 - 1.0);
  float fade = 1.0 - smoothstep(0.8, 1.0, max(e.x, e.y));
  float far = fade < 1.0 ? sun_shadow_mid(w) : 0.0;
  if (shadow_params.w > 0.0) {
    // soft shadows (PCSS): how far above this point the things in the way are (the blockers'
    // average depth, 12 taps over 1.5 m) sets how wide the penumbra is: sharp where a wall meets
    // the ground, soft under a high roof
    float rot = pixel_angle();
    float search = 1.5 / shadow_params.y;
    float blockers = 0.0;
    float found = 0.0;
    for (int i = 0; i < 12; i++) {
      float bd = texture(tex_T13, s.xy + vogel(i, 12, rot) * search).r;
      if (bd < s.z - NEAR_BIAS) {
        blockers += bd;
        found += 1.0;
      }
    }
    if (found == 0.0) {
      return far * (1.0 - fade);
    }
    float gap_m = (s.z - blockers / found) * shadow_params.x;
    float penumbra_m = clamp(gap_m * shadow_params.z, 0.05, 1.2);
    float r = penumbra_m / shadow_params.y;
    // a wider filter reaches further over sloped ground: more bias with it
    float bias = NEAR_BIAS + penumbra_m / shadow_params.x * 0.6;
    float lit = 0.0;
    for (int i = 0; i < 16; i++) {
      lit += texture(tex_T9, vec3(s.xy + vogel(i, 16, rot + 1.3) * r, s.z - bias));
    }
    return mix(far, 1.0 - lit / 16.0, fade);
  }
  vec2 t = vec2(1.0 / 4096.0);
  float lit = 0.0;
  for (int y = -1; y <= 2; y++) {
    for (int x = -1; x <= 2; x++) {
      lit += texture(tex_T9, vec3(s.xy + (vec2(x, y) - 0.5) * t, s.z - NEAR_BIAS));
    }
  }
  return mix(far, 1.0 - lit / 16.0, fade);
}

// a character's sun shadow: only casters at least gap_m toward the sun (not its own body); 8 taps
// over 25 cm, a soft edge as it walks into a wall's shade
float chr_sun_shadow(vec3 w, float gap_m) {
  vec3 s = (light_mvp * vec4(w, 1.0)).xyz * 0.5 + 0.5;
  if (any(lessThan(s, vec3(0.0))) || any(greaterThan(s, vec3(1.0)))) {
    return sun_shadow_mid(w);
  }
  float r = 0.25 / shadow_params.y;
  float bias = gap_m / shadow_params.x;
  float rot = pixel_angle();
  float lit = 0.0;
  for (int i = 0; i < 8; i++) {
    lit += texture(tex_T9, vec3(s.xy + vogel(i, 8, rot) * r, s.z - bias));
  }
  return 1.0 - lit / 8.0;
}

// a view direction (view space) in world axes (y up)
vec3 world_dir(vec3 dv) {
  return vec3(dot(right_view, dv), dot(up_view, dv), dot(fwd_view, dv));
}

float hash1(vec2 q) {
  return fract(sin(dot(q, vec2(127.1, 311.7))) * 43758.5453);
}

float fbm(vec2 q) {
  float sum = 0.0, amp = 0.5;
  mat2 turn = mat2(0.8, -0.6, 0.6, 0.8);
  for (int i = 0; i < 5; i++) {
    sum += amp * gnoise(q);
    q = turn * q * 2.03 + 3.7;
    amp *= 0.5;
  }
  return sum;
}

// 3D value noise on an integer hash (no sin: stays clean kilometers from the origin)
float ihash3(vec3 c) {
  uvec3 v = uvec3(ivec3(c) + ivec3(1 << 20));
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z;
  v.y += v.z * v.x;
  v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z;
  return float(v.x & 0xffffffu) / 16777215.0;
}

float vnoise3(vec3 x) {
  vec3 i = floor(x);
  vec3 f = fract(x);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(ihash3(i), ihash3(i + vec3(1, 0, 0)), f.x),
                 mix(ihash3(i + vec3(0, 1, 0)), ihash3(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(ihash3(i + vec3(0, 0, 1)), ihash3(i + vec3(1, 0, 1)), f.x),
                 mix(ihash3(i + vec3(0, 1, 1)), ihash3(i + vec3(1, 1, 1)), f.x), f.y),
             f.z);
}

// the dust and spray puffs along the view ray, up to the scene (m): soft balls that grow, rise (the
// spray falls back) and fade, broken up by noise; dust drifts with the wind. Premultiplied color,
// coverage.
vec4 ride_puffs(vec3 ray_view, float scene_dist, vec3 sand, vec3 spray, vec3 sun_w) {
  vec3 o = cam_pos / 4096.0;
  vec3 d = view_to_world * ray_view;
  vec3 col = vec3(0.0);
  float trans = 1.0;
  for (int i = 0; i < puff_count; i++) {
    float age = puff[i].w;
    float u = age / puff_info[i].w;
    if (u >= 1.0) {
      continue;
    }
    int kind = int(puff_info[i].y + 0.5);
    bool dust = kind != 1;
    float seed = puff_info[i].z;
    vec3 c = puff[i].xyz / 4096.0;
    float r;
    if (kind == 3) {
      // smoke: a column that swells as it climbs and leans with the wind
      r = puff_info[i].x * (0.5 + 1.8 * sqrt(u));
      c.y += r * 0.3 + 1.4 * age;
      c.xz += dust_wind.xy * age * 2.5;
    } else if (kind == 2) {
      // snow kicked up: a quick white burst that settles back down
      r = puff_info[i].x * (0.5 + 1.2 * sqrt(u));
      c.y += r * 0.25 + 0.6 * age - 0.5 * age * age;
      c.xz += dust_wind.xy * age;
    } else if (dust) {
      r = puff_info[i].x * (0.45 + 1.6 * sqrt(u));
      // kicked up off the ground: low at first, its lower half in the sand, rising slowly
      c.y += r * 0.2 + 0.3 * age;
      c.xz += dust_wind.xy * age * 1.5;
    } else {
      r = puff_info[i].x * (0.5 + 1.1 * sqrt(u));
      c.y += r * 0.3 + 3.0 * age - 4.0 * age * age;
    }
    vec3 oc = c - o;
    float tc = dot(oc, d);
    float d2 = dot(oc, oc) - tc * tc;
    if (d2 >= r * r || tc + r < 0.0) {
      continue;
    }
    float half_chord = sqrt(r * r - d2);
    float t0 = max(tc - half_chord, 0.0);
    float t1 = min(tc + half_chord, scene_dist);
    if (t1 <= t0) {
      continue;
    }
    // four samples through it: billows of 3D noise, thinning toward the edge
    float dens = 0.0;
    vec3 lit_dir = vec3(0.0);
    float dt = (t1 - t0) / 4.0;
    for (int k = 0; k < 4; k++) {
      vec3 q = (o + d * (t0 + dt * (float(k) + 0.5)) - c) / r;
      float fall = max(1.0 - dot(q, q), 0.0);
      vec3 nq = q * 1.9 + vec3(seed * 17.0, -age * 0.9, seed * 5.0);
      float n = vnoise3(nq) * 0.6 + vnoise3(nq * 2.3 + 4.0) * 0.4;
      float sd = fall * smoothstep(0.42, 0.72, n + fall * 0.2);
      if (!dust) {
        // drops: a fine grain on top of the mist
        sd = sd * 0.6 + fall * smoothstep(0.6, 0.8, vnoise3(q * 9.0 + vec3(seed * 9.0, age * 6.0, 0.0))) * 0.8;
      }
      dens += sd * dt / r;
      lit_dir += q * sd;
    }
    float lit = 0.75 + 0.25 * dot(normalize(lit_dir + vec3(0.0, 1e-3, 0.0)), sun_w);
    float a;
    vec3 pc;
    if (kind == 3) {
      // (thin and grey: a wisp, not a ball; steamier in the rain)
      a = min(dens * (1.0 - smoothstep(0.15, 1.0, u)) * smoothstep(0.0, 0.15, age) * 0.75, 0.5);
      pc = vec3(luma(sand)) * mix(0.7, 0.95, weather.x) * lit;
      // lit from below by the fires near it (orange, strongest in the column's lower part) and by
      // the game's glows (lamps, eco) around it
      vec3 glow = vec3(0.0);
      for (int k = 0; k < fire_count; k++) {
        vec3 fw = o + view_to_world * (fire_pos[k].xyz / 4096.0);
        float fr = 2.5 + 3.0 * fire_pos[k].w / 4096.0;
        vec3 fd = c - fw;
        glow += vec3(1.0, 0.42, 0.12) * exp(-dot(fd, fd) / (fr * fr)) * (1.2 - 0.6 * u);
      }
      for (int k = 0; k < light_count; k++) {
        vec3 lw = o + view_to_world * (light_pos[k].xyz / 4096.0);
        float lr = max(light_pos[k].w / 4096.0, 0.5);
        vec3 ld = c - lw;
        glow += light_color[k].rgb * max(1.0 - dot(ld, ld) / (lr * lr), 0.0) * 0.5;
      }
      pc += vec3(luma(sand)) * glow;
    } else if (kind == 2) {
      a = dens * (1.0 - smoothstep(0.2, 1.0, u)) * smoothstep(0.0, 0.04, age) * 1.4;
      pc = vec3(luma(spray)) * vec3(1.05, 1.1, 1.18) * lit;
    } else if (dust) {
      a = dens * (1.0 - smoothstep(0.3, 1.0, u)) * smoothstep(0.0, 0.06, age) * 1.3;
      pc = sand * lit;
    } else {
      a = dens * (1.0 - smoothstep(0.2, 1.0, u)) * 1.2;
      pc = spray * lit;
    }
    a = clamp(a, 0.0, 0.8);
    col += trans * a * pc;
    trans *= 1.0 - a;
  }
  return vec4(col, 1.0 - trans);
}

// a piece thrown by an explosion, age seconds later: gravity, a little drag, and one bounce on the
// ground (g: its height from the start along up, > 0: none), then it lies there (rest). Analytic, and
// only pieces under the ground look for when they hit it: the sparks' loop runs per pixel
vec3 blast_arc(vec3 bp, vec3 dir, float speed, float age, float g, out vec3 vel, out bool rest) {
  const float fall = 0.5 * 9.8 * 4096.0 * 0.7;
  rest = false;
  vec3 sp = bp + dir * speed * (1.0 - exp(-age * 2.0)) * 0.5 - up_view * (fall * age * age);
  vel = dir * speed * exp(-age * 2.0) - up_view * (2.0 * fall * age);
  if (g > 0.0 || dot(sp - bp, up_view) >= g) {
    return sp;
  }
  float vy = dot(dir, up_view) * speed;
  float t0 = 0.0, t1 = age;
  for (int k = 0; k < 5; k++) {
    float t = 0.5 * (t0 + t1);
    if (vy * (1.0 - exp(-t * 2.0)) * 0.5 - fall * t * t > g) {
      t0 = t;
    } else {
      t1 = t;
    }
  }
  float drag = exp(-t1 * 2.0);
  vec3 hit = bp + dir * speed * (1.0 - drag) * 0.5 - up_view * (fall * t1 * t1);
  vec3 v = dir * speed * drag - up_view * (2.0 * fall * t1);
  float vn = dot(v, up_view);
  // (loses most of its fall and some of its way along the ground)
  vec3 vb = (v - up_view * vn) * 0.6 - up_view * (vn * 0.45);
  float tau = age - t1;
  float second = -vn * 0.45 / fall;  // when it comes down again
  if (tau >= second) {
    rest = true;
    vel = vec3(0.0);
    return hit + (vb - up_view * dot(vb, up_view)) * second;
  }
  vel = vb - up_view * (2.0 * fall * tau);
  return hit + vb * tau - up_view * (fall * tau * tau);
}

// the dark chunks an explosion throws: small tumbling bits of rock and metal, hot at first, hidden
// behind the scene, that bounce once and lie on the ground until the blast's end
void blast_chunks(int i, float z, inout vec3 c) {
  float age = blast_age[i];
  vec3 bp = blast_pos[i].xyz;
  if (age > 2.4 || bp.z < 1.0 * 4096.0) {
    return;
  }
  float sc = clamp(blast_pos[i].w / (4.0 * 4096.0), 0.6, 2.5);
  // (far from the pixel on screen: they fly up to ~15 m)
  vec2 cuv = vec2(bp.x * proj.z / bp.z, bp.y * proj.w / bp.z) * 0.5 + 0.5;
  vec2 reach = abs(vec2(proj.z, proj.w)) * 0.5 * (16.0 * sc * 4096.0) / max(bp.z - 15.0 * sc * 4096.0, 2.0 * 4096.0);
  vec2 off = abs(tex_coord - cuv);
  if (off.x > reach.x || off.y > reach.y) {
    return;
  }
  for (int j = 0; j < 12; j++) {
    float s = blast_seed[i] * 9.1 + float(j) * 5.3;
    float h1 = fract(sin(s * 12.9898) * 43758.5453);
    float h2 = fract(sin(s * 78.233) * 43758.5453);
    float h3 = fract(sin(s * 37.719) * 43758.5453);
    float h4 = fract(sin(s * 93.989) * 43758.5453);
    vec3 dw = normalize(vec3(h1 - 0.5, 0.35 + h2 * 0.65, h3 - 0.5));
    vec3 dir = right_view * dw.x + up_view * dw.y + fwd_view * dw.z;
    vec3 vel;
    bool rest;
    vec3 sp = blast_arc(bp, dir, (5.0 + 11.0 * h4) * sc * 4096.0, age, blast_ground[i], vel, rest);
    if (sp.z < 0.5 * 4096.0 || (z > 0.0 && sp.z > z + 0.1 * 4096.0)) {
      continue;
    }
    vec2 a = vec2(sp.x * proj.z / sp.z, sp.y * proj.w / sp.z) * 0.5 + 0.5;
    vec2 pa = (tex_coord - a) / texel;
    float px = max((0.05 + 0.13 * h1) * sc * 4096.0 * abs(proj.z) * 0.5 / (sp.z * texel.x), 0.8);
    if (dot(pa, pa) > px * px * 4.0) {
      continue;
    }
    // (tumbling until it has settled, about a second in)
    float ang = (h2 - 0.5) * 22.0 * min(age, 1.0) + h3 * 6.283;
    vec2 q = abs(mat2(cos(ang), -sin(ang), sin(ang), cos(ang)) * pa);
    float shape = max(q.x, q.y * (1.4 + h3));
    float cov = (1.0 - smoothstep(px * 0.7, px, shape)) * (1.0 - smoothstep(1.8, 2.4, age)) * blast_sparks;
    vec3 col = vec3(0.05, 0.045, 0.04) + vec3(1.0, 0.3, 0.05) * 1.4 * exp(-age * 3.5);
    c = mix(c, col, clamp(cov, 0.0, 1.0) * min(fx_detail * 2.0, 1.0));
  }
}

// an explosion's layers over the scene along the ray (the flash and the sparks are drawn apart, the
// smoke is puffs): the ring of dust it blows out along the ground, and the fireball, boiling from
// white to orange to soot. Everything scales with the blast's radius. Premultiplied color, coverage.
vec4 blast_layers(int i, vec3 ray, float scene_dist, vec3 dust) {
  vec3 bp = blast_pos[i].xyz;
  float age = blast_age[i];
  float rad = clamp(blast_pos[i].w, 1.5 * 4096.0, 12.0 * 4096.0);
  float seed = blast_seed[i];
  vec3 col = vec3(0.0);
  float trans = 1.0;
  float upd = dot(ray, up_view);
  if (blast_ground[i] <= 0.0 && age < 2.3 && abs(upd) > 0.01) {
    // three sheets one over the other: low and wide, higher and behind it
    float grow = 1.0 - exp(-age * 3.0);
    float ring = rad * (0.35 + 1.9 * grow);
    float fade = smoothstep(0.0, 0.08, age) * (1.0 - smoothstep(0.9, 2.3, age));
    for (int k = 0; k < 3; k++) {
      float t = (dot(bp, up_view) + blast_ground[i] + float(k) * 0.14 * rad + 0.1 * 4096.0) / upd;
      if (t <= 0.0 || t > scene_dist) {
        continue;
      }
      vec3 hp = ray * t - bp;
      float rr = length(hp - up_view * dot(hp, up_view));
      float band = (rr - ring * (1.0 - 0.13 * float(k))) / (rad * (0.28 + 0.35 * grow) * (1.0 - 0.2 * float(k)));
      if (abs(band) > 1.6) {
        continue;
      }
      float b = exp(-band * band * 2.5);
      vec3 nq = (view_to_world * hp) / (0.33 * rad) + vec3(seed * 7.0, float(k) * 5.0 - age * 0.6, seed);
      b *= smoothstep(0.25, 0.7, vnoise3(nq) + b * 0.15);
      float a = b * fade * clamp((scene_dist - t) / (0.5 * 4096.0), 0.0, 1.0) * (0.55 - 0.12 * float(k));
      col += trans * a * dust;
      trans *= 1.0 - a;
    }
  }
  if (age < 0.75) {
    float u = age / 0.75;
    float r = rad * (0.25 + 0.75 * (1.0 - exp(-age * 9.0)));
    float tc = dot(bp, ray);
    float h2 = dot(bp, bp) - tc * tc;
    if (h2 < r * r && tc + r > 0.0) {
      float half_chord = sqrt(r * r - h2);
      float t0 = max(tc - half_chord, 0.0);
      float t1 = min(tc + half_chord, scene_dist);
      if (t1 > t0) {
        vec3 q = view_to_world * (ray * (0.5 * (t0 + t1)) - bp) / r;
        float n = vnoise3(q * 2.6 + vec3(seed * 3.0, -age * 3.0, seed)) * 0.6 +
                  vnoise3(q * 6.0 + vec3(-seed, -age * 5.0, seed * 2.0)) * 0.4;
        float thick = (t1 - t0) / (2.0 * r);
        float body = smoothstep(0.35, 0.75, n + thick * 0.75 - u * 0.35) * (1.0 - smoothstep(0.6, 1.0, u));
        float temp = thick * 0.8 + n * 0.6 - u * 1.25;
        vec3 fire_c = mix(vec3(0.6, 0.08, 0.01), vec3(1.0, 0.45, 0.08), smoothstep(0.0, 0.4, temp));
        fire_c = mix(fire_c, vec3(1.0, 0.9, 0.55), smoothstep(0.45, 0.85, temp));
        float soot = body * mix(0.2, 0.85, smoothstep(0.1, 0.8, u)) * (1.0 - smoothstep(-0.1, 0.25, temp));
        col = col * (1.0 - soot) + vec3(0.035, 0.03, 0.028) * soot + fire_c * body * (2.2 - 1.6 * u);
        trans *= 1.0 - max(soot, body * 0.5);
      }
    }
  }
  float k = fx_detail * blast_strength;
  return vec4(col * k, 1.0 - mix(1.0, trans, min(k, 1.0)));
}

// the sandstorm's air at a world point (m): rolling clouds of sand stretched along the wind and
// carried by it (the game storm's own wind), thicker near the ground, with fine streamers blowing
// low over it close by. Roughly 1 on average.
float dust_density(vec3 w, float near, float ground) {
  vec2 dir = dust_wind.xy;
  vec2 q = w.xz - dust_wind.zw;
  // along the wind, up, across it; the clouds boil slowly as they go
  vec3 s = vec3(dot(q, dir), w.y + clouds.z * 0.35, dot(q, vec2(-dir.y, dir.x)));
  float big = vnoise3(s * vec3(1.0 / 26.0, 1.0 / 9.0, 1.0 / 11.0));
  float mid = vnoise3(s * vec3(1.0 / 8.0, 1.0 / 3.5, 1.0 / 3.5) + vec3(17.0, 3.0, 5.0));
  float n = big * 0.65 + mid * 0.35;
  float dens = 0.2 + 1.6 * smoothstep(0.32, 0.8, n);
  // low streamers: the sand that skims the ground, fast, near the camera only
  if (near > 0.0) {
    vec3 sf = vec3(dot(q, dir) * 2.0, w.y, dot(q, vec2(-dir.y, dir.x)));
    float fine = vnoise3(sf * vec3(1.0 / 3.0, 1.0 / 0.5, 1.0 / 0.4) + vec3(3.0, 0.0, 9.0));
    dens += near * 0.9 * smoothstep(0.55, 0.9, fine);
  }
  // thicker toward the ground (taken as a little under the camera, or the ray's end when lower:
  // flying over the desert the ground is far under the camera), thin high up
  float h = w.y - ground;
  return dens * mix(1.0, 0.35, smoothstep(0.0, 40.0, h)) * mix(1.0, 0.12, smoothstep(40.0, 160.0, h));
}

// how much of the dust hides the scene along a view ray (view space, unit) up to dist (m), marched
// through the volume close up (where its shapes show) and in one step past that
float dust_cover(vec3 ray_view, float dist, float amount, float jitter, bool surface) {
  vec3 wd = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  float k = 0.006 * amount + 0.03 * amount * amount * amount;  // extinction per meter
  float march = min(dist, 160.0);
  // (the ground: a little under the camera, or where the ray ends when that is lower. In the Hellcat
  // 100+ m up, "3 m under the camera" filled the whole way down with the storm's thickest air and
  // the desert went one beige veil, the rocks chalk white; tune_haze_layer 0: as before)
  float ground = cam_pos.y / 4096.0 - 3.0;
  float end_h = 0.0;
  if (tune_haze_layer > 0.5 && (tune_haze_layer < 1.5 || gl_FragCoord.x > 0.5 / texel.x)) {
    ground = surface ? min(ground, o.y + wd.y * dist) : ground;
    end_h = surface ? max(o.y - ground, 0.0) : 0.0;
  }
  float tau = 0.0;
  float prev = 0.0;
  const int N = 12;
  for (int i = 0; i < N; i++) {
    // more steps near the camera
    float u = (float(i) + jitter) / float(N);
    float t = march * u * u;
    float near = 1.0 - smoothstep(4.0, 14.0, t);
    tau += dust_density(o + wd * t, near, ground) * (t - prev) * k;
    prev = t;
  }
  tau += dust_density(o + wd * march, 0.0, ground) * (march - prev) * k;
  // the rest of the way, at the average (of the part of it inside the thick low layer)
  tau += max(dist - march, 0.0) * k * 0.8 * 60.0 / max(end_h, 60.0);
  return 1.0 - exp(-tau);
}

// low fog lying on the ground (thickest a couple of meters under the camera, thinning upward):
// the height fog integral along the ray to dist (m)
float ground_fog(vec3 ray_view, float dist, float amount) {
  vec3 wd = world_dir(ray_view);
  const float H = 7.0;
  float base = exp(-2.0 / H);  // the density at the camera, which is ~2 m over the ground
  float dy = wd.y;
  float path = abs(dy) < 1e-3 ? dist : H / dy * (1.0 - exp(-dy * dist / H));
  // (never denser than at the ground, tune_fog_floor_m under the camera: looking down from a dock
  // ~15 m over the sea the density grew exp(15/7) ~ 8x and 300 m of harbour went one opaque milky
  // slab, hard-edged at the far wall that rises out of it)
  if (tune_fog_floor_m > 0.0 && dy < -1e-3) {
    float tc = tune_fog_floor_m / -dy;
    if (dist > tc) {
      path = H / dy * (1.0 - exp(-dy * tc / H)) + (dist - tc) * exp(tune_fog_floor_m / H);
    }
  }
  return min(1.0 - exp(-0.035 * amount * base * max(path, 0.0)), tune_fog_max);
}

// lightning, far out in the sky: a jagged channel from the clouds down to the horizon at the
// bolt's azimuth. The leader steps down from the top in under a tenth of a second (its tip
// brightest), then the return stroke lights the whole channel and its branches, it strikes again
// twice and fades. Returns the light (unitless, ~1 at the core).
float bolt_jag(float u, float seed) {
  // the channel's sideways offset (rad) at u (0 at the clouds, 1 at the horizon)
  return (gnoise(vec2(u * 5.0, seed)) * 0.07 + gnoise(vec2(u * 22.0, seed + 5.0)) * 0.022 +
          gnoise(vec2(u * 70.0, seed + 11.0)) * 0.006) * (0.4 + u);
}

// (az: its azimuth, seed: its shape, a: its age, s)
float lightning_bolt(vec3 ray_view, float px, float az, float seed, float a) {
  // the cloud base where it comes out (sine of the elevation): lower for a farther bolt
  // (a strike: from the cloud base above it down to its foot on the ground; the nearer, the bigger
  // it all is on screen: s scales the channel's sideways angles)
  bool ground = strike.w > 0.0;
  float top = ground ? strike.y : 0.14 + 0.16 * fract(seed * 0.37);
  float bottom = ground ? strike.x : 0.0;
  float s = ground ? clamp(200.0 * 4096.0 / strike.z, 0.5, 10.0) : 1.0;
  if (bolt.w <= 0.0 || a < 0.0 || a > 0.42) {
    return 0.0;
  }
  vec3 wd = world_dir(ray_view);
  float el = wd.y;
  if (el < bottom - 0.01 || el > top) {
    return 0.0;
  }
  float da = (mod(atan(wd.x, wd.z) - az + 3.14159, 6.28318) - 3.14159) / s;
  if (abs(da) > 0.5) {
    return 0.0;
  }
  float u = (top - el) / max(top - bottom, 1e-4);
  // the leader's tip
  float lead = clamp(a / 0.09, 0.0, 1.0);
  if (u > lead) {
    return 0.0;
  }
  float w = max(px * 1.3 / s, 0.0012);
  float x = da - bolt_jag(u, seed);
  float core = exp(-x * x / (w * w));
  float glow = exp(-x * x / 0.0012) * 0.3 + exp(-x * x / 0.015) * 0.08;
  // branches off the channel, reaching sideways and down
  for (int k = 0; k < 3; k++) {
    float b0 = 0.18 + 0.22 * float(k) + 0.08 * fract(seed * (1.3 + float(k)));
    float len = 0.2 + 0.1 * fract(seed * 3.7 + float(k));
    float bu = (u - b0) / len;
    if (bu > 0.0 && bu < 1.0) {
      float side = fract(seed * 7.1 + float(k) * 0.37) > 0.5 ? 1.0 : -1.0;
      float bx = da - bolt_jag(b0, seed) - side * bu * len * 0.45 -
                 gnoise(vec2(u * 40.0, seed + 20.0 + float(k))) * 0.012;
      float fade = 1.0 - bu;
      core += exp(-bx * bx / (w * w * 0.6)) * 0.6 * fade;
      glow += exp(-bx * bx / 0.0006) * 0.12 * fade;
    }
  }
  // over time: the dim leader with a bright tip, the return stroke, two restrikes, fading
  float bright;
  if (a < 0.09) {
    bright = 0.3 + 2.0 * exp(-sq((u - lead) / 0.03));
  } else {
    float t = a - 0.09;
    bright = 1.6 * exp(-t * 25.0) + 1.1 * exp(-sq((t - 0.08) * 60.0)) +
             0.7 * exp(-sq((t - 0.18) * 60.0));
    bright *= 1.0 - smoothstep(0.25, 0.33, t);
  }
  // out of the clouds at the top
  float in_cloud = smoothstep(0.0, 0.06, u);
  return (core + glow) * bright * in_cloud * bolt.w;
}

// the sky's lightning: often one bolt, sometimes two or three close together in the same flash,
// each a little after the last (a strike to the ground: just its own)
float sky_lightning(vec3 ray_view, float px) {
  float l = lightning_bolt(ray_view, px, bolt.x, bolt.y, bolt.z);
  if (strike.w > 0.0) {
    return l;
  }
  int extra = (fract(bolt.y * 5.3) > 0.5 ? 1 : 0) + (fract(bolt.y * 9.1) > 0.75 ? 1 : 0);
  for (int i = 1; i <= extra; i++) {
    float fi = float(i);
    float off = (0.12 + 0.35 * fract(bolt.y * (2.3 + fi))) * (fract(bolt.y * 4.7 * fi) > 0.5 ? 1.0 : -1.0);
    float delay = 0.04 + 0.1 * fract(bolt.y * (6.1 + fi));
    l += 0.8 * lightning_bolt(ray_view, px, bolt.x + off, bolt.y + 13.7 * fi, bolt.z - delay);
  }
  return l;
}

// the rainbow opposite the sun after the rain: the primary bow at 42 degrees, red outside, and a
// faint secondary one at 51 with its colors the other way round
vec3 rainbow(vec3 ray_view) {
  float a = acos(clamp(dot(ray_view, -sky_sun.xyz), -1.0, 1.0)) * 57.2958;
  vec3 col = vec3(0.0);
  for (int i = 0; i < 2; i++) {
    float center = i == 0 ? 41.5 : 51.0;
    float width = i == 0 ? 3.2 : 5.0;
    float t = (a - center) / width + 0.5;  // 0 violet .. 1 red (the other way for the secondary)
    if (t > 0.0 && t < 1.0) {
      float tt = i == 0 ? t : 1.0 - t;
      float hue = (1.0 - tt) * 0.78;
      vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
      col += rgb * smoothstep(0.0, 0.25, t) * smoothstep(1.0, 0.75, t) * (i == 0 ? 1.0 : 0.35);
    }
  }
  return col;
}

// the night sky over the game's: stars in two layers (cells on a cube of directions; twinkling,
// bluish to warm), a faint milky way, the moon's disk and halo; hidden where the game's sky is
// bright (its clouds), by the weather and by a city's glow
vec3 night_sky(vec3 dv, vec3 base, float px) {
  vec3 wd = world_dir(dv);
  float up = wd.y;
  vec3 add = vec3(0.0);
  float clear = (1.0 - smoothstep(0.1, 0.28, luma(base))) *
                (1.0 - clamp((clouds.y - 0.45) * 1.8, 0.0, 1.0)) * (1.0 - min(weather.x, 1.0)) *
                (1.0 - min(weather.y * 1.5, 1.0)) * (1.0 - min(weather2.y, 1.0)) * smoothstep(0.0, 0.15, up);
  if (clear > 0.0) {
    vec3 a = abs(wd);
    vec2 f;
    float face;
    if (a.x > a.y && a.x > a.z) {
      f = wd.yz / a.x;
      face = wd.x > 0.0 ? 0.0 : 1.0;
    } else if (a.y > a.z) {
      f = wd.xz / a.y;
      face = wd.y > 0.0 ? 2.0 : 3.0;
    } else {
      f = wd.xy / a.z;
      face = wd.z > 0.0 ? 4.0 : 5.0;
    }
    // the milky way: a band across the sky, mottled
    vec3 band_n = normalize(vec3(0.35, 0.45, 0.82));
    float bd = dot(wd, band_n);
    float band = exp(-bd * bd / 0.035) * (0.4 + 0.9 * vnoise3(wd * 7.0) * vnoise3(wd * 19.0 + 3.0));
    float city_dim = 1.0 - 0.75 * city_glow;
    add += vec3(0.55, 0.6, 0.78) * band * 0.045 * city_dim * sky_kind.x * sky_kind.x;
    for (int layer = 0; layer < 2; layer++) {
      float cells = layer == 0 ? 70.0 : 220.0;
      vec2 g = (f * 0.5 + 0.5) * cells;
      vec2 cid = floor(g);
      vec3 key = vec3(cid, face + 7.0 * float(layer));
      float h = ihash3(key);
      // (more of the faint ones along the milky way)
      float thr = layer == 0 ? 0.9 : 0.78 - 0.25 * band;
      // (a hazy or moonlit night shows only some of them: the brightest stay)
      if (h > thr && h > mix(1.0, thr, sky_kind.x)) {
        vec2 sp = vec2(ihash3(key + 11.0), ihash3(key + 23.0)) * 0.7 + 0.15;
        float ang = length(g - cid - sp) / cells * 1.5708;
        float size = px * (layer == 0 ? 1.1 : 0.75);
        float shape = exp(-ang * ang / (size * size));
        float b = (h - thr) / (1.0 - thr);
        b = layer == 0 ? 0.25 + 2.2 * b * b * b : 0.15 + 0.5 * b;
        float tw = 0.7 + 0.3 * sin(clouds.z * (2.0 + 5.0 * ihash3(key + 5.0)) + h * 60.0);
        vec3 tint = mix(vec3(0.72, 0.82, 1.0), vec3(1.0, 0.86, 0.66), ihash3(key + 31.0));
        add += tint * shape * b * tw * (layer == 0 ? 1.0 : city_dim);
      }
    }
    // a shooting star now and then (about one every 15 s, two in three slots empty): a thin streak
    // that crosses a bit of the sky in half a second, brightest at its head
    float slot = floor(clouds.z / 6.0);
    float since = clouds.z - slot * 6.0 - 6.0 * ihash3(vec3(slot, 3.0, 9.0)) * 0.8;
    if (ihash3(vec3(slot, 1.0, 5.0)) > 0.62 && since > 0.0 && since < 0.55) {
      float az = 6.2832 * ihash3(vec3(slot, 2.0, 7.0));
      float el = 0.45 + 0.6 * ihash3(vec3(slot, 4.0, 1.0));
      vec3 from = vec3(cos(az) * cos(el), sin(el), sin(az) * cos(el));
      vec3 across = normalize(cross(from, vec3(0.0, 1.0, 0.0)) + vec3(0.0, -0.35, 0.0));
      float prog = since / 0.55;
      vec3 head = normalize(from + across * 0.3 * prog);
      vec3 tail = normalize(from + across * 0.3 * max(prog - 0.35, 0.0));
      vec3 seg = head - tail;
      float k = clamp(dot(wd - tail, seg) / max(dot(seg, seg), 1e-6), 0.0, 1.0);
      float d = length(wd - (tail + seg * k));
      float w = px * 0.9;
      add += vec3(0.85, 0.9, 1.0) * exp(-d * d / (w * w)) * k * k * 1.6 * sin(3.1416 * prog) * city_dim;
    }
    add *= clear;
  }
  // the moon (the night's light comes from it): a pale disk, darker seas, a soft halo
  float md = dot(dv, sky_sun.xyz);
  if (md > 0.9 && sky_sun.w > 0.0) {
    float mclear = (1.0 - clamp((clouds.y - 0.6) * 2.0, 0.0, 1.0)) * (1.0 - min(weather.x + weather.y, 1.0));
    float rr = 0.054;  // (4x the real moon: a big storybook moon)
    float off = sqrt(max(2.0 * (1.0 - md), 0.0));
    float disk = 1.0 - smoothstep(rr - px, rr + px, off);
    vec3 side = normalize(cross(sky_sun.xyz, vec3(0.0, 1.0, 0.0)) + 1e-4);
    vec3 upv = cross(side, sky_sun.xyz);
    vec2 mq = vec2(dot(dv, side), dot(dv, upv)) / rr;
    float seas = smoothstep(0.45, 0.7, vnoise3(vec3(mq * 2.2, 1.7)) * 0.7 + vnoise3(vec3(mq * 5.0, 4.1)) * 0.3);
    float limb = sqrt(max(1.0 - dot(mq, mq), 0.0));
    vec3 moon = vec3(0.98, 0.96, 0.88) * (1.0 - 0.3 * seas) * (0.75 + 0.25 * limb) * 1.25;
    // its phase: lit right of the terminator (an ellipse across the disk), the rest by earthshine
    float term = (1.0 - 2.0 * sky_kind.y) * sqrt(max(1.0 - mq.y * mq.y, 0.0));
    moon *= mix(0.04, 1.0, smoothstep(term - 0.08, term + 0.08, -mq.x));
    // a full moon: brighter and a wider halo; the eclipse: Earth's shadow, a dim copper red
    float full = step(0.99, sky_kind.y) * (1.0 - sky_kind.z);
    moon *= 1.0 + 0.25 * full;
    vec3 blood = vec3(0.62, 0.17, 0.07) * (0.55 + 0.45 * limb) * (1.0 - 0.35 * seas) *
                 (0.7 + 0.3 * smoothstep(-1.0, 1.0, mq.x + mq.y));
    moon = mix(moon, blood, sky_kind.z);
    float halo = (pow(md, 60.0) * 0.25 + pow(md, 12.0) * 0.06) * (1.0 + 0.8 * full) * (1.0 - 0.6 * sky_kind.z);
    add = mix(add, moon, disk * mclear * smoothstep(0.0, 0.05, up));
    add += mix(vec3(0.6, 0.68, 0.85), vec3(0.7, 0.25, 0.15), sky_kind.z) * halo * (0.4 + 0.6 * mclear);
  }
  // a city's lights on the low sky
  add += vec3(1.0, 0.55, 0.25) * city_glow * 0.06 * (1.0 - smoothstep(-0.05, 0.4, up));
  return add * clouds.w;
}

// the sun's color by its height, the moon's at night
vec3 sun_color() {
  vec3 col = mix(vec3(1.0, 0.5, 0.28), vec3(1.0, 0.96, 0.9), smoothstep(0.05, 0.35, sky_sun.w));
  // the red sunset: the low sun through a thick sky, deep orange-red and strong
  col = mix(col, vec3(1.25, 0.36, 0.1), sky_kind.w * (1.0 - smoothstep(0.1, 0.45, sky_sun.w)));
  return mix(col, vec3(0.13, 0.15, 0.22), clouds.w);
}

// the desert's bright hours (0..1): noon in a hazy area, none of the late afternoon's amber
float desert_noon() {
  return clamp(noon * (1.0 - golden * 2.0), 0.0, 1.0) * min(sun_haze * 1.25, 1.0) * (1.0 - day_look);
}

// our sky by day: the horizon in the fog's color (so the distance melts into it), deepening to blue
// overhead, a glow and a disk for the sun; greyer the more overcast. Sunny: a deep summer blue.
vec3 sky_color(vec3 dv) {
  vec3 wd = world_dir(dv);
  float up = max(wd.y, 0.0);
  vec3 zen = mix(vec3(0.2, 0.4, 0.78), vec3(0.08, 0.32, 0.9), sunny);
  vec3 hor = mix(fog_color * 1.05, vec3(0.6, 0.8, 0.97), sunny * 0.8);
  {
    float nd = desert_noon();
    zen = mix(zen, vec3(0.13, 0.36, 0.82), nd * 0.6);
    hor = mix(hor, vec3(0.8, 0.85, 0.9), nd * 0.75);
  }
  vec3 col = mix(hor, zen, pow(smoothstep(0.0, 0.75, up), 0.7));
  // overcast: one grey
  float over = clamp((clouds.y - 0.5) * 2.0, 0.0, 1.0);
  col = mix(col, vec3(luma(hor)) * vec3(0.9, 0.94, 1.0) * (1.0 - 0.35 * weather2.x), over);
  float sd = max(dot(dv, sky_sun.xyz), 0.0);
  vec3 sc = sun_color();
  // sunsets: a warm band along the horizon, wider and brighter toward the sun; the red one burns
  // orange-red low down under a mauve sky (the sun's half of the sky the most)
  {
    float low = 1.0 - smoothstep(0.1, 0.45, sky_sun.w);
    float toward = 0.5 + 0.5 * dot(normalize(vec2(wd.x, wd.z) + 1e-4),
                                   normalize(world_dir(sky_sun.xyz).xz + 1e-4));
    float band = exp(-up * (6.0 - 3.0 * toward)) * (0.35 + 0.65 * toward * toward);
    vec3 warm = mix(vec3(1.0, 0.62, 0.32), vec3(1.0, 0.3, 0.1), sky_kind.w);
    vec3 above = mix(col, vec3(0.42, 0.24, 0.36), 0.75 * sky_kind.w * (1.0 - band));
    col = mix(col, above, low);
    col = mix(col, warm * (0.8 + 0.5 * sky_kind.w), band * low * (0.35 + 0.55 * sky_kind.w) * (1.0 - over * 0.6));
  }
  // the desert's late afternoon: a pale lilac sky over a dusky amber horizon, and a strong glow
  // low under the sun (full from half the area's golden look: a softer ground keeps the sky)
  if (golden > 0.0) {
    float gs = min(golden * 2.0, 1.0);
    vec3 lilac = mix(vec3(0.76, 0.5, 0.3), vec3(0.6, 0.58, 0.74), smoothstep(0.02, 0.28, up));
    col = mix(col, lilac, gs * 0.8 * (1.0 - over));
    col *= 1.0 - 0.25 * gs * (1.0 - over);
    // (the glow low on the horizon under the sun, wherever it stands: the light across the valley)
    vec2 sh = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
    float az = max(dot(normalize(wd.xz + 1e-4), sh), 0.0);
    float glow = 0.9 * pow(az, 6.0) * exp(-up * 6.0) + 0.6 * pow(az, 24.0) * exp(-up * 2.5);
    // (away from the sun the low sky dusky: dust and far cloud banks along the horizon)
    col *= 1.0 - 0.22 * gs * (1.0 - smoothstep(0.02, 0.22, up)) * (1.0 - pow(az, 4.0)) * (1.0 - over);
    col += vec3(1.0, 0.6, 0.3) * glow * gs * 0.2 * (1.0 - over * 0.7);
    col += sc * pow(sd, 16.0) * 0.3 * gs * (1.0 - over * 0.7);
  }
  // the desert's morning: a clear blue overhead, the low sky pink-peach toward the sun and a pale
  // lilac away from it, a warm yellow-white glow around the sun
  // (the same blue all day, deeper overhead as the sun climbs; the pink and peach the morning's)
  if (sky_day > 0.0) {
    float pink = clamp(sky_morning / max(sky_day, 1e-4), 0.0, 1.0);  // (0/0 outside day_sky zones and at night)
    vec2 sh = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
    float az = 0.5 + 0.5 * dot(normalize(wd.xz + 1e-4), sh);
    vec3 zen_m = mix(vec3(0.42, 0.65, 0.98), vec3(0.6, 0.8, 1.0), pink);
    vec3 hor_m = mix(vec3(0.68, 0.82, 0.98), vec3(1.0, 0.76, 0.66), pow(az, 2.5) * pink);
    vec3 mcol = mix(hor_m, zen_m, pow(smoothstep(0.0, 0.55, up), 0.75));
    // (a pink band above the horizon toward the sun)
    mcol = mix(mcol, vec3(1.0, 0.7, 0.72), exp(-up * 7.0) * pow(az, 3.0) * 0.45 * pink);
    float glow = pow(max(sd, 0.0), 8.0) * 0.6 + pow(max(sd, 0.0), 64.0) * 0.8;
    mcol += vec3(1.0, 0.86, 0.6) * glow * pink;
    // (and wide and low on the sun's side: the yellow-peach glow along the horizon)
    mcol = mix(mcol, vec3(1.0, 0.82, 0.55), clamp(pow(az, 6.0) * exp(-up * 3.5) * 0.85 * pink, 0.0, 1.0));
    col = mix(col, mcol, sky_day * (1.0 - over));
  }
  col += sc * (pow(sd, 6.0) * 0.18 + pow(sd, 60.0) * 0.35) * (1.0 - over * 0.7) * (1.0 - 0.6 * sky_day);
  col += sc * smoothstep(0.99955, 0.9998, sd) * 3.0 * (1.0 - over);
  return col;
}

// the cloud volume: a slab 1100-2000 m up, marched along the view ray (view space, unit). Rounded
// heaps of 3D noise (the sky cover sets how much of it is cloud), drifting with the wind; each step
// lit by the sun through the cloud above it toward the sun (two samples), forward scattering bright
// around the sun, the shaded undersides in the sky's own color, and lit from inside by lightning.
// Returns the light it adds (rgb) and what still shows through (a).
float cloud_density(vec3 p, float lo, int octaves) {
  float h = (p.y - 1100.0) / 900.0;
  if (h <= 0.0 || h >= 1.0) {
    return 0.0;
  }
  vec3 q = p / 1300.0 + vec3(clouds.z * 0.0045, 0.0, clouds.z * 0.002);
  float n = vnoise3(q) * 0.6 + vnoise3(q * 2.6 + 7.0) * 0.27;
  if (octaves > 2) {
    n += vnoise3(q * 7.3 + 3.0) * 0.13;
  } else {
    n += 0.065;
  }
  // flat bottoms, heaped tops
  float shape = smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(0.35, 1.0, h));
  // (the late afternoon's heaps with crisper edges: cumulus, clear sky between)
  float gsd = min(golden * 2.0, 1.0);
  n += (vnoise3(q * 4.1 + 11.0) - 0.5) * 0.14 * gsd;
  return clamp((n * shape - lo) * (4.0 + 6.0 * gsd), 0.0, 1.0);
}

vec4 cloud_volume(vec3 dv, float jitter, vec3 sky) {
  vec3 wd = world_dir(dv);
  if (wd.y < 0.015) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  float cam_y = cam_pos.y / 4096.0;
  float t0 = max((1100.0 - cam_y) / wd.y, 0.0);
  float t1 = min((2000.0 - cam_y) / wd.y, t0 + 7000.0);
  if (t0 > 30000.0) {
    return vec4(0.0, 0.0, 0.0, 1.0);
  }
  // (the late afternoon's heaps fewer over the desert: apart, with clear sky between)
  float lo = 0.6 - clouds.y * 0.42 + 0.03 * min(golden * 2.0, 1.0) * sun_haze + 0.13 * sky_day;
  vec3 sw = world_dir(sky_sun.xyz);
  vec3 sc = sun_color() * (1.0 - 0.6 * weather2.x);
  float cosang = dot(wd, sw);
  // two-lobe phase: a bright rim around the sun, some back scattering
  float phase = 0.6 + 1.6 * pow(max(cosang, 0.0), 6.0) + 0.25 * pow(max(-cosang, 0.0), 2.0);
  vec3 under = mix(sky * 0.55, vec3(luma(sky)) * 0.5, 0.3) * (1.0 - 0.45 * weather.x - 0.3 * weather2.x);
  vec3 top = sky * 0.9 + 0.08;
  // a warm hazy area (the desert): sunset clouds, gold where the sun lights them, dusty mauve
  // undersides and peach tops
  if (sun_haze > 0.0) {
    // (at noon white heaps with soft grey-blue undersides: the tints are the low sun's)
    float warm_k = sun_haze * (1.0 - 0.85 * desert_noon());
    sc *= mix(vec3(1.0), vec3(1.2, 0.86, 0.52), warm_k * 0.75);
    under = mix(under, vec3(0.5, 0.36, 0.38) * max(luma(under) * 2.2, 0.25 * (1.0 - clouds.w)), warm_k * 0.55);
    top = mix(top, vec3(0.95, 0.72, 0.55), warm_k * 0.35);
    // (and late in the day their shaded undersides a deep warm brown, the sun's gold on the rims)
    // (full from half the area's golden look, as the sky)
    float gs = min(golden * 2.0, 1.0);
    under = mix(under, vec3(0.3, 0.2, 0.15), gs * 0.9);
    top = mix(top, vec3(0.56, 0.46, 0.44), gs * 0.6);
    // (the sunlit sides a soft gold, not lit up: the bodies stay grey-brown against the sky)
    sc *= mix(vec3(1.0), vec3(1.4, 0.92, 0.58) * 0.85, gs);
  }
  // the day sky's (any outdoor area): small pink heaps in the morning, lilac-grey bellies, lit
  // soft peach; pink toward the sun, white-lilac away from it; by day the same heaps white
  if (sky_day > 0.0) {
    float tw = smoothstep(-0.3, 0.9, cosang) * clamp(sky_morning / max(sky_day, 1e-4), 0.0, 1.0);  // (0/0 outside day_sky zones and at night)
    under = mix(under, mix(vec3(0.74, 0.74, 0.84), vec3(0.8, 0.62, 0.7), tw), sky_day * 0.85);
    top = mix(top, mix(vec3(0.95, 0.94, 0.98), vec3(1.0, 0.8, 0.82), tw), sky_day * 0.8);
    sc = mix(sc, vec3(1.15, 0.85, 0.75), sky_morning * 0.7);
  }
  // (squared by hand: pow() of a negative base is undefined in GLSL and gave NaN on some drivers)
  float bolt_da = mod(atan(wd.x, wd.z) - bolt.x + 3.14159, 6.28318) - 3.14159;
  float near_bolt = exp(-bolt_da * bolt_da / 0.2);
  vec3 flash = vec3(0.8, 0.85, 1.0) * (weather2.w * (0.12 + 1.1 * near_bolt) + sheet * 0.9);
  // more steps toward the horizon, where the ray crosses kilometers of slab: long steps there
  // left a per-pixel grain (the jitter) the TAA couldn't smooth, drawn as fine diagonal hatching
  // (the sun shows, out of rain: the clouds thin out just round it, a soft gap, the disc mostly
  // clear; a heap may still drift across it)
  float sun_gap = smoothstep(0.975, 0.997, cosang) * (1.0 - weather.x) * (1.0 - weather2.x) * 0.85;
  int N = int(mix(34.0, 16.0, clamp(wd.y * 2.5, 0.0, 1.0)));
  float dt = (t1 - t0) / float(N);
  vec3 sum = vec3(0.0);
  float trans = 1.0;
  for (int i = 0; i < N; i++) {
    vec3 p = cam_pos / 4096.0 + wd * (t0 + (float(i) + jitter) * dt);
    float d = cloud_density(p, lo, 3) * (1.0 - sun_gap);
    if (d > 0.002) {
      float dl = cloud_density(p + sw * 90.0, lo, 2) * 90.0 + cloud_density(p + sw * 300.0, lo, 2) * 210.0;
      // (the late afternoon's heaps dense: dark cores under the low sun, bright thin rims)
      float lit = exp(-dl * 0.012 * (1.0 + 3.5 * min(golden * 2.0, 1.0) * sun_haze));
      float powder = 1.0 - exp(-d * 4.0);
      float h = (p.y - 1100.0) / 900.0;
      vec3 light = sc * lit * phase * (0.5 + 0.5 * powder) * 1.15 + mix(under, top, h) + flash * (1.0 - h * 0.5);
      // (the late afternoon's heaps darker than the sky behind them, as the low sun leaves them)
      light *= 1.0 - 0.3 * min(golden * 2.0, 1.0) * sun_haze;
      // (thinner by night: under the moon they read as dark cut-out slabs over the dim sky)
      // (the late afternoon's heaps solid: dark bellies, not veils)
      float a = 1.0 - exp(-d * 0.022 * (1.0 + 1.2 * min(golden * 2.0, 1.0)) * dt * (1.0 - 0.45 * max(clouds.w, 1.0 - smoothstep(0.08, 0.3, luma(sky)))));
      sum += trans * a * light;
      trans *= 1.0 - a;
      if (trans < 0.02) {
        break;
      }
    }
  }
  // far clouds melt into the haze at the horizon
  float haze = 1.0 - exp(-t0 / 14000.0);
  sum = mix(sum, (1.0 - trans) * sky, haze);
  // by night they keep close to the sky's color, a little lighter (moonlit), never blacker than it
  // (the dawn's and dusk's dim skies too: the game doesn't call those night)
  float dim = max(clouds.w, 1.0 - smoothstep(0.08, 0.3, luma(sky)));
  sum = mix(sum, (1.0 - trans) * sky * 1.15, 0.7 * dim);
  return vec4(sum, trans);
}

// the clouds at a quarter of the pixels (cloud_pass): a = -1 where no sky shows. Read back
// bilinearly from only the texels that hold clouds (they stop at the scene's edges)
vec4 cloud_fetch(vec2 uv) {
  vec2 size = vec2(textureSize(tex_T15, 0));
  vec2 g = uv * size - 0.5;
  ivec2 i0 = ivec2(floor(g));
  vec2 f = g - vec2(i0);
  vec4 sum = vec4(0.0);
  float wsum = 0.0;
  for (int k = 0; k < 4; k++) {
    ivec2 o = ivec2(k & 1, k >> 1);
    vec4 s = texelFetch(tex_T15, clamp(i0 + o, ivec2(0), ivec2(size) - 1), 0);
    float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
    if (s.a >= 0.0 && w > 0.0 && !any(isnan(s)) && !any(isinf(s))) {
      sum += s * w;
      wsum += w;
    }
  }
  return wsum > 1e-4 ? sum / wsum : vec4(0.0, 0.0, 0.0, -1.0);
}

// the game's own sky around a point, broadly: its average over a wide ring of sky pixels (its
// painted cloud cards drift through it: read pixel by pixel they printed their cut-out shapes into
// our clouds' color); our sky where none of the ring is sky
vec3 game_sky_around(vec2 uv, vec3 fallback, vec2 radius) {
  vec3 sum = vec3(0.0);
  float n = 0.0;
  for (int k = 0; k < 12; k++) {
    float a = float(k) * 2.39996;
    float r = k < 6 ? 0.45 : 1.0;
    vec2 q = clamp(uv + r * radius * vec2(cos(a), sin(a) * (texel.x / texel.y)), vec2(0.002), vec2(0.998));
    float zz = view_z(texture(tex_T3, q).r);
    if (zz == 0.0 || zz > 2800.0 * 4096.0) {
      sum += texture(tex_T0, q).rgb;
      n += 1.0;
    }
  }
  return n > 0.0 ? sum / n : fallback;
}

// the cloud pass: one pixel here covers 2x2 of the screen. Marched only where the sky shows in
// one of them, then blended into last frame's clouds, found where this direction was then (the
// clouds are far enough to ignore the camera's move): the march's noise averages out over frames.
vec4 cloud_march() {
  vec2 uv = tex_coord;
  vec3 ray = normalize(vec3((uv.x * 2.0 - 1.0) / proj.z, (uv.y * 2.0 - 1.0) / proj.w, 1.0));
  ivec2 base = ivec2(gl_FragCoord.xy) * 2;
  ivec2 top = textureSize(tex_T3, 0) - 1;
  bool sky = false;
  for (int k = 0; k < 4; k++) {
    float zz = view_z(texelFetch(tex_T3, min(base + ivec2(k & 1, k >> 1), top), 0).r);
    sky = sky || zz == 0.0 || zz > 2800.0 * 4096.0;
  }
  if (!sky || world_dir(ray).y < 0.015) {
    return vec4(0.0, 0.0, 0.0, -1.0);
  }
  // the sky behind them as the composite makes it (by night the game's own, read from the scene)
  vec3 s = sky_color(ray);
  // (at dawn and dusk too: the game's dim sky, not ours, or the clouds come out darker than it)
  float dim = max(clouds.w, 1.0 - smoothstep(0.08, 0.3, luma(s)));
  vec3 gs = dim > 0.0 ? game_sky_around(uv, s, vec2(0.22)) : s;
  vec4 cl = cloud_volume(ray, pixel_angle() / 6.2832, mix(s, mix(gs, s, 0.25), dim));
  if (cloud_info.y > 0.0) {
    vec3 pp = to_prev * ray;
    vec2 prev_uv = 0.5 * motion.xy * pp.xy / pp.z + 0.5;
    if (pp.z > 0.0 && all(greaterThan(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)))) {
      vec4 h = cloud_fetch(prev_uv);
      // (lightning changes them at once)
      float keep = cloud_info.z * (1.0 - 0.8 * clamp((weather2.w + sheet) * 4.0, 0.0, 1.0));
      if (h.a >= 0.0) {
        cl = mix(cl, h, keep);
      }
    }
  }
  // (a NaN marched once, looking straight at the sun, stayed in the history and spread a black
  // block over the sky as the camera turned: never keep one)
  if (any(isnan(cl)) || any(isinf(cl))) {
    cl = vec4(0.0, 0.0, 0.0, -1.0);
  }
  return cl;
}

// rain: real drops in the world. Each cell of a horizontal grid holds a column where drops fall
// (one every few meters, each a short streak, as a camera sees it); the view ray walks the cells
// it crosses (2D DDA) and meets each column where it passes closest. So the drops stay put in the
// world as the camera moves, the near ones pass by bigger, and the scene hides the ones behind it.
// o: the camera (m), d: the ray (world, unit), far: where the scene is along it (m), px: the angle
// of a pixel (radians). Returns how much rain covers the pixel.
float rain_grid(vec3 o, vec3 d, float near, float far, float cell, float px, float seed) {
  // the wind slants the rain: shear space so the drops' paths stand vertical in it
  vec2 slant = dust_wind.xy * (0.06 + 0.12 * weather2.x);
  o.xz -= slant * o.y;
  d.xz -= slant * d.y;
  vec2 dh = d.xz;
  float hl = length(dh);
  if (hl < 1e-4) {
    return 0.0;
  }
  vec2 start = o.xz + dh * near;
  vec2 c = floor(start / cell);
  vec2 stp = sign(dh);
  vec2 inv = 1.0 / max(abs(dh), vec2(1e-5));
  vec2 next = ((c + max(stp, 0.0)) * cell - o.xz) * stp * inv;  // ray t of the next cell edges
  float sum = 0.0;
  float t = near;
  float t_rain = weather.z;
  for (int i = 0; i < 28; i++) {
    if (t > far) {
      break;
    }
    float h = hash1(c + seed);
    // gusts: denser and sparser sheets of rain drifting with the wind
    vec2 cw = c * cell;
    float gust = 0.55 + 0.45 * smoothstep(-0.3, 0.4, gnoise(cw * 0.06 - dust_wind.xy * t_rain * 0.25));
    if (h < 0.92 * gust) {
      // the column, somewhere in the cell
      vec2 col = (c + 0.15 + 0.7 * vec2(fract(h * 13.7), fract(h * 71.3))) * cell;
      float tc = dot(col - o.xz, dh) / (hl * hl);  // closest along the ray
      if (tc > near && tc < far) {
        float miss = length(o.xz + dh * tc - col);
        float foot = tc * px;           // a pixel's size there (m)
        float width = max(0.002, foot * 0.7);
        float line = 1.0 - smoothstep(0.0, width, miss);
        if (line > 0.0) {
          // drops every 1.5 m down the column, falling 9 m/s, each 0.4 m long (motion blur)
          float y = o.y + d.y * tc;
          float v = fract((y + t_rain * 9.0) / 1.5 + fract(h * 37.1));
          float len = 0.35 / 1.5;
          float streak = smoothstep(0.0, len * 0.3, v) * (1.0 - smoothstep(len * 0.6, len, v));
          // thin far drops are faint, not thick
          // (not right at the lens: a drop there would cross the whole screen)
          sum += line * streak * min(1.0, 0.005 / width) * 3.0 * smoothstep(2.0, 3.5, tc);
        }
      }
    }
    // the next cell
    if (next.x < next.y) {
      t = next.x;
      next.x += cell * inv.x;
      c.x += stp.x;
    } else {
      t = next.y;
      next.y += cell * inv.y;
      c.y += stp.y;
    }
  }
  return sum;
}

float rain_drops(vec3 ray_view, float scene_z, float px) {
  vec3 d = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  // along the ray to the scene (m); the sky is far
  float far = scene_z > 0.0 ? scene_z / 4096.0 / max(ray_view.z, 1e-3) : 1e4;
  // near drops on a fine grid, farther ones on a coarser one
  return rain_grid(o, d, 2.0, min(far, 4.0), 0.25, px, 0.0) +
         rain_grid(o, d, 4.0, min(far, 12.0), 0.7, px, 17.0) * 0.9 +
         rain_grid(o, d, 12.0, min(far, 30.0), 1.6, px, 41.0) * 0.7;
}

// snowflakes: like the rain (columns in world cells, walked along the view ray), but round flakes
// drifting down at about a meter a second, swaying, carried by the wind
float snow_grid(vec3 o, vec3 d, float near, float far, float cell, float px, float seed) {
  float t_snow = weather.z;
  vec2 slant = dust_wind.xy * 0.35;
  o.xz -= slant * o.y;
  d.xz -= slant * d.y;
  vec2 dh = d.xz;
  float hl = length(dh);
  if (hl < 1e-4) {
    return 0.0;
  }
  vec2 start = o.xz + dh * near;
  vec2 c = floor(start / cell);
  vec2 stp = sign(dh);
  vec2 inv = 1.0 / max(abs(dh), vec2(1e-5));
  vec2 next = ((c + max(stp, 0.0)) * cell - o.xz) * stp * inv;
  float sum = 0.0;
  float t = near;
  for (int i = 0; i < 24; i++) {
    if (t > far) {
      break;
    }
    float h = hash1(c + seed);
    if (h < 0.85) {
      // the column sways as a whole (its flakes are far apart)
      vec2 sway = 0.18 * cell * vec2(sin(t_snow * 0.9 + h * 40.0), cos(t_snow * 0.7 + h * 23.0));
      vec2 col = (c + 0.2 + 0.6 * vec2(fract(h * 13.7), fract(h * 71.3))) * cell + sway;
      float tc = dot(col - o.xz, dh) / (hl * hl);
      if (tc > near && tc < far) {
        float miss = length(o.xz + dh * tc - col);
        float foot = tc * px;
        float r = max(0.012, foot * 0.9);
        if (miss < r) {
          // a flake every 0.9 m down the column, falling 1.1 m/s
          float y = o.y + d.y * tc;
          float dy = (fract((y + t_snow * 1.1) / 0.9 + fract(h * 37.1)) - 0.5) * 0.9;
          float dist = length(vec2(miss, dy));
          float flake = 1.0 - smoothstep(r * 0.4, r, dist);
          // far flakes smaller than a pixel are faint; none right at the lens
          sum += flake * min(1.0, (0.012 / r) * (0.012 / r) * 4.0) * smoothstep(1.0, 2.0, tc);
        }
      }
    }
    if (next.x < next.y) {
      t = next.x;
      next.x += cell * inv.x;
      c.x += stp.x;
    } else {
      t = next.y;
      next.y += cell * inv.y;
      c.y += stp.y;
    }
  }
  return sum;
}

// dust motes in the headlights' beams: specks hanging in the air in columns (as the snowflakes),
// drifting slowly, each lit only while it's inside the cone of the nearest vehicle's lamps,
// glinting as it tumbles. Returns how much lit mote covers the pixel
float mote_grid(vec3 o, vec3 d, float near, float far, float cell, float px, float seed) {
  float t_m = weather.z;
  vec2 dh = d.xz;
  float hl = length(dh);
  if (hl < 1e-4) {
    return 0.0;
  }
  mat3 to_view = transpose(view_to_world);
  vec2 start = o.xz + dh * near;
  vec2 c = floor(start / cell);
  vec2 stp = sign(dh);
  vec2 inv = 1.0 / max(abs(dh), vec2(1e-5));
  vec2 next = ((c + max(stp, 0.0)) * cell - o.xz) * stp * inv;
  float sum = 0.0;
  float t = near;
  for (int i = 0; i < 24; i++) {
    if (t > far) {
      break;
    }
    float h = hash1(c + seed);
    if (h < 0.7) {
      vec2 drift = 0.3 * cell * vec2(sin(t_m * 0.21 + h * 40.0), cos(t_m * 0.17 + h * 23.0)) +
                   dust_wind.xy * 0.15 * cell * sin(t_m * 0.3 + h * 9.0);
      vec2 col = (c + 0.2 + 0.6 * vec2(fract(h * 13.7), fract(h * 71.3))) * cell + drift;
      float tc = dot(col - o.xz, dh) / (hl * hl);
      if (tc > near && tc < far) {
        float miss = length(o.xz + dh * tc - col);
        float r = max(0.01, tc * px * 1.2);
        if (miss < r) {
          // a mote every 0.3 m up the column, bobbing a few centimeters
          float y = o.y + d.y * tc;
          float ph = (y + 0.05 * sin(t_m * 0.8 + h * 31.0)) / 0.3 + fract(h * 37.1);
          float dy = (fract(ph) - 0.5) * 0.3;
          float dist = length(vec2(miss, dy));
          float mote = 1.0 - smoothstep(r * 0.4, r, dist);
          // (only some places up the column hold a mote: no lines of them)
          if (hash1(c * 1.37 + vec2(floor(ph), seed)) > 0.45) {
            mote = 0.0;
          }
          if (mote > 0.0) {
            vec3 wp = vec3(col.x, y - dy, col.y);
            vec3 vp = to_view * (wp * 4096.0 - cam_pos);
            float lit = 0.0;
            for (int s = 0; s < min(spot_count, 2); s++) {
              vec3 q = vp - spot_pos[s].xyz;
              float dq = length(q);
              float dm = dq / 4096.0;
              lit = max(lit, smoothstep(0.88, 0.97, dot(q / max(dq, 1.0), spot_dir[s].xyz)) * spot_pos[s].w /
                                 (1.0 + dm * dm / 40.0) * smoothstep(0.4, 1.5, dm));
            }
            float glint = 0.55 + 0.45 * sin(t_m * (2.0 + 3.0 * fract(h * 5.3)) + floor(ph) * 7.1 + h * 50.0);
            // specks smaller than a pixel are faint
            sum += mote * lit * glint * min(1.0, (0.01 / r) * (0.01 / r) * 4.0);
          }
        }
      }
    }
    if (next.x < next.y) {
      t = next.x;
      next.x += cell * inv.x;
      c.x += stp.x;
    } else {
      t = next.y;
      next.y += cell * inv.y;
      c.y += stp.y;
    }
  }
  return sum;
}

float beam_motes(vec3 ray_view, float scene_z, float px) {
  vec3 d = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  float far = scene_z > 0.0 ? scene_z / 4096.0 / max(ray_view.z, 1e-3) : 1e4;
  return mote_grid(o, d, 0.8, min(far, 8.0), 0.35, px, 11.0) +
         mote_grid(o, d, 8.0, min(far, 22.0), 0.8, px, 37.0);
}

float snow_flakes(vec3 ray_view, float scene_z, float px) {
  vec3 d = world_dir(ray_view);
  vec3 o = cam_pos / 4096.0;
  float far = scene_z > 0.0 ? scene_z / 4096.0 / max(ray_view.z, 1e-3) : 1e4;
  return snow_grid(o, d, 1.0, min(far, 6.0), 0.45, px, 5.0) +
         snow_grid(o, d, 6.0, min(far, 18.0), 1.1, px, 29.0) * 0.8 +
         snow_grid(o, d, 18.0, min(far, 45.0), 2.4, px, 53.0) * 0.6;
}

// raindrops in the puddles: each 0.5 m cell gets a ring now and then, spreading and fading
// (returns the slope it gives the water, xz)
// how much of a puddle is at a world point (m), as the wet ground draws them (flat ground)
float puddle_at(vec2 xz) {
  float pd = gnoise(xz * 0.22) + 0.45 * gnoise(xz * 0.7 + 7.1) + 0.12 * gnoise(xz * 3.1 + 2.3);
  float spread = 0.22 * weather2.x - 0.3 * (1.0 - puddle_level);
  return smoothstep(-0.12 - spread, 0.02 - spread, pd) * max(weather.w, puddle_level);
}

// rings on the puddles where Jak stepped: spreading 0.8 m/s for a second (slope, xz)
vec2 step_ripples(vec2 xz) {
  vec2 slope = vec2(0.0);
  for (int i = 0; i < splash_count; i++) {
    float strength = floor(splash[i].w / 10.0);
    float age = splash[i].w - 10.0 * strength;
    vec2 rel = xz - splash[i].xz / 4096.0;
    float d = length(rel);
    for (int k = 0; k < 4; k++) {
      float a = age - float(k) * 0.12;
      if (a <= 0.0) {
        continue;
      }
      float ring = d - a * (0.8 + 0.3 * strength);
      float band = exp(-ring * ring / 0.0012) * (1.0 - smoothstep(0.3, 1.1, a));
      slope += rel / max(d, 1e-3) * band * sin(ring * 90.0) * 0.38 * strength;
    }
  }
  return slope;
}

// the drops a step throws up from a puddle: ten (twenty landing) small drops on short arcs,
// hidden behind the scene. Returns how much drop covers the pixel
float step_drops(vec2 uv, float scene_z) {
  float sum = 0.0;
  mat3 to_view = transpose(view_to_world);
  for (int i = 0; i < splash_count; i++) {
    float strength = floor(splash[i].w / 10.0);
    float age = splash[i].w - 10.0 * strength;
    if (age > 0.55) {
      continue;
    }
    vec3 foot = splash[i].xyz / 4096.0;  // m
    // (a raindrop's bounce, strength 0: anywhere in the rain)
    bool rain_drop = strength < 0.5;
    float wet_here = rain_drop ? 1.0 : puddle_at(foot.xz);
    if (wet_here < 0.2) {
      continue;
    }
    // skip what's far from this pixel on screen
    vec3 cv = to_view * (splash[i].xyz - cam_pos);
    if (cv.z < 0.5 * 4096.0) {
      continue;
    }
    vec2 cuv = 0.5 + 0.5 * vec2(cv.x * proj.z, cv.y * proj.w) / cv.z;
    float reach = 1.2 * 4096.0 * abs(proj.w) / cv.z;  // ~1.2 m in uv (height)
    if (abs(uv.y - cuv.y) > reach || abs(uv.x - cuv.x) > reach * abs(proj.z / proj.w)) {
      continue;
    }
    int n = rain_drop ? 3 : (strength > 1.5 ? 20 : 10);
    for (int j = 0; j < n; j++) {
      float h1 = hash1(vec2(float(j), splash[i].x + 0.37));
      float h2 = hash1(vec2(splash[i].z, float(j) * 1.7));
      float h3 = hash1(vec2(float(j) * 3.1, splash[i].y));
      float ang = (float(j) + h1) * 6.2832 / float(n);
      float speed = rain_drop ? 0.15 + 0.3 * h2 : (0.5 + 0.9 * h2) * (0.7 + 0.3 * strength);
      float up = rain_drop ? 0.6 + 0.7 * h3 : (1.4 + 1.2 * h3) * (0.8 + 0.2 * strength);
      float t = age;
      vec3 wp = foot + vec3(cos(ang) * speed * t, up * t - 4.9 * t * t, sin(ang) * speed * t);
      if (wp.y < foot.y - 0.02) {
        continue;  // back in the water
      }
      vec3 v = to_view * (wp * 4096.0 - cam_pos);
      if (v.z < 0.3 * 4096.0 || (scene_z > 0.0 && v.z > scene_z + 0.05 * 4096.0)) {
        continue;
      }
      vec2 duv = 0.5 + 0.5 * vec2(v.x * proj.z, v.y * proj.w) / v.z;
      vec2 dd = (uv - duv) / texel;
      // a drop of ~2 cm, at least a pixel and a bit, a little stretched along its fall
      float rad = max(rain_drop ? 1.0 : 1.3,
                      (rain_drop ? 0.01 : 0.018) * 4096.0 * abs(proj.w) * 0.5 / v.z / texel.y);
      float k = 1.0 - smoothstep(rad * 0.5, rad, length(vec2(dd.x, dd.y * 0.6)));
      sum += k * (1.0 - smoothstep(0.3, 0.55, age)) * min(1.0, wet_here * 2.0);
    }
  }
  return min(sum, 1.0);
}

vec2 rain_ripples(vec2 xz, float t) {
  vec2 slope = vec2(0.0);
  vec2 cell = floor(xz / 0.5);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 cc = cell + vec2(i, j);
      float h = hash1(cc);
      vec2 center = (cc + 0.2 + 0.6 * vec2(h, fract(h * 23.1))) * 0.5;
      float cycle = t * 1.3 + h * 7.0;
      float phase = fract(cycle);
      // not every cell every time
      if (hash1(cc + floor(cycle) * 1.37) > 0.45) {
        continue;
      }
      vec2 rel = xz - center;
      float d = length(rel);
      float ring = d - phase * 0.3;
      float band = exp(-ring * ring / 0.0005) * (1.0 - phase) * (1.0 - phase);
      slope += rel / max(d, 1e-3) * band * sin(ring * 140.0) * 0.5;
    }
  }
  return slope;
}

// the little crowns the drops of rain_ripples throw up as they land, at the start of each ring
// (same cells and timing): how much splash is at a point. radius in m (bigger far away, so it
// still covers a pixel)
float rain_splashes(vec2 xz, float t, float radius) {
  float sum = 0.0;
  vec2 cell = floor(xz / 0.5);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 cc = cell + vec2(i, j);
      float h = hash1(cc);
      vec2 center = (cc + 0.2 + 0.6 * vec2(h, fract(h * 23.1))) * 0.5;
      float cycle = t * 1.3 + h * 7.0;
      float phase = fract(cycle);
      if (phase > 0.14 || hash1(cc + floor(cycle) * 1.37) > 0.45) {
        continue;
      }
      vec2 rel = xz - center;
      sum += exp(-dot(rel, rel) / (radius * radius)) * (1.0 - phase / 0.14);
    }
  }
  return sum;
}

// distant rain hitting a surface: one short sparkle per cell for each drop, the cells growing
// with distance (powers of two, blended) so the drops stay about a pixel big and as many on screen
float rain_spark_level(vec2 xz, float t, float cs, float r) {
  float sum = 0.0;
  vec2 cell = floor(xz / cs);
  for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
      vec2 cc = cell + vec2(i, j);
      float h = hash1(cc + cs * 17.0);
      vec2 center = (cc + 0.2 + 0.6 * vec2(h, fract(h * 23.1))) * cs;
      float cycle = t * 1.6 + h * 7.0;
      float phase = fract(cycle);
      if (phase > 0.12 || hash1(cc + floor(cycle) * 1.37 + cs) > 0.55) {
        continue;
      }
      vec2 rel = xz - center;
      sum += exp(-dot(rel, rel) / (r * r)) * (1.0 - phase / 0.12);
    }
  }
  return sum;
}

// across: the pixel's width there (m)
float rain_sparkles(vec2 xz, float t, float across) {
  float r = max(across, 0.02);
  float lvl = max(log2(r * 4.5 / 0.25), 0.0);
  float l0 = floor(lvl);
  float cs = 0.25 * exp2(l0);
  return mix(rain_spark_level(xz, t, cs, r), rain_spark_level(xz, t, cs * 2.0, r), lvl - l0);
}

// FXAA (Lottes' compact version): blend along the edge direction where the luma jumps
vec3 fxaa_at(vec2 uv) {
  vec3 m = texture(tex_T0, uv).rgb;
  vec3 nw = texture(tex_T0, uv + vec2(-1.0, -1.0) * texel).rgb;
  vec3 ne = texture(tex_T0, uv + vec2(1.0, -1.0) * texel).rgb;
  vec3 sw = texture(tex_T0, uv + vec2(-1.0, 1.0) * texel).rgb;
  vec3 se = texture(tex_T0, uv + vec2(1.0, 1.0) * texel).rgb;
  float lm = luma(m), lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se);
  float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));
  float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
  if (lmax - lmin < max(0.04, lmax * 0.12)) {
    return m;  // no edge
  }
  vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
  float reduce = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);
  float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcp_min, vec2(-8.0), vec2(8.0)) * texel;
  vec3 a = 0.5 * (texture(tex_T0, uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                  texture(tex_T0, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 b = a * 0.5 + 0.25 * (texture(tex_T0, uv - dir * 0.5).rgb + texture(tex_T0, uv + dir * 0.5).rgb);
  float lb = luma(b);
  return (lb < lmin || lb > lmax) ? a : b;
}

// the occlusion and the characters' sun shadows, made at half the resolution a side: of the four
// texels around, those at this pixel's depth (plain bilinear blurred them across the edges of
// grass blades and pebbles, and the TAA's jitter moved those edges each frame: they flickered)
float ao_upsample(vec2 uv, float zc) {
  vec2 size = vec2(textureSize(tex_T2, 0));
  vec2 g = uv * size - 0.5;
  ivec2 i0 = ivec2(floor(g));
  vec2 f = g - vec2(i0);
  if (zc <= 0.0) {
    return texture(tex_T2, uv).r;
  }
  float sum = 0.0;
  float wsum = 0.0;
  for (int k = 0; k < 4; k++) {
    ivec2 o = ivec2(k & 1, k >> 1);
    ivec2 ic = clamp(i0 + o, ivec2(0), ivec2(size) - 1);
    vec2 tuv = (vec2(ic) + 0.5) / size;
    float zs = view_z(texture(tex_T3, tuv).r);
    float bw = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
    float dz = abs(zs - zc) / (zc * 0.02 + 0.05 * 4096.0);
    float w = (bw + 1e-3) / (1.0 + dz * dz * 16.0);
    sum += texelFetch(tex_T2, ic, 0).r * w;
    wsum += w;
  }
  return sum / wsum;
}

// the terrain's pictures without their repeats showing (Inigo Quilez's "texture repetition" 3): a
// slow noise over the picture picks one of 8 offsets of it, two neighbouring ones blended across
// the change, where they differ least. anti_tile 0..1 (the editor's terrain window)
uniform float anti_tile;
float notile_noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
  float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
  float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
  float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
vec4 notile(sampler2D s, vec2 uv, float lod) {
  if (anti_tile <= 0.0) {
    return textureLod(s, uv, lod);
  }
  float l = notile_noise(uv * 0.37) * 8.0;
  float ia = floor(l), f = fract(l);
  vec2 offa = sin(vec2(3.0, 7.0) * ia), offb = sin(vec2(3.0, 7.0) * (ia + 1.0));
  vec4 a = textureLod(s, uv + offa, lod), b = textureLod(s, uv + offb, lod);
  vec4 c = mix(a, b, smoothstep(0.2, 0.8, f - 0.1 * dot(a.rgb - b.rgb, vec3(1.0))));
  return anti_tile >= 0.999 ? c : mix(textureLod(s, uv, lod), c, anti_tile);
}

// the rocky ground's made-up relief (m): rounded lumps of stone (smooth noise, a half meter and
// more); the rock picture's brightness a faint crust on them (as the main height its cracks
// crossed into a hatching)
// (with the ground picture: its stones and pebbles, lighter on top, the relief)
float ground_height(vec2 xz, float glod, float plod) {
  if (ground_detail > 0.0) {
    vec2 gq = mat2(0.87, 0.5, -0.5, 0.87) * xz / 2.2;
    vec2 pq = mat2(0.6, -0.8, 0.8, 0.6) * xz / 8.5 + 0.29;
    return luma(notile(tex_T16, gq, glod).rgb) * 0.035 + luma(notile(tex_T16, pq, plod).rgb) * 0.04 +
           gnoise(xz / 1.4 + 3.3) * 0.1;
  }
  vec2 gq = mat2(0.87, 0.5, -0.5, 0.87) * xz / 0.7;
  vec2 pq = mat2(0.6, -0.8, 0.8, 0.6) * xz / 3.1 + 0.29;
  return luma(notile(tex_T10, gq, glod).rgb) * 0.008 + luma(notile(tex_T10, pq, plod).rgb) * 0.015 +
         smoothstep(-0.2, 0.4, gnoise(xz / 0.45 + 1.9)) * 0.05 + gnoise(xz / 1.4 + 3.3) * 0.12;
}

// (no const arrays in this shader: one of them, indexed in a loop, slows the whole shader on
// NVIDIA's driver, even when that code doesn't run)
// a fire's place on screen, in uv, and the flame's up there, one flame size long
vec2 fire_uv(int i) {
  vec3 fp = fire_pos[i].xyz;
  return vec2(fp.x * proj.z / fp.z, fp.y * proj.w / fp.z) * 0.5 + 0.5;
}

vec2 fire_axis(int i) {
  vec3 top = fire_pos[i].xyz + up_view * fire_pos[i].w;
  return vec2(top.x * proj.z / top.z, top.y * proj.w / top.z) * 0.5 + 0.5 - fire_uv(i);
}

float flare_ghost(vec2 at, float aspect, float rad) {
  float d = length((tex_coord - at) * vec2(aspect, 1.0));
  return (1.0 - smoothstep(rad * 0.5, rad, d)) * 0.07;
}

// how much of the sun the shadow map blocks here (the rock's light below asks)
float rock_sh = 0.0;
float chr_bounce = 0.0;  // (a character's shade lit back by the ground: the sun's height, 0..1)
// what the levels drew over the game's sky here (tex_T25: the sky as the sky bucket drew it; the
// scene compared before the sprites when that copy is there)
vec3 sky_over(vec2 q) {
  vec3 g = sky_copy > 1.5 ? textureLod(tex_T23, q, 0.0).rgb : textureLod(tex_T0, q, 0.0).rgb;
  return g - textureLod(tex_T25, q, 0.0).rgb;
}
void main() {
  if (cloud_pass == 1) {
    color = cloud_march();
    return;
  }
  vec2 uv = tex_coord;
  float d = texture(tex_T3, uv).r;
  float z = view_z(d);
  vec3 p = d > 1e-7 ? view_pos(uv, d) : vec3(0.0);
  // the view ray, and the angle a pixel spans
  vec3 ray = normalize(vec3((tex_coord.x * 2.0 - 1.0) / proj.z, (tex_coord.y * 2.0 - 1.0) / proj.w, 1.0));
  float pixel_rad = 2.0 * texel.y / abs(proj.w);
  float height = dot(up_view, p);  // above (+) or below (-) the camera
  // is the scene level here (a floor, a decal lying on the water)? (uniform control flow)
  vec3 scene_dp = view_pos(uv, max(d, 1e-6));
  bool scene_level = abs(dot(normalize(cross(dFdx(scene_dp), dFdy(scene_dp))), up_view)) > 0.9;

  // water: the near sea's surface in front of the scene, or the far sea still showing. From here
  // on the surface is this pixel's depth (fog, blur, ...); seabed_d keeps what's under it.
  float wsurf = 0.0;
  float seabed_d = d;
  bool water_deep = true;
  bool water_decal = false;  // something flat lying on the water (splash and ripple discs)
  if (water_strength > 0.0) {
    float wd = texture(tex_T5, uv).r;
    float fd = texture(tex_T6, uv).r;
    bool under_is_sea = fd > 1e-7 && abs(fd - d) < 1e-7 && dot(up_view, view_pos(uv, fd)) < -4096.0;
    // (where the scene's depth changed after the sea was drawn, something drawn later covers it,
    // as the game shows it: Spargus palace's floor, under the sea's level)
    bool covered = abs(texture(tex_T14, uv).r - d) > 1e-7;
    if (wd > d + 1e-7 && !covered) {
      wsurf = wd;
      water_deep = under_is_sea || d <= 1e-7;
    } else if (under_is_sea) {
      wsurf = fd;
    } else {
      // a flat effect a few cm over the water (splashes draw see-through discs that write depth):
      // still water, with the game's color only where it's brighter (the foam)
      // (the near sea only: the far one is a plane under the whole level, docks included)
      float ws = wd;
      // (level and up to 40 cm over it, or anything within 12 cm: the discs' edges; the sea must
      // be drawn behind it: where something solid hid the sea, its depth is the scene's own)
      if (ws > 1e-7 && d > ws + 1e-7) {
        float above = dot(up_view, p - view_pos(uv, ws));
        if (above > 0.0 && above < (scene_level ? 0.4 : 0.12) * 4096.0) {
          wsurf = ws;
          water_decal = true;
        }
      }
    }
    if (wsurf > 0.0) {
      d = wsurf;
      z = view_z(d);
      p = view_pos(uv, d);
      height = dot(up_view, p);
    }
  }
  // explosions: a shockwave ring racing out from each one bends the picture for half a second
  for (int i = 0; i < blast_count; i++) {
    vec3 bp = blast_pos[i].xyz;
    float age = blast_age[i];
    float ring = blast_pos[i].w * (1.0 + 14.0 * age);  // game units
    vec2 cuv = vec2(bp.x * proj.z / bp.z, bp.y * proj.w / bp.z) * 0.5 + 0.5;
    vec2 size = abs(vec2(proj.z, proj.w)) * 0.5 * ring / bp.z;  // the ring's radii in uv
    vec2 rel = tex_coord - cuv;
    float r = length(rel / size);
    float band = exp(-sq((r - 1.0) / 0.12)) * (1.0 - smoothstep(0.1, 0.5, age));
    // a gun's blast: only the arc in front of the muzzle (as seen on screen; all round when it's
    // fired toward or away from the camera)
    if (blast_dir[i].w > 0.5) {
      vec3 ahead = bp + blast_dir[i].xyz * 4096.0;
      vec2 auv = vec2(ahead.x * proj.z / ahead.z, ahead.y * proj.w / ahead.z) * 0.5 + 0.5;
      vec2 sd = (auv - cuv) / size;
      float slen = length(sd);
      float front = dot(normalize(rel / size + 1e-6), sd / max(slen, 1e-6));
      // (a charged shot: a much wider arc)
      float arc = blast_dir[i].w > 2.5 ? -0.5 : 0.2;
      band *= mix(1.0, smoothstep(arc, arc + 0.5, front), clamp(slen * 3.0, 0.0, 1.0));
    }
    if (abs(blast_dir[i].w - 2.0) < 0.5) {
      continue;  // charging: no wave, no hot air
    }
    if (band > 0.001) {
      uv += normalize(rel + 1e-6) * band * 0.012 * blast_strength;
    }
    // then the hot air over the blast wobbles for a second
    if (blast_sparks > 0.0 && age < 1.3) {
      vec2 hsize = abs(vec2(proj.z, proj.w)) * 0.5 * blast_pos[i].w * 2.5 / bp.z;
      vec2 hr = rel / hsize;
      float heat = exp(-dot(hr, hr) * 1.5) * (1.0 - age / 1.3) * smoothstep(0.0, 0.15, age);
      if (heat > 0.001) {
        float ht = haze.w;
        uv += vec2(sin(hr.y * 14.0 - ht * 11.0), cos(hr.x * 12.0 - ht * 9.0)) * heat * 0.004 *
              blast_sparks;
      }
    }
  }

  // fires: the hot air over each flame bends what's seen through it
  for (int i = 0; i < fire_count; i++) {
    vec3 fp = fire_pos[i].xyz;
    float fs = fire_pos[i].w;
    vec3 top = fp + up_view * fs;
    if (fp.z < 0.5 * 4096.0 || top.z < 0.5 * 4096.0 || fire.x <= 0.0) {
      continue;
    }
    vec2 a = fire_axis(i);
    float aa = max(dot(a, a), 1e-10);
    vec2 rel = tex_coord - fire_uv(i);
    float along = dot(rel, a) / aa;                      // flame sizes up
    float across = (rel.x * a.y - rel.y * a.x) / aa;     // and sideways
    // (from the flame's top up: over the flame itself it bent the flame away, see-through and
    // tinted by what's behind it)
    float column = exp(-across * across * 1.2) * smoothstep(1.0, 1.8, along) *
                   (1.0 - smoothstep(1.5, 4.5, along));
    // only what's behind the fire
    if (column > 0.0 && (z == 0.0 || z > fp.z - fs)) {
      float t = fire.z;
      vec2 wob = vec2(sin(along * 9.0 - t * 9.0 + across * 3.0) + 0.5 * sin(along * 21.0 - t * 14.0),
                      cos(along * 7.0 - t * 8.0 + across * 5.0));
      uv += wob * length(a) * fire.x * column;
    }
  }

  // drops on the lens in a downpour: beads land now and then, slide down a little and dry; each
  // bends what's behind it like a small lens, upside down (two sizes, cells across the screen)
  if (lens_drops > 0.0 && weather.x > 0.25) {
    float amt = lens_drops * smoothstep(0.25, 0.7, weather.x);
    float aspect = texel.y / texel.x;
    for (int layer = 0; layer < 2; layer++) {
      float cell = layer == 0 ? 0.12 : 0.07;
      vec2 s = vec2(tex_coord.x * aspect, tex_coord.y) / cell + float(layer) * 7.3;
      vec2 id = floor(s);
      vec2 f = fract(s);
      vec2 h = hash2(id + 17.0 * float(layer)) * 0.5 + 0.5;
      float age = fract(weather.z / (3.0 + 3.0 * h.y) + h.x);
      if (fract(h.x * 13.7 + floor(weather.z / (3.0 + 3.0 * h.y) + h.x) * 0.618) > 0.45 * amt) {
        continue;
      }
      vec2 ctr = vec2(0.3 + 0.4 * h.y, 0.75 - 0.5 * age * age * age);
      float r = (0.1 + 0.12 * h.x) * (1.0 - age * age * age * age);
      vec2 d = (f - ctr) * vec2(1.0, 1.25);  // (a little taller than wide, as it slides)
      float dl = length(d);
      if (dl < r) {
        float k = 1.0 - dl / r;
        uv -= d / vec2(aspect, 1.0) * cell * (1.6 + 1.2 * k) * smoothstep(0.0, 0.25, k);
      }
    }
  }

  // the light effects (tex_T23/T24): what the sprite buckets added here, by day
  vec3 fx_d = vec3(0.0);
  float fx_w = 0.0;
  float fx_under = 0.0;  // the scene's brightness under them
  vec3 fx_all = vec3(0.0);  // all the sprites did here (fx_rest below)
  if (fx_day > 0.0) {
    vec3 f0 = textureLod(tex_T23, uv, 0.0).rgb;
    vec3 f1 = textureLod(tex_T24, uv, 0.0).rgb;
    fx_all = f1 - f0;
    fx_d = max(f1 - f0, 0.0);
    float fm = max(fx_d.r, max(fx_d.g, fx_d.b));
    // (the surroundings looked up only where there is an effect: most of the screen has none, and
    // the four wide reads cost ~0.3 ms there for nothing)
    if (fm > tune_fx_min) {
    // (a channel the sum clipped at 1 lost part of the effect's light: an orange flame over the
    // sand kept less red than green and turned green-yellow. Such a channel had at least as much as
    // the strongest one that wasn't clipped: hot cores go pale, as they do at night)
    vec3 clip = smoothstep(vec3(0.94), vec3(0.99), f1);
    fx_d = mix(fx_d, max(fx_d, vec3(fm)), clip);
    // (only what stands out from its surroundings: a shot, a thruster, a flame, sparks. A wash of
    // light over a whole room (the arena's sun pool) has about as much all around, and stays in
    // the scene's grade as before. The mean around this pixel, over a sixth of the screen's height)
    float lod = log2(max(1.0, 1.0 / (texel.y * tune_fx_area_frac)));
    float wide = max(luma(max(textureLod(tex_T24, uv, lod).rgb - textureLod(tex_T23, uv, lod).rgb, 0.0)), 0.0);
    // (by the ratio, and by the amount itself: the edges of a wash stand out from its middle)
    float wider = max(luma(textureLod(tex_T24, uv, lod + tune_fx_wash_lod).rgb -
                           textureLod(tex_T23, uv, lod + tune_fx_wash_lod).rgb), 0.0);
    float compact = (1.0 - smoothstep(tune_fx_area0, tune_fx_area1, wide / max(luma(fx_d), 0.02))) *
                    (1.0 - smoothstep(tune_fx_wash0, tune_fx_wash1, max(wide, wider)));
    // (blended dust and smoke are no light: a tan puff over darker ground made a warm difference, and
    // with the gain and the evening's grade it glowed as an orange orb. The sprite renderer takes
    // their coverage out of the copy's alpha (EnhancedSpriteFx::mark_blended): they stay in the scene)
    float dust = fx_mark > 0.5 ? smoothstep(tune_fx_dust0, tune_fx_dust1, 1.0 - textureLod(tex_T24, uv, 0.0).a) : 0.0;
    fx_w = fx_day * smoothstep(tune_fx_min, tune_fx_min * 4.0, fm) * compact * (1.0 - dust);
    fx_under = luma(f0);
    }
  }
  // (test: tune_fx_ab = s, off and on in turn every that many seconds, a corner mark tells which)
  bool fx_ab_off = tune_fx_ab > 0.0 && mod(floor(haze.w / tune_fx_ab), 2.0) < 0.5;
  if (fx_ab_off) {
    fx_w = 0.0;
  }
  // (not on the sky: the sun's and moon's glows go back over our sky as the game drew them. With the
  // gain the sun was a saturated purple diamond, its quad's edge where the weight cut in)
  if (tune_fx_sky > 0.5 && (z == 0.0 || z > 2800.0 * 4096.0)) {
    fx_w = 0.0;
  }
  // (what the sprites did that stays in the scene: smoke darkening the sky, washes of light. Our
  // sky replaces the game's by day and took them away with it: put back over ours)
  vec3 fx_rest = fx_all - fx_w * fx_d;

  // (the flames' place on screen: kept out of the daylight's corrections and the highlights'
  // bleaching further down, which took their orange to a pale yellow, green and blue by day)
  float flame_keep = 0.0;
  for (int i = 0; i < fire_count; i++) {
    float aa = dot(fire_axis(i), fire_axis(i));
    if (fire_pos[i].z < 1.0 * 4096.0 || aa <= 1e-10) {
      continue;
    }
    vec2 rel = tex_coord - fire_uv(i);
    float along = dot(rel, fire_axis(i)) / aa;
    float across = (rel.x * fire_axis(i).y - rel.y * fire_axis(i).x) / aa;
    flame_keep = max(flame_keep, (1.0 - smoothstep(0.7, 1.3, abs(across))) *
                                     smoothstep(-1.4, -0.8, along) * (1.0 - smoothstep(1.6, 2.4, along)));
  }  // (the flame's own pixels only, by the game's colors: a sprite with no depth of its own, it was
  // shaded as the trunk or the wall behind it, in its shadow: green-blue and dim)
  vec3 flame_raw = texture(tex_T0, tex_coord).rgb;
  flame_keep *= smoothstep(0.3, 0.5, luma(flame_raw));
  // (and the fires the game doesn't report (the desert's torches aren't particle spawners): by
  // their color, yellow to white hot and brighter than anything sunlit (red at its full: the
  // brightest grass in the noon sun stops short of it). Not over the land: the sand's sunlit patches are as pale; in front of a trunk
  // or a wall the flame took its depth, its shadow and its occlusion)
  // (this pixel's normal and the length its material wrote it at, for everything below: read once)
  vec4 pix_normal = gbuf(tex_coord);
  float pix_tag = length(pix_normal.rgb * 2.0 - 1.0);
  {
    float ft = pix_tag;
    // (nor over grass and bushes, shrub.frag's 0.97: noon burns the grass's pale tips as white)
    float over_land = ((ft > 0.4 && ft < 0.95) || (ft > 0.955 && ft < 0.983)) ? 1.0 : 0.0;
    flame_keep = max(flame_keep, smoothstep(0.85, 0.92, luma(flame_raw)) * smoothstep(0.94, 0.985, flame_raw.r) * (1.0 - over_land) *
                                     step(flame_raw.b, flame_raw.g + 0.02) * step(flame_raw.g, flame_raw.r + 0.02) *
                                     step(0.9 * flame_raw.r, flame_raw.g) *
                                     // (by day not the game's own sun, which has these colors: its glow on the sky, and on what
                                     // stands toward it, kept its raw yellow as a hard-edged patch)
                                     (1.0 - max(z == 0.0 ? 1.0 : 0.0, smoothstep(0.3, 0.6, dot(ray, sky_sun.xyz))) * (1.0 - clouds.w)));
  }
  // (what flame_keep puts back: without the effects' light, added after the grade like the rest)
  vec3 flame_base = max(flame_raw - fx_w * fx_d, 0.0);

  // the water's world position and pixel footprint, here in uniform control flow (derivatives)
  vec2 water_world = vec2(dot(right_view, p) + cam_pos.x, dot(fwd_view, p) + cam_pos.z) / 4096.0;
  float water_foot = clamp(length(fwidth(water_world)), 0.0005, 50.0);

  // the surface under the water: is it level (a beach) or upright (a leg, a wall)?
  vec3 seabed_p = view_pos(uv, max(seabed_d, 1e-6));
  vec3 seabed_n = normalize(cross(dFdx(seabed_p), dFdy(seabed_p)));
  bool seabed_level = abs(dot(seabed_n, up_view)) > 0.75;

  // heat haze: far ground below the camera wobbles a little
  if (haze.x > 0.0 && z > 0.0) {
    float amount = smoothstep(haze.y, haze.z, z) * clamp(-height / (z * 0.05 + 1.0), 0.0, 1.0);
    if (amount > 0.0) {
      float t = haze.w;
      vec2 wob = vec2(sin(uv.y * 180.0 + t * 3.1) + 0.5 * sin(uv.y * 410.0 - t * 4.3),
                      cos(uv.x * 140.0 + t * 2.3));
      uv += wob * haze.x * amount;
    }
  }

  // hot air over lava: the picture ripples, rising, stronger above the glow of the lava (the pixels
  // a little below this one)
  if (snow_heat.y > 0.0 && z > 0.0) {
    vec3 below = texture(tex_T0, uv - vec2(0.0, 0.035)).rgb;
    float lava = smoothstep(0.12, 0.35, below.r - below.b) * smoothstep(0.45, 0.85, below.r);
    float amount = snow_heat.y * smoothstep(2.0 * 4096.0, 15.0 * 4096.0, z) * (0.3 + 0.7 * lava);
    if (amount > 0.0) {
      float t = haze.w;
      vec2 wob = vec2(sin(uv.y * 230.0 - t * 6.0 + 2.0 * sin(uv.x * 70.0)),
                      sin(uv.x * 160.0 + uv.y * 90.0 - t * 5.0));
      uv += wob * 0.0018 * amount;
    }
  }

  // camera motion blur: where this pixel was last frame, if the world held still
  float blur = 0.0;
  vec3 c = vec3(0.0);
  if (motion.z > 0.0 && z > motion_keep.x && wsurf == 0.0) {
    vec3 pp = to_prev * p + to_prev_offset;
    vec2 prev_uv = 0.5 * vec2(motion.x * pp.x, motion.y * pp.y) / pp.z + 0.5;
    vec2 vel = (uv - prev_uv) * motion.z * smoothstep(motion_keep.x, motion_keep.y, z);
    float len = length(vel);
    if (len > motion.w) {
      vel *= motion.w / len;
      len = motion.w;
    }
    blur = clamp(len / (texel.x * 3.0), 0.0, 1.0);
    if (blur > 0.0) {
      vec3 sum = vec3(0.0);
      for (int i = 0; i < 8; i++) {
        sum += texture(tex_T0, uv + vel * (float(i) / 7.0 - 0.5)).rgb;
      }
      c = sum / 8.0;
    }
  }
  if (blur == 0.0) {
    c = fxaa != 0 ? fxaa_at(uv) : texture(tex_T0, uv).rgb;
  }
  // a hit (and, faintly, Dark Jak): the colors part toward the screen's edges
  if (chroma > 0.002 && wsurf == 0.0) {
    vec2 from_mid = uv - 0.5;
    vec2 off = from_mid * dot(from_mid, from_mid) * 4.0 * chroma * 0.012;
    c.r = texture(tex_T0, uv + off).r;
    c.b = texture(tex_T0, uv - off).b;
  }

  // water: our own shading over the game's, the same for the near and far sea (drawn by two
  // renderers with different colors), with reflections, sun glints, clearer shallows and foam
  if (wsurf > 0.0) {
    // ripples: the slope of a non-repeating noise height field (finite differences)
    vec2 world = water_world;
    float t = haze.w;
    float far = 1.0 / (1.0 + z / (140.0 * 4096.0));
    float e = 0.08;
    float h0 = water_height(world, t, water_foot);
    vec2 slope = vec2(water_height(world + vec2(e, 0.0), t, water_foot) - h0,
                      water_height(world + vec2(0.0, e), t, water_foot) - h0) / e;

    // how deep the water is here (meters), from the sea bed seen through it (near sea only)
    float depth_m = 40.0;
    float depth_real = 40.0;  // (not faded out: the caustics and clearness fade by near_fade instead)
    float near_fade = 1.0;
    if (!water_deep && seabed_d > 1e-7) {
      depth_m = max(dot(up_view, p - seabed_p), 0.0) / 4096.0;
      depth_real = depth_m;
      // (going out toward where the far sea takes over, as deep as it: no seam between the two. The
      // game draws the near sea only on the 24 m cells around the camera's own, and none from high
      // up: by the horizontal distance and the height over it, gone before its edge, or the clear
      // shallows and their caustics popped in and out over the beaches and creeks)
      float wdist = length(vec2(dot(right_view, p), dot(fwd_view, p))) / 4096.0;
      float whigh = max(-height, 0.0) / 4096.0;
      // (the clearness itself fades, evenly: fading the depth instead, exp() made it all happen in a
      // few meters, a visible arc around the camera)
      near_fade = max(smoothstep(8.0, 22.0, wdist), smoothstep(6.0, 14.0, whigh));
      depth_m = mix(depth_m, 40.0, near_fade);
    }
    // a pond: murky, the bed gone a few cm down, no swell or caustics (in the desert: the oases'
    // water, not the sea around it, which lies at 9 m)
    bool pond_here = pond != 0 && abs((dot(up_view, p) + cam_pos.y) / 4096.0 - 9.0) > 0.4;
    // (the sea's shallows only half clear: fully clear, they stood out bright and pale against the
    // deeper water around them)
    // (the sea: the bed shows only along the very edge, the last half meter. The game draws the near
    // sea, the only one the bed is seen through, on some cells around the camera and not on others:
    // any wider clearness showed those cells, square patches moving with the camera)
    float clear_water = pond_here ? exp(-depth_real / 0.25) * (1.0 - near_fade)
                                  : exp(-depth_real / 0.35) * 0.6 * (1.0 - near_fade);

    // how rough the sea is: some chop always, rising and falling over minutes, more with rain and
    // a sandstorm (ponds stay still). In a thunderstorm the water looks as in clear weather: no
    // storm waves, no rain on it (the user's choice)
    float storm_off = 1.0 - clamp(weather2.x * 4.0, 0.0, 1.0);
    float sea_state = pond_here ? 0.0
        : clamp(0.35 + 0.2 * sin(t * 0.011) * sin(t * 0.0047 + 1.7) + 0.45 * weather.x * storm_off +
                0.35 * weather.y, 0.0, 2.0);
    slope *= 1.0 + 0.5 * sea_state;

    // the swell: long waves in open water, faded out with distance (they'd shimmer) and depth;
    // higher and quicker the rougher the sea, a short wind sea over it when it's rough
    float swell_amt = (pond_here ? 0.0 : water_swell_strength * (0.6 + 1.4 * sea_state)) *
                      smoothstep(1.0, 6.0, depth_m) * (1.0 - smoothstep(1.5, 7.0, water_foot));
    float crest = 0.0;
    float wave_hi = 0.0;  // how high on a storm wave (-1..1, 0 calm): crests lighter, troughs darker
    if (swell_amt > 0.0) {
      // (slow: the long swell's crests crawl, they raced across at 5-6 m/s)
      float ts = t * 0.4;
      float s0 = water_swell(world, ts);
      slope += swell_amt * vec2(water_swell(world + vec2(0.5, 0.0), ts) - s0,
                                water_swell(world + vec2(0.0, 0.5), ts) - s0) / 0.5;
      vec2 wd = length(dust_wind.xy) > 0.1 ? normalize(dust_wind.xy) : vec2(0.8, 0.6);
      // a storm's big waves: long, steep, sharp-crested, marching with the wind, seen far out
      float storm_k = smoothstep(0.7, 1.6, sea_state) * smoothstep(1.0, 6.0, depth_m) *
                      (1.0 - smoothstep(1.0, 4.5, water_foot));
      float big = 0.0;
      if (storm_k > 0.0) {
        const float be = 0.4;
        float tw = t * 0.3;  // (the waves slowed down: at full speed they raced by)
        big = storm_sea(world, wd, tw);
        slope += storm_k * 0.9 * vec2(storm_sea(world + vec2(be, 0.0), wd, tw) - big,
                                       storm_sea(world + vec2(0.0, be), wd, tw) - big) / be;
      }
      // the wind sea: shorter waves running with the wind (gone where they'd be under a pixel)
      float rough = smoothstep(0.5, 1.2, sea_state) * swell_amt * (1.0 - smoothstep(0.1, 0.5, water_foot));
      if (rough > 0.0) {
        const float we = 0.15;
        float w0 = wind_sea(world, wd, t * 0.35);
        float w1 = wind_sea(world + vec2(we, 0.0), wd, t * 0.35);
        float w2 = wind_sea(world + vec2(0.0, we), wd, t * 0.35);
        slope += rough * 0.12 * vec2(w1 - w0, w2 - w0) / we;
        // (a breaking crest: the very top of a wind wave, riding high on the swell, here and
        // there along it)
        float top = smoothstep(0.55, 0.85, w0) * smoothstep(-0.1, 0.25, s0);
        float breaking = smoothstep(0.2, 0.6, gnoise(world * 0.09 + wd * t * 0.15));
        crest = rough * top * breaking;
      }
      // (a storm wave's crest breaks along its length, in torn stretches)
      if (storm_k > 0.0) {
        crest = max(crest, storm_k * smoothstep(0.62, 0.9, big) *
                               smoothstep(0.0, 0.5, gnoise(world * 0.05 + wd * t * 0.1) + 0.2));
      }
      wave_hi = storm_k * big;
    }
    // wakes: rings spreading from where Jak touched the water lately (each point of his path near
    // this water's height)
    float water_y = dot(up_view, p) + cam_pos.y;
    float wake_foam = 0.0;
    for (int i = 0; i < wake_count; i++) {
      // (Jak's root is his feet: up to 2.5 m under the surface swimming, over it riding; a
      // vehicle's seat higher)
      bool ride = wake[i].w >= 10.0;
      float above = wake[i].y - water_y;
      if (above < -2.5 * 4096.0 || above > (ride ? 3.0 : 1.0) * 4096.0) {
        continue;
      }
      vec2 rel = world - wake[i].xz / 4096.0;  // meters
      float dist = length(rel);
      float age = wake[i].w - (ride ? 10.0 : 0.0);
      if (ride) {
        // churned water behind it: foam spreading and thinning out
        float fr = 0.9 + 1.2 * age;
        wake_foam += exp(-dist * dist / (fr * fr)) * (1.0 - age / 2.0) * 0.7;
      }
      float x = dist - (0.3 + 1.4 * age);
      float band = exp(-x * x / 0.15) * (1.0 - age / 2.0);
      if (band > 0.001) {
        slope += rel / max(dist, 0.05) * (band * 0.45 * wake_strength * sin(x * 10.0));
      }
    }
    // rain on the water: rings where the drops land (two staggered layers) and their small
    // splashes up close; further out, where the rings would shimmer, the surface just roughened
    float rain_hits = 0.0;
    if (weather.x * storm_off > 0.01) {
      float rain_amt = min(weather.x * storm_off, 1.5);
      float rings = 1.0 - smoothstep(0.03, 0.08, water_foot);
      vec2 w2 = world * 1.6 + vec2(3.7, 1.3);
      if (rings > 0.0) {
        slope += (rain_ripples(world, t) + rain_ripples(w2, t + 0.37)) * rain_amt * rings;
      }
      // the drops' splashes: crowns up close, sparkles further out (sized by the pixel's width, not
      // its length along the view, which grows at grazing angles), out to the horizon
      float across = z / 4096.0 * pixel_rad;
      float close = 1.0 - smoothstep(0.012, 0.03, across);
      if (close > 0.0) {
        rain_hits = (rain_splashes(world, t, 0.018) + rain_splashes(w2, t + 0.37, 0.03)) * close;
      }
      if (close < 1.0) {
        rain_hits += rain_sparkles(world, t, across) * 1.6 * (1.0 - close) *
                     (1.0 - smoothstep(350.0 * 4096.0, 600.0 * 4096.0, z));
      }
      rain_hits *= rain_amt;
      // (the rain's chop drifts slowly in the world: drifting in screen terms it raced across the
      // far water, its pattern there meters wide)
      vec2 rw = (world + vec2(t * 0.3, -t * 0.25)) / clamp(water_foot * 4.0, 0.08, 2.0);
      slope += vec2(gnoise(rw), gnoise(rw + 5.3)) * 0.08 * rain_amt * (1.0 - rings) * far;
    }
    vec3 n = normalize(up_view + slope.x * right_view + slope.y * fwd_view);

    // what's under shallow water, bent by the waves (refraction), where the bent ray still lands
    // under water (not on something above the surface); lit by caustics where the sun gets in
    if (!water_deep && clear_water > 0.02) {
      vec2 ruv = uv + slope * water_refraction * min(depth_m, 2.0) * far;
      float rwd = texture(tex_T5, ruv).r;
      if (rwd > texture(tex_T3, ruv).r + 1e-7) {
        c = texture(tex_T0, ruv).rgb;
      }
      if (sun_dir.w > 0.0 && seabed_level && water_caustics > 0.0 && !pond_here) {
        vec2 bed = vec2(dot(right_view, seabed_p) + cam_pos.x, dot(fwd_view, seabed_p) + cam_pos.z) / 4096.0;
        float k = caustics(bed, t) * water_caustics * sun_dir.w * smoothstep(0.05, 0.4, depth_real) *
                  exp(-depth_real / 3.0) * (1.0 - smoothstep(0.3, 1.0, water_foot)) * (1.0 - near_fade) *
                  0.35 * clear_water;  // (faint, and only where the bed shows: the edge)
        c += c * k * vec3(0.9, 1.0, 0.95);
      }
    }
    vec3 vdir = normalize(p);
    vec3 r = reflect(vdir, n);
    float facing = clamp(dot(-vdir, n), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - facing, 5.0);

    // the water's body: one color for the near and far sea (the game draws them differently),
    // shaded a little by the ripples so it keeps some texture; clearer where it's shallow, where
    // what's under it (and the game's own water) shows through
    float shade = 1.0 + 0.3 * far * dot(n - up_view, normalize(fwd_view + right_view * 0.3));
    vec3 body_col = mix(water_deep_color, water_shallow_color, clear_water);
    // (the body follows the hour: full at noon, dimmer and tinted by the low sky as the sun goes
    // down; the night's darkening is night_dim's, so the factor goes back to 1 into the night)
    if (tune_water_hour > 0.0) {
      float wday = smoothstep(0.02, 0.5, sky_sun.w);
      float wk = mix(mix(1.0 - 0.45 * tune_water_hour, 1.0, wday), 1.0, night_dark.x);
      // (and under a grey sky or rain, as the city around it dims)
      float wover = clamp((clouds.y - 0.5) * 2.0, 0.0, 1.0);
      wk *= 1.0 - tune_water_hour * (0.3 * wover + 0.15 * min(weather.x, 1.0));
      vec3 wsk = sky_color(normalize(fwd_view + up_view * 0.2));
      vec3 wtint = clamp(wsk / max(luma(wsk), 1e-3), vec3(0.6), vec3(1.5));
      body_col *= wk * mix(vec3(1.0), wtint, 0.4 * tune_water_hour * (1.0 - wday));
    }
    vec3 body = night_dim(body_col * shade);
    // (a pond's edge: its sandy bed seen through a few cm of murky water, not the sea's light
    // turquoise: that drew a pale line along the shore, there only once the near water is drawn)
    if (pond_here) {
      body = mix(night_dim(water_deep_color * shade), c * vec3(0.7, 0.76, 0.78), clear_water);
    }
    // (only a little of the game's own water texture shows: it's a small repeating tile)
    vec3 wc = mix(body, c, clear_water * 0.3 * water_unify + (1.0 - water_unify));

    // reflection. Far away: the screen where the reflected ray vanishes (sky, distant hills).
    // Near: trace the ray through the depth buffer (which has no water) to catch nearby things.
    // (a wave tilted toward the eye reflects a ray going down: it'd see the sea itself or the
    // rock, dark blots on the water. Kept just over the horizon, it sees the sky low down)
    vec3 re = r;
    float re_up = dot(re, up_view);
    if (re_up < 0.03) {
      re = normalize(re + up_view * (0.03 - re_up));
    }
    vec3 refl = fog_color;
    if (re.z > 1e-3) {
      vec2 env_uv = clamp(0.5 * vec2(proj.z * re.x, proj.w * re.y) / re.z + 0.5, vec2(0.002), vec2(0.998));
      refl = texture(tex_T0, env_uv).rgb;
      // (where that lands on the water, the sky's color there instead)
      float ed = texture(tex_T3, env_uv).r;
      float efd = texture(tex_T6, env_uv).r;
      if (texture(tex_T5, env_uv).r > ed + 1e-7 || (efd > 1e-7 && abs(efd - ed) < 1e-7)) {
        refl = fog_color;
      }
    }
    vec3 ssr = refl;
    float ssr_hit = 0.0;
    if (z < 80.0 * 4096.0) {
      float step_len = 0.25 * 4096.0 + z * 0.01;
      // start each pixel's march a little further along (the TAA averages the steps away: no
      // staircase where the reflection meets what it reflects)
      vec3 q = p + r * step_len * hash1(gl_FragCoord.xy + fract(haze.w * 7.0) * 61.0);
      for (int i = 0; i < 32; i++) {
        q += r * step_len;
        step_len *= 1.12;
        if (q.z <= 0.0) {
          break;  // came back toward the camera
        }
        vec2 quv = 0.5 * vec2(proj.z * q.x, proj.w * q.y) / q.z + 0.5;
        if (quv.x < 0.0 || quv.y < 0.0 || quv.x > 1.0 || quv.y > 1.0) {
          break;
        }
        float sd = texture(tex_T3, quv).r;
        float sfd = texture(tex_T6, quv).r;
        if (sd <= 1e-7 || (sfd > 1e-7 && abs(sfd - sd) < 1e-7)) {
          continue;  // sky, clouds or the sea itself: not a reflector
        }
        float scene_z = view_z(sd);
        // (not what stands between the camera and the water, the rock Jak's on: a wave tilted
        // toward the eye sent the ray back into it, dark blots on the sea)
        if (scene_z < z * 0.97) {
          continue;
        }
        if (q.z > scene_z && q.z - scene_z < step_len + 4096.0) {
          // home in on where the ray meets the surface (halving the last step)
          vec3 lo = q - r * step_len / 1.12;
          vec3 hi = q;
          for (int k = 0; k < 5; k++) {
            vec3 mid = (lo + hi) * 0.5;
            vec2 muv = 0.5 * vec2(proj.z * mid.x, proj.w * mid.y) / mid.z + 0.5;
            if (mid.z > view_z(texture(tex_T3, muv).r)) {
              hi = mid;
            } else {
              lo = mid;
            }
          }
          quv = 0.5 * vec2(proj.z * hi.x, proj.w * hi.y) / hi.z + 0.5;
          // fade toward the screen's edges, wider at the top (where the reflected ray leaves the
          // picture it ends in a hard line otherwise)
          vec2 edge = min(quv, 1.0 - quv);
          ssr_hit = smoothstep(0.0, 0.08, edge.x) * smoothstep(0.0, 0.15, edge.y) *
                    (1.0 - smoothstep(50.0 * 4096.0, 80.0 * 4096.0, z));
          ssr = texture(tex_T0, quv).rgb;
          break;
        }
      }
    }
    // what the screen doesn't have: traced through the levels (near water only, it's costly)
    // (faded out from 150 to 200 m, no seam)
    refl = reflection_with_rt(p, r, refl, ssr, ssr_hit, 400.0 * 4096.0,
                              1.0 - smoothstep(150.0 * 4096.0, 200.0 * 4096.0, z));
    wc = rt_mode == 3 ? refl : mix(wc, refl, clamp(fresnel * 1.2, 0.0, 1.0) * water_strength);
    if (abs(tune_water_dbg - 3.0) < 0.5) {
      wc = body;  // (test: no reflection)
    }

    // the lamps on the water at night: each one's image drawn out by the waves into a shimmering
    // column toward the eye (a harbor at night); it reaches much further than the lamp's light
    float lamp_night = max(night_dark.x, clouds.w);
    if ((light_count > 0 || spot_count > 0) && lamp_night > 0.0 && abs(tune_water_dbg - 1.0) > 0.5) {
      vec3 lsum = vec3(0.0);
      for (int i = 0; i < light_count; i++) {
        vec3 tl = light_pos[i].xyz - p;
        float d = length(tl);
        vec3 h = normalize(tl / max(d, 1.0) - vdir);
        float nh = max(dot(n, h), 0.0);
        float sp = pow(nh, 500.0) * 5.0 + pow(nh, 70.0) * 0.2;
        float rr = d / max(light_pos[i].w, 4096.0);
        lsum += light_color[i].rgb * sp / (1.0 + 0.04 * rr * rr);
      }
      for (int i = 0; i < spot_count; i++) {
        vec3 tl = spot_pos[i].xyz - p;
        float d = length(tl);
        vec3 l = tl / max(d, 1.0);
        vec3 h = normalize(l - vdir);
        float nh = max(dot(n, h), 0.0);
        // only toward where the headlight points (seen from the front it's blinding, from behind dark)
        float facing_us = smoothstep(0.3, 0.8, dot(-l, spot_dir[i].xyz));
        lsum += vec3(1.0, 0.93, 0.8) * (pow(nh, 500.0) * 6.0 + pow(nh, 70.0) * 0.25) * facing_us *
                spot_pos[i].w / (1.0 + d * d / (60.0 * 60.0 * 4096.0 * 4096.0));
      }
      // (not where the reflected ray meets a wall first: a lamp behind it can't be seen in the water;
      // and a soft ceiling, or a few close lamps burn a white hole in calm water)
      lsum *= 1.0 - 0.9 * ssr_hit;
      lsum /= 1.0 + max(lsum.r, max(lsum.g, lsum.b));
      wc += lsum * lamp_night * (0.4 + 0.6 * water_strength);
    }
    // the far lights too (across a harbor, the desert's fires by an oasis): thin columns of light
    // broken up by the waves
    // (summed, then hidden where the reflected ray meets a wall first and rolled off like the lamps'
    // columns: in the harbor at night the near ones added up to flat white blocks and wide red and
    // pink bands with hard sides, where vanilla's water is plain dark blue)
    vec3 bsum = vec3(0.0);
    float bwide = mix(90.0, 220.0, tune_beacon_soft);
    for (int i = 0; i < (abs(tune_water_dbg - 2.0) < 0.5 ? 0 : beacon_count); i++) {
      vec3 l = normalize(beacon_pos[i].xyz - p);
      float nh = max(dot(n, normalize(l - vdir)), 0.0);
      float fade = exp(-length(beacon_pos[i].xyz) / (1800.0 * 4096.0));
      bsum += beacon_color[i].rgb * beacon_color[i].w * fade * (pow(nh, 700.0) * 3.0 + pow(nh, bwide) * 0.08);
    }
    if (tune_beacon_soft > 0.0) {
      vec3 soft = bsum * (1.0 - 0.9 * ssr_hit);
      soft = soft / (1.0 + max(soft.r, max(soft.g, soft.b))) * tune_beacon_water;
      bsum = mix(bsum, soft, tune_beacon_soft);
    }
    wc += bsum * (0.4 + 0.6 * water_strength);

    // sun glints: a tight highlight plus a broad sheen toward the sun
    if (sun_dir.w > 0.0) {
      float sr = max(dot(r, sun_dir.xyz), 0.0);
      float glint = pow(sr, 900.0) * 7.0 + pow(sr, 80.0) * 0.25;
      wc += vec3(1.0, 0.93, 0.78) * glint * sun_dir.w;
    }

    // the drops' splashes, lit like the rain
    if (rain_hits > 0.0) {
      wc = mix(wc, wc * 1.3 + vec3(0.16, 0.17, 0.19) + fog_color * 0.4, min(rain_hits, 1.0) * 0.8);
    }

    // foam where the sea meets the shore, moving with the waves (a still pond has none)
    if (!water_deep && seabed_level && depth_m < 0.8 && !pond_here) {
      float wave = 0.5 + 0.7 * gnoise(world * 1.4 + vec2(t * 0.35, -t * 0.2)) +
                   0.35 * gnoise(world * 3.7 - vec2(t * 0.5, t * 0.3));
      float foam = smoothstep(0.8, 0.05, depth_m) * smoothstep(0.25, 0.75, wave + 0.6 - depth_m);
      wc = mix(wc, vec3(0.92, 0.94, 0.9), foam * 0.55);
    }
    // a rough sea: greyer under the storm, whitecaps breaking on the crests
    if (sea_state > 0.5) {
      wc = mix(wc, vec3(luma(wc)) * vec3(0.9, 0.97, 1.0), 0.35 * weather2.x * storm_off);
      // (the light through a storm wave's thin crest, the shadowed trough before it)
      wc *= 1.0 + 0.18 * clamp(wave_hi, -1.0, 1.0);
      // (torn foam: fine streaks, not a flat patch)
      float tear = smoothstep(0.1, 0.5, gnoise(world * vec2(3.0, 5.0) + vec2(t * 0.3, 0.0)) * 0.5 +
                                         gnoise(world * 9.0) * 0.25 + 0.25);
      float caps = crest * tear;
      wc = mix(wc, vec3(0.86, 0.9, 0.9) * (0.25 + 0.75 * clamp(sky_sun.w * 2.5, 0.0, 1.0)),
               caps * 0.45 * (1.0 - smoothstep(0.3, 1.2, water_foot)));
    }
    if (wake_foam > 0.0) {
      float lace = smoothstep(0.35, 0.75, gnoise(world * 2.2 + vec2(t * 0.3, 0.0)) * 0.5 + 0.5 +
                              (min(wake_foam, 1.0) - 0.5) * 0.8);
      wc = mix(wc, vec3(0.9, 0.93, 0.92) * (0.35 + 0.65 * clamp(sky_sun.w * 2.5, 0.0, 1.0)),
               lace * min(wake_foam, 1.0) * 0.8);
    }
    // the very edge: the last few cm of water fade into the wet ground beside them (the color of
    // the dry ground nearby, darkened as the shore band below darkens it), no hard line where the
    // sand dips under the water
    if (!water_deep && seabed_d > 1e-7 && depth_m < 0.35) {
      float r_uv = clamp(0.8 * 4096.0 * abs(proj.w) * 0.5 / max(z, 1.0), 2.0 * texel.y, 0.06);
      vec3 shore_sum = vec3(0.0);
      float shore_n = 0.0;
      for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.7854 + 0.3;
        vec2 tuv = uv + vec2(cos(a) * texel.x / texel.y, sin(a)) * r_uv * (i % 2 == 0 ? 1.0 : 0.55);
        if (textureLod(tex_T5, tuv, 0.0).r <= textureLod(tex_T3, tuv, 0.0).r + 1e-7) {
          shore_sum += textureLod(tex_T0, tuv, 0.0).rgb;
          shore_n += 1.0;
        }
      }
      if (shore_n > 0.0) {
        float edge = smoothstep(0.0, pond_here ? 0.14 : 0.3, depth_m);
        wc = mix(shore_sum / shore_n * vec3(0.6, 0.6, 0.62), wc, edge);
      }
    }
    // a puddle or a creek's edge where the game draws only the far sea (no near sea there, so no
    // bed seen through it): close by, ringed by ground, it takes that ground's color, darkened,
    // instead of the open sea's deep blue
    if (water_deep && !pond_here && z < 30.0 * 4096.0) {
      float r_uv = clamp(1.5 * 4096.0 * abs(proj.w) * 0.5 / max(z, 1.0), 3.0 * texel.y, 0.08);
      vec3 land_sum = vec3(0.0);
      float land_n = 0.0;
      for (int i = 0; i < 8; i++) {
        float a = float(i) * 0.7854 + 0.3;
        vec2 tuv = uv + vec2(cos(a) * texel.x / texel.y, sin(a)) * r_uv * (i % 2 == 0 ? 1.0 : 0.5);
        float td = textureLod(tex_T3, tuv, 0.0).r;
        if (td > 1e-7 && abs(textureLod(tex_T6, tuv, 0.0).r - td) > 1e-7 &&
            textureLod(tex_T5, tuv, 0.0).r <= td + 1e-7) {
          land_sum += textureLod(tex_T0, tuv, 0.0).rgb;
          land_n += 1.0;
        }
      }
      if (land_n > 0.0) {
        float k = smoothstep(0.0, 4.0, land_n) * (1.0 - smoothstep(18.0, 30.0, z / 4096.0)) * 0.7;
        wc = mix(wc, land_sum / land_n * vec3(0.5, 0.56, 0.58), k);
      }
    }
    // the game's effects drawn over the water (foam, splashes, wakes) show through where they're
    // clearly brighter than our water; on a disc lying on the water, anything brighter does
    const vec3 luma = vec3(0.299, 0.587, 0.114);
    float over = dot(c, luma) - dot(wc, luma);
    c = water_decal ? max(wc, c) : mix(wc, c, smoothstep(0.06, 0.2, over));
  } else if (water_strength > 0.0 && z > 0.0 && z < 90.0 * 4096.0) {
    // the shore: the ground just above the water is wet, darker and a little glossy, its line
    // ragged and breathing with the water (the sea's runs up and down with the waves and leaves
    // a thin line of foam). The water's height: from any water seen within a meter or two.
    float r_uv = clamp(1.8 * 4096.0 * abs(proj.w) * 0.5 / z, 3.0 * texel.y, 0.1);
    float above_water = 1e9;
    for (int i = 0; i < 12; i++) {
      float a = float(i) * 1.0472 + (i >= 6 ? 0.5236 : 0.0);
      vec2 tuv = uv + vec2(cos(a) * texel.x / texel.y, sin(a)) * r_uv * (i >= 6 ? 1.0 : 0.45);
      float twd = textureLod(tex_T5, tuv, 0.0).r;
      if (twd > textureLod(tex_T3, tuv, 0.0).r + 1e-7) {
        above_water = dot(up_view, p - view_pos(tuv, twd)) / 4096.0;
        break;
      }
    }
    if (above_water > -0.05 && above_water < 1.0) {
      float t = haze.w;
      vec2 wxz = water_world;
      bool still = pond != 0 && abs((dot(up_view, p) + cam_pos.y) / 4096.0 - above_water - 9.0) > 0.4;
      // where the water reaches now (a pond: barely moves; the sea: runs up with the waves)
      float lap = still ? 0.01 + 0.01 * sin(t * 0.7 + gnoise(wxz * 0.5) * 5.0)
                        : 0.05 + 0.07 * (0.5 + 0.5 * sin(t * 1.1 + gnoise(wxz * 0.25) * 6.0));
      float hw = max(above_water - lap, 0.0);
      // how high the sand stays wet (the sea's higher, where the waves have been), ragged
      float reach = (still ? 0.22 : 0.55) * (0.65 + 0.7 * (gnoise(wxz * 1.6) * 0.5 + 0.5));
      float wet = 1.0 - smoothstep(0.0, reach, hw);
      wet *= wet;
      c = mix(c, c * vec3(0.6, 0.6, 0.62), wet * 0.9);
      // just where the water drew back: a glossy film with the sky in it
      float film = 1.0 - smoothstep(0.0, still ? 0.03 : 0.06, hw);
      c += night_dim(fog_color) * film * 0.07;
      // the sea's runup leaves a thin, broken line of foam
      if (!still) {
        float lace = smoothstep(0.1, 0.6, gnoise(wxz * 3.1 + vec2(t * 0.2, 0.0)) * 0.5 + 0.5);
        float line = exp(-sq((above_water - lap) / 0.02)) * lace;
        c = mix(c, vec3(0.9, 0.92, 0.9) * (0.35 + 0.65 * clamp(sky_sun.w * 2.5, 0.0, 1.0)), line * 0.5);
      }
    }
  }

  // cutscenes: a real depth of field around what the camera looks at (autofocus on the middle of
  // the screen): each sample counts if its own blur reaches this pixel, and something behind only
  // as far as this pixel's blur, so a sharp face doesn't smear over the blurred background
  if (cine_dof.z > 0.0 && wsurf == 0.0) {
    float fz = cine_dof.x;
    float maxr = cine_dof.z;
    float zc = z > 0.0 ? z : 1e9;
    float coc_c = clamp(abs(1.0 - fz / zc) * 1.2 * cine_dof.y, 0.0, 1.0) * maxr;
    vec3 sum = c;
    float wsum = 1.0;
    float rot = pixel_angle();
    for (int i = 0; i < 24; i++) {
      vec2 o = vogel(i, 24, rot) * maxr;
      float rr = length(o);
      vec2 suv = uv + o * texel;
      float sd = texture(tex_T3, suv).r;
      float sz = sd > 1e-7 ? view_z(sd) : 1e9;
      float coc_s = clamp(abs(1.0 - fz / sz) * 1.2 * cine_dof.y, 0.0, 1.0) * maxr;
      float reach = sz < zc ? coc_s : min(coc_s, coc_c);
      float wgt = clamp(reach - rr + 1.0, 0.0, 1.0);
      sum += texture(tex_T0, suv).rgb * wgt;
      wsum += wgt;
    }
    c = sum / wsum;
    blur = max(blur, coc_c / maxr);
  }

  // light depth of field on the far distance (samples nearer than the pixel are skipped)
  // (not on water: its pixels would blur back in the game's own, two-colored water)
  if (dof.z > 0.0 && cine_dof.z == 0.0 && z > 0.0 && wsurf == 0.0) {
    float coc = smoothstep(dof.x, dof.y, z) * dof.z;
    if (coc > 0.25) {
      vec3 sum = c;
      float wsum = 1.0;
      for (int i = 0; i < 12; i++) {
        vec2 suv = uv + vogel(i, 12, 0.0) * coc * texel;
        float sd = texture(tex_T3, suv).r;
        float sz = sd > 1e-7 ? view_z(sd) : 1e12;
        float w = sz > z * 0.85 ? 1.0 : 0.0;
        sum += texture(tex_T0, suv).rgb * w;
        wsum += w;
      }
      c = sum / wsum;
      blur = max(blur, coc / dof.z);
    }
  }

  // contrast-adaptive sharpen (AMD CAS, simplified), not where we just blurred
  float sharp = wsurf > 0.0 ? 0.0 : sharpen * (1.0 - blur);  // not on water (raw neighbors)
  if (sharp > 0.0) {
    vec3 n = texture(tex_T0, uv + vec2(0, -texel.y)).rgb;
    vec3 s = texture(tex_T0, uv + vec2(0, texel.y)).rgb;
    vec3 w = texture(tex_T0, uv + vec2(-texel.x, 0)).rgb;
    vec3 e = texture(tex_T0, uv + vec2(texel.x, 0)).rgb;
    vec3 mn = min(c, min(min(n, s), min(w, e)));
    vec3 mx = max(c, max(max(n, s), max(w, e)));
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, 1e-4), 0.0, 1.0));
    vec3 wgt = -amp * mix(0.125, 0.2, sharp) * sharp;
    c = clamp((c + (n + s + w + e) * wgt) / (1.0 + 4.0 * wgt), 0.0, 1.0);
  }
  // (the light effects out of the scene: from here on the grade shades what's behind them)
  c = max(c - fx_w * fx_d, 0.0);

  // ambient occlusion and sun shadows
  float ao = wsurf > 0.0 ? 1.0 : ao_upsample(tex_coord, z);
  // (not on the flames: sprites with no depth of their own, they took the occlusion of the trunk or
  // wall behind them, its dark cool tint, and burned green-blue and dim)
  ao = mix(ao, 1.0, flame_keep);
  // (not on the characters: their see-through cloth (Jak's tunic, scarf) has no depth of its own, it took
  // the occlusion of the legs and arm behind it and looked transparent)
  {
    float ctag = pix_tag;
    ao = mix(ao, 1.0, (ctag > 0.4 && ctag < 0.535) ? 1.0 : 0.0);  // (and their cloth, the 0.52 tag)
  }
  if (ao_debug != 0 || tune_ao_view > 0.5) {
    // (tune_ao_view: the same view from the tuning file, in a running game)
    // right half: distance (grey: log10 of meters / 4, so 1 km = 0.75), sky in blue
    float lm = z > 0.0 ? log(z / 4096.0) / log(10.0) / 4.0 : -1.0;
    color = tex_coord.x > 0.5 ? (z > 0.0 ? vec4(vec3(lm), 1.0) : vec4(0.0, 0.0, 1.0, 1.0)) : vec4(vec3(ao), 1.0);
    return;
  }
  // the surface normal, once: the shadows, the wet ground, the prints, the lights and the
  // reflections all use it
  vec3 surf_n = z > 0.0 ? surface_normal(tex_coord, p) : vec3(0.0, 0.0, -1.0);
  // the same across a meter, for how the sun and the lamps fall on it: the terrain is flat triangles
  vec3 soft_n = z > 0.0 ? soft_normal(tex_coord, surf_n, z) : surf_n;
  // (how much the normal turns inside this pixel: the highlights' specular AA. Out here, where
  // every pixel runs, so the derivatives are defined)
  vec3 n_dx = dFdx(surf_n), n_dy = dFdy(surf_n);
  float n_var = tune_spec_aa * (dot(n_dx, n_dx) + dot(n_dy, n_dy));
  // how much of the sun reaches it (the shadow map's, below): the sand's glints and sheen need it
  float sun_lit = 1.0;
  // (how much of the low sun's sky fill the ground takes, set by the shadow map's test below)
  float gold_w = 1.0;
  // (how much the low sun's shade below may take at most: never under tune_dusk_floor of the game's color)
  float dusk_floor_w = 0.0;
  if (normal_debug != 0 && z > 0.0) {
    // the level's own normals in color, the depth-rebuilt ones in grey
    // left half: the normals used (level's own in color, depth-rebuilt in grey); right half: the
    // depth-rebuilt ones in color, to compare
    vec3 nn = surf_n;
    bool own = pix_normal.a > 0.17;
    if (tex_coord.x > 0.5) {
      vec3 pr = view_pos(tex_coord + vec2(texel.x, 0), texture(tex_T3, tex_coord + vec2(texel.x, 0)).r);
      vec3 pu = view_pos(tex_coord + vec2(0, texel.y), texture(tex_T3, tex_coord + vec2(0, texel.y)).r);
      nn = normalize(cross(pr - p, pu - p));
      nn = dot(nn, -p) < 0.0 ? -nn : nn;
      own = true;
    }
    vec3 shown = nn * 0.5 + 0.5;
    color = vec4(own ? shown : vec3(dot(shown, vec3(0.33))), 1.0);
    return;
  }
  // (per channel, by the surface's own color (Jimenez's GTAO multi-bounce fit): light bounced
  // between bright walls of a crevice keeps it their color, a deeper sand or sandstone; a grey
  // multiply took them to a muddy grey-brown. Dark surfaces stay as dark)
  if (tune_ao_multibounce > 0.0 && ao < 1.0) {
    vec3 alb = clamp(c, 0.0, 1.0);
    vec3 ka = 2.0404 * alb - 0.3324;
    vec3 kb = -4.7951 * alb + 0.6417;
    vec3 kc = 2.7552 * alb + 0.6903;
    vec3 ao_mb = max(vec3(ao), ((ao * ka + kb) * ao + kc) * ao);
    // (its color only: the fit also gives back light, which lifted the arena at 20h by ~5%: the
    // dark look is deliberate. Brought back to the grey occlusion's brightness)
    ao_mb *= mix(1.0, ao / max(luma(ao_mb), 1e-3), tune_ao_mb_luma);
    c *= mix(vec3(ao), ao_mb, tune_ao_multibounce);
  } else {
    c *= ao;
  }
  // bounced light: where the surroundings hide the sky (the occluded corners, crevices, the foot of
  // a wall) the light comes from them instead, tinted by their color (an orange canyon's shadows go
  // warm, a lawn's corners green). Six taps about a meter around, on the scene as drawn
  if (z > 0.0 && wsurf == 0.0 && ao < 0.98) {
    float rad = clamp(0.9 * 4096.0 * proj.z / z * 0.5, 4.0 * texel.x, 0.08);
    vec3 nb = vec3(0.0);
    for (int i = 0; i < 6; i++) {
      float a = float(i) * 1.0472 + 0.4;
      nb += texture(tex_T0, uv + vec2(cos(a), sin(a) * texel.y / texel.x) * rad).rgb;
    }
    nb /= 6.0;
    float nl = dot(nb, vec3(0.3, 0.59, 0.11));
    vec3 tint = clamp(nb / max(nl, 1e-3), 0.0, 2.0);
    float occ = (1.0 - ao) * smoothstep(1200.0 * 4096.0, 200.0 * 4096.0, z);
    c = c * mix(vec3(1.0), tint, 0.45 * occ) + nb * 0.1 * occ;
  }

  // rain: everything darkens a little as it soaks, level ground more (it gets the puddles and
  // reflections further down)
  float wet = 0.0;
  float wet_flat = 0.0;  // level ground, where puddles stay after the rest has dried
  float wet_gloss = 1.0;  // how much of a sheen the rain gives it (wood and walls drink it, tfrag3.frag)
  vec3 wet_n = vec3(0.0);
  if (max(weather.w, puddle_level) > 0.0 && z > 0.0 && wsurf == 0.0) {
    wet_n = surf_n;
    {
      vec4 wg = pix_normal;
      float wl = length(wg.rgb * 2.0 - 1.0);
      if (wg.a > 0.83 && wl > 0.985 && wl <= 0.9975) {
        wet_gloss = wl > 0.9925 ? 0.35 : 0.6;
      }
    }
    // (out to the horizon: far away the ground still reads soaked, only the puddles' detail goes)
    wet_flat = smoothstep(0.5, 0.85, dot(wet_n, up_view));
    wet = weather.w * wet_flat;
    c *= 1.0 - 0.1 * weather.x - 0.38 * wet;
    // what isn't level ground (grass, bushes, rocks, walls): soaked too, darker and deeper in color,
    // with a wet sheen at grazing angles and drops glinting on it up close
    float wet_side = weather.w * (1.0 - wet_flat);
    if (wet_side > 0.01) {
      vec3 wq = (view_to_world * p + cam_pos) / 4096.0;
      // (streaks: noise stretched down the slope, where the rain runs off)
      float runs = smoothstep(0.45, 0.75, vnoise3(wq * vec3(1.6, 0.12, 1.6)));
      c = mix(c, c * c * 1.7, 0.5 * wet_side);
      c *= 1.0 - (0.22 + 0.18 * runs) * wet_side;
      vec3 vd = normalize(p);
      float graze = pow(1.0 - clamp(abs(dot(-vd, wet_n)), 0.0, 1.0), 4.0);
      c += (fog_color * 0.35 + 0.04) * graze * wet_side * wet_gloss;
      if (z < 25.0 * 4096.0) {
        vec3 wp = (view_to_world * p + cam_pos) / 4096.0;
        float drop = smoothstep(0.86, 0.97, vnoise3(wp * 38.0)) *
                     (0.5 + 0.5 * sin(weather.z * 3.0 + dot(wp, vec3(17.0, 11.0, 23.0))));
        c += vec3(0.85, 0.9, 1.0) * drop * wet_side * 0.35 *
             (1.0 - smoothstep(10.0 * 4096.0, 25.0 * 4096.0, z));
      }
    }
  }

  // snow lying on the ground: first in patches on level ground, then over everything that faces
  // up as it builds; lit by the light that reached the ground there, bluish in the shade. Not on Jak
  // himself (the prints below are pressed into it)
  if (snow_cover > 0.0 && z > 0.0 && wsurf == 0.0) {
    bool on_jak = player_view.w > 0.0 && length(p - player_view.xyz) < 1.5 * 4096.0 &&
                  dot(p - player_view.xyz, up_view) > 0.25 * 4096.0;
    vec3 sw = (view_to_world * p + cam_pos) / 4096.0;
    float upn = dot(surf_n, up_view);
    float n = vnoise3(vec3(sw.xz * 0.35, 0.0)) * 0.6 + vnoise3(vec3(sw.xz * 1.7, 3.0)) * 0.3 +
              vnoise3(vec3(sw.xz * 7.0, 9.0)) * 0.1;
    float slope = smoothstep(mix(0.95, 0.35, snow_cover), mix(1.0, 0.6, snow_cover), upn);
    float th = 1.0 - snow_cover * 1.15;
    float lie = on_jak ? 0.0 : slope * smoothstep(th, th + 0.2, n);
    if (lie > 0.0) {
      // (only as bright as the ground's own light allows, a bit over it: snow is whiter than sand,
      // not a lamp. Capped below white so it keeps its shading, grain from the patch noise)
      // (half from the daylight alone, so it stays white on dark wood and stone too)
      float day = clamp(sky_sun.w * 2.5, 0.0, 1.0);
      float light = clamp(mix(luma(c) * 1.4, 0.55 * (0.3 + 0.7 * day), 0.4), 0.03, 0.72);
      light *= 0.9 + 0.12 * n;
      c = mix(c, vec3(0.88, 0.92, 1.0) * light, lie * 0.9);
    }
    // the bare ground between the patches is damp from melt: darker, less saturated
    float damp = on_jak ? 0.0 : snow_cover * slope * (1.0 - lie);
    c = mix(c, vec3(luma(c)), 0.35 * damp) * (1.0 - 0.25 * damp);
  }

  // footprints and ruts (left where the game said the ground is soft): pressed into the flat ground
  // around them, not into grass, darker inside with a lighter rim of pushed-up sand, fading over
  // a minute and a half
  if (print_count > 0 && z > 0.0 && z < 60.0 * 4096.0 && wsurf == 0.0) {
    float sandy = (1.0 - smoothstep(1.05, 1.2, c.g / max(c.r, 0.01))) *
                  smoothstep(0.08, 0.2, dot(c, vec3(0.333)));
    vec3 nn = surf_n;
    float level_ground = smoothstep(0.75, 0.9, dot(nn, up_view));
    if (sandy * level_ground > 0.0) {
      vec3 w = view_to_world * p + cam_pos;
      float dent = 0.0, rim = 0.0;
      for (int i = 0; i < print_count; i++) {
        vec3 d = (w - prints[i].xyz) / 4096.0;  // meters
        float kind = print_info[i].y;
        // feet: the ground is at the print; wheels: under the seat
        if (d.y > 0.4 || d.y < (kind > 1.5 ? -2.5 : -0.4) || dot(d.xz, d.xz) > (kind > 1.5 ? 25.0 : 9.0)) {
          continue;
        }
        vec2 f = vec2(sin(prints[i].w), cos(prints[i].w));
        float along = dot(d.xz, f);
        float side = dot(d.xz, vec2(f.y, -f.x));
        float fade = 1.0 - smoothstep(40.0, 90.0, print_info[i].x);
        if (kind > 1.5) {
          // two tracks 1.7 m apart, each piece reaching back to the one before (2 + its length):
          // one solid track, no dashes (a little overlap at both ends hides the joins)
          float seg = max(kind - 2.0, 0.3);
          float t = abs(abs(side) - 0.85);
          float len = smoothstep(-seg - 0.15, -seg + 0.05, along) * (1.0 - smoothstep(0.0, 0.15, along));
          dent = max(dent, (1.0 - smoothstep(0.07, 0.17, t)) * len * fade * 0.8);
          rim = max(rim, smoothstep(0.12, 0.18, t) * (1.0 - smoothstep(0.18, 0.28, t)) * len * fade * 0.5);
        } else {
          // a sole: heel behind, wider toes in front
          float width = mix(0.06, 0.085, smoothstep(-0.1, 0.1, along));
          float e = (along / 0.16) * (along / 0.16) + (side / width) * (side / width);
          dent = max(dent, (1.0 - smoothstep(0.55, 1.0, e)) * fade);
          rim = max(rim, smoothstep(0.9, 1.15, e) * (1.0 - smoothstep(1.15, 1.7, e)) * fade);
        }
      }
      float k = sandy * level_ground * prints_strength;
      // bright sand is over 1 here: brought down to 1 inside the print first, or darkening it
      // still leaves it white
      c = mix(c, min(c, vec3(1.0)), clamp(dent + rim, 0.0, 1.0) * k);
      c *= 1.0 - k * (0.35 * dent - 0.06 * rim * (1.0 - dent));
    }
  }

  // lasting marks: a scorch where an explosion hit flat ground, a small burn where a shot hit
  // anything; black with a broken edge, glowing for a moment, fading away
  if (decal_count > 0 && z > 0.0 && z < 110.0 * 4096.0 && wsurf == 0.0) {
    vec3 w = view_to_world * p + cam_pos;
    float level = smoothstep(0.6, 0.85, dot(surf_n, up_view));
    float soot = 0.0;
    vec3 hot = vec3(0.0);
    for (int i = 0; i < decal_count; i++) {
      vec3 d = (w - decal[i].xyz) / 4096.0;  // meters
      float r = decal[i].w / 4096.0;
      float age = decal_info[i].x;
      int kind = int(decal_info[i].y + 0.5);
      float rr;
      if (kind == 0) {
        if (level <= 0.0 || abs(d.y) > 0.6 + r * 0.12) {
          continue;
        }
        rr = length(d.xz) / r;
      } else {
        rr = length(d) / r;
      }
      if (rr >= 1.3) {
        continue;
      }
      float n = vnoise3(d * (kind == 0 ? 1.4 : 6.0) + float(i) * 1.7);
      float edge = rr + (n - 0.5) * 0.5;
      float fade = kind == 0 ? 1.0 - smoothstep(20.0, 60.0, age) : 1.0 - smoothstep(10.0, 25.0, age);
      soot = max(soot, (1.0 - smoothstep(0.45, 1.0, edge)) * fade * (kind == 0 ? level : 1.0));
      vec3 tint = kind == 2 ? vec3(0.3, 0.6, 1.0) : kind == 3 ? vec3(1.0, 0.7, 0.2) :
                  kind == 4 ? vec3(0.7, 0.2, 1.0) : vec3(1.0, 0.3, 0.05);
      hot += tint * (1.0 - smoothstep(0.2, 0.8, edge)) * exp(-age * (kind == 0 ? 0.9 : 3.0));
    }
    float k = min(fx_detail * 2.0, 1.0);
    c = mix(c, vec3(luma(c)), soot * 0.5 * k) * (1.0 - soot * 0.78 * k) + hot * k;
  }

  // sun shadow map: surfaces facing the sun but blocked from it lose some light (the game's light is
  // baked per vertex and knows nothing of what stands between them and the sun)
  if (shadow_strength > 0.0 && sun_dir.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surf_n;
    // (a character: its own normal, not the one averaged over a meter, which gave the whole 50 cm
    // body one terminator; and no rough look 2 m toward the sun, which took a wall's shadow onto it)
    bool chr = pix_tag > 0.3 && pix_tag < 0.55;
    float ndl = dot(chr ? surf_n : soft_n, sun_dir.xyz);
    sun_lit = smoothstep(0.0, 0.15, ndl);
    float facing = smoothstep(0.0, 0.3, ndl);
    float sh = 0.0;
    // (a character with tune_chr_gap_m > 0: only casters at least that far toward the sun count, so
    // its own head, arms and Daxter don't shade it: at noon the map put Jak in his own shadow and the
    // shade's tint turned his hair green; walls and roofs further off still do)
    bool chr_gap = chr && tune_chr_gap_m > 0.0;
    if (ndl > 0.0 && !chr_gap) {
      // pushed off the surface along its normal (more far away, where a texel is bigger)
      vec3 w = view_to_world * (p + nn * (0.08 * 4096.0 + z * 0.002)) + cam_pos;
      sh = sun_shadow(w) * facing;
    }
    if (chr_gap) {
      vec3 w = view_to_world * (p + nn * (0.05 * 4096.0)) + cam_pos;
      sh = chr_sun_shadow(w, tune_chr_gap_m) * tune_chr_shade;
    }
    // turned away from the sun the map can't tell a shadow from the surface itself: looked at from
    // 2 m toward the sun, past the small shapes. Without it, inside a mountain's shadow the edges
    // of the rocks kept their full light
    if (facing < 1.0 && !chr) {
      vec3 w2 = view_to_world * (p + sun_dir.xyz * (2.0 * 4096.0 + z * 0.01)) + cam_pos;
      sh = max(sh, sun_shadow_rough(w2) * (1.0 - facing));
    }
    // a character standing in a wall's shadow: the game lights it in full anyway (its own lights
    // know no shadows), and the faces turned from the sun skipped the map above, so Jak glowed
    // sunlit inside the shade. Looked up 70 cm toward the sun, past the body's own thickness
    if (chr && !chr_gap) {
      vec3 w3 = view_to_world * (p + sun_dir.xyz * (0.7 * 4096.0 + z * 0.002)) + cam_pos;
      sh = max(sh, sun_shadow(w3) * tune_chr_shade);
    }
    if (close_up > 0.0) {
      sh *= smoothstep(0.5 * close_up, close_up, z);
    }
    // contact shadows: the shadow map is too coarse for a foot on the ground, a hand on a hip, a
    // crate's edge; a short march toward the sun through the depth buffer finds them (50 cm)
    if (ndl > 0.0 && z < 30.0 * 4096.0 && !chr_gap) {
      vec3 from = p + nn * (0.03 * 4096.0 + z * 0.001);
      float cs = contact_shadow(from, sun_dir.xyz);
      sh = max(sh, cs * 0.7 * smoothstep(30.0 * 4096.0, 15.0 * 4096.0, z) * facing);
    }
    if (tune_shadow_dbg > 0.5 && tune_shadow_dbg < 1.5) {
      sh = 0.0;  // (test: no sun shadows)
    }
    // (a character in shade under a high sun: lit back by the bright ground around it, so less deep;
    // dark yellow hair and a dark orange Daxter read olive green at noon)
    if (chr) {
      chr_bounce = smoothstep(0.35, 0.85, world_dir(sun_dir.xyz).y);
      sh *= 1.0 - tune_chr_bounce * chr_bounce;
      // (a character inside a pool of the game's own light, the arena's shaft at dusk: the map knows
      // only the sun, the game lights the body in full, and the shade made Jak a dark silhouette on
      // the bright floor. The bloom on the floor beside and below him says how strong that light is;
      // his own pixel's bloom stays low, he is the dark thing in it)
      if (tune_chr_pool > 0.0) {
        float bl = max(max(luma(texture(tex_T1, tex_coord + vec2(0.07, -0.05)).rgb),
                           luma(texture(tex_T1, tex_coord + vec2(-0.07, -0.05)).rgb)),
                       luma(texture(tex_T1, tex_coord + vec2(0.0, -0.1)).rgb));
        sh *= 1.0 - smoothstep(tune_chr_pool, tune_chr_pool * 2.5, bl) * (1.0 - chr_bounce);
      }
    }
    rock_sh = sh;
    // (the low sun's sky fill below: only where the ground is turned from the sun, not in what a
    // wall or a rock casts on ground that faces it: Spargus' streets at 18h went 2x brighter)
    gold_w = 1.0 - sh * smoothstep(0.0, 0.1, ndl) * tune_gold_shadow;
    sun_lit *= 1.0 - sh;
    // the shade is lit by the blue sky only: cooler as well as darker; the sunlit side warmer
    // and fuller (light and shadow apart)
    // (in the desert's day the shade takes the open sky's blue: lilac-grey, not brown)
    // (the desert's deep lilac; elsewhere only a little cooler: on Sandover's beach that blue
    // turned the shaded sand navy)
    float lilac = sky_day * min(sun_haze * 1.25, 1.0);
    vec3 tint = mix(mix(vec3(1.05, 0.97, 0.8), vec3(1.05, 0.95, 0.72), sky_day), vec3(1.15, 0.85, 0.3), lilac);
    // (the sun near the horizon outside the desert: the whole street is in shade, the tint became
    // the frame's color and Haven's slums went deep navy at 18-19h, red near 0: as neutral as the
    // game's own dusk light there: the shade keeps its hue)
    // (by the clock too: the areas' sun is still 20 deg up at 19h, the game's light already dusk)
    float low_sun = max(1.0 - smoothstep(0.08, 0.35, world_dir(sun_dir.xyz).y), dusk);
    tint = mix(tint, vec3(1.0), low_sun * (1.0 - lilac) * tune_dusk_shade);
    dusk_floor_w = chr ? 0.0 : low_sun * smoothstep(0.0, 0.5, sh);
    // (characters: the shade only a little cooler, never the desert's lilac: taking much more red
    // than green off yellow hair and an orange Daxter turned them olive green)
    if (chr) {
      tint = mix(tint, vec3(1.0, 0.98, 0.9), tune_chr_tint);
    }
    // (near shade on a strongly colored ground keeps that ground's hue: the lilac took 4x more red
    // than blue off the sand and the Hellcat's noon shadow was a flat blue-grey patch. Far off the
    // lilac stays, the dunes' shade across the valley)
    if (tune_shade_hue > 0.0) {
      float mx = max(c.r, max(c.g, c.b));
      float sat = mx > 1e-4 ? (mx - min(c.r, min(c.g, c.b))) / mx : 0.0;
      float hw = tune_shade_hue * smoothstep(0.1, 0.45, sat) *
                 (1.0 - smoothstep(30.0 * 4096.0, 80.0 * 4096.0, z));
      tint = mix(tint, vec3(dot(tint, vec3(1.0 / 3.0))), hw);
    }
    c *= vec3(1.0) - shadow_strength * sh * tint;
    c *= vec3(1.0) + (1.0 - sh) * smoothstep(0.0, 0.6, ndl) * sun_dir.w * (1.0 - night_dark.x) *
                     vec3(0.08, 0.07, 0.05);
  }

  // cloud shadows: each point looks up toward the sun through the same cloud field the sky draws;
  // where a cloud is in the way it's in shade. The patches drift with the clouds over the ground
  // (fainter when the sky is all cloud: then it's all shade, and the game's light is already dull)
  if (cloud_shadows > 0.0 && clouds.x > 0.0 && clouds.w < 0.6 && sky_sun.w > 0.0 && z > 0.0 &&
      wsurf == 0.0) {
    vec3 sw = world_dir(sky_sun.xyz);
    if (sw.y > 0.06) {
      vec3 wp = (view_to_world * p + cam_pos) / 4096.0;
      // (the late afternoon's heaps fewer over the desert: apart, with clear sky between)
  float lo = 0.6 - clouds.y * 0.42 + 0.03 * min(golden * 2.0, 1.0) * sun_haze;
      // (one look through the middle of the layer, counted for three: the patches are big and soft,
      // three looks cost a millisecond)
      // (through a fixed slant, not along the sun: with the layer so high the sun's own move sent the
      // patches racing over the ground as the hours passed, the whole view dark within seconds)
      float d = 3.0 * cloud_density(wp + vec3(0.3, 1.0, 0.2) * (1450.0 - wp.y), lo, 2);
      float cs = (1.0 - exp(-d * 2.5)) * min(clouds.x, 1.0) * (1.0 - clouds.w) *
                 smoothstep(0.06, 0.2, sw.y) * (1.0 - smoothstep(0.75, 0.97, clouds.y));
      // (faint in the late afternoon: the low sun's light across the valley stays whole)
      // (and neutral with the sun near the horizon outside the desert: over a whole dusk street the
      // cool tint added up with the shade's and the grade's into navy)
      float cs_low = max(1.0 - smoothstep(0.1, 0.35, sw.y), dusk) * (1.0 - sky_day * min(sun_haze * 1.25, 1.0)) * tune_dusk_shade;
      // (a cloud only takes the sun away: where the sun's shadow already has it, little is left to
      // take. Both together crushed the arena's walls at dusk to black, x0.65 then x0.69)
      cs *= mix(tune_cloud_in_shade, 1.0, sun_lit);
      c *= vec3(1.0) - 0.5 * (1.0 - 0.75 * golden) * cs * mix(vec3(1.03, 0.97, 0.86), vec3(0.97), cs_low);
    }
  }

  // the low sun's shade never under a share of the game's own color: the game lights a dusk wall
  // warm and dim, and the occlusion, the sun's and the clouds' shade stacked to x0.4 of it, the
  // arena's walls beside the lit column black (vanilla 73,53,30)
  // (here only with tune_dusk_floor_end 0: the golden and warm grade after it took its blue, see the end)
  if (tune_dusk_floor > 0.0 && dusk_floor_w > 0.0 && tune_dusk_floor_end <= 0.0) {
    vec3 game_c = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
    c = mix(c, max(c, game_c * tune_dusk_floor), dusk_floor_w);
  }
  // dark nights: where no light reaches the world goes dark (the moon's blue left in it); what
  // shines by itself (windows, fires, lamps: the brightest pixels) stays. The lights below then
  // light the surfaces in their own colors.
  // Around Jak a little is spared (he stays readable, as if the eyes were on him), and the eyes
  // adapt: a dark view is brought up some (from a few points of the scene), a view with lights in
  // it isn't, so they keep their contrast.
  vec3 albedo = c;
  // (lights.albedo, M1: the surface's own color, the texture before the game's baked light, where
  // the level wrote it. The lamps and fires light that, not the color the night already darkened:
  // their pools read on pale stone and dock wood. a = 0: none (characters, water, effects))
  if (albedo_on > 0.5 && z > 0.0 && tune_albedo_mix > 0.0) {
    vec4 alb = texture(tex_T27, tex_coord);
    if (alb.a > 0.0) {
      albedo = mix(albedo, alb.rgb * tune_albedo_gain, tune_albedo_mix);
    }
  }
  float exposure = 1.0;
  if (night_dark.x > 0.0) {
    float dk = night_dark.x;
    if (player_view.w > 0.0 && z > 0.0) {
      float pd = length(p - player_view.xyz) / 4096.0;
      dk *= 1.0 - 0.25 * (1.0 - smoothstep(0.5, 8.0, pd));
    }
    float l = luma(c);
    // (what shines by itself, by the game's own picture: the occlusion and shadows above took the
    // arena's light shaft under the threshold, and the night then turned it into a black column)
    // (the game's own picture only from a higher threshold: from 0.4 it also spared the arena's
    // lamp-lit floors, +7% over the old night; the shaft is far brighter)
    float raw_l = luma(texture(tex_T0, tex_coord).rgb);
    float keep = max(smoothstep(0.4, 0.85, l),
                     tune_night_keep_raw * smoothstep(tune_night_raw_lo, max(tune_night_raw_lo + 0.1, 0.85), raw_l));
    vec3 moon = vec3(l) * mix(vec3(0.7, 0.82, 1.1), vec3(1.1, 0.55, 0.45), sky_kind.z);
    vec3 darkened = mix(c, moon, 0.45 * dk) * (1.0 - 0.9 * dk);
    c = mix(darkened, c, keep);
    // pale ground under the moon (the slums' flagstones at 21h): the brightest thing in the frame,
    // lilac-white like snow over near-black walls, and the lamps' pools barely read on it. Up-facing
    // ground away from the lamps (their bloom) is pressed toward the walls' level above a knee, by
    // 40% at most: only darker, the lamps and windows then stand out
    if (tune_night_pale > 0.0 && z > 0.0 && wsurf == 0.0 && (tune_night_pale < 1.5 || tex_coord.x > 0.5)) {
      float upf = smoothstep(0.55, 0.8, abs(world_dir(surf_n).y));
      float pool = smoothstep(0.03, 0.15, luma(texture(tex_T1, tex_coord).rgb));
      float w = upf * (1.0 - pool) * smoothstep(0.1, 0.45, dk) * min(tune_night_pale, 1.0) * pale_ground;
      pale_w = w;
    }
    float seen = 0.0;
    for (int j = 0; j < 4; j++) {
      for (int i = 0; i < 5; i++) {
        float sl = luma(texture(tex_T0, vec2((float(i) + 0.5) / 5.0, (float(j) + 0.5) / 4.0)).rgb);
        seen += mix(sl * (1.0 - 0.9 * night_dark.x), sl, smoothstep(0.4, 0.85, sl));
      }
    }
    seen /= 20.0;
    exposure = clamp(0.05 / max(seen, 1e-3), 1.0, 1.0 + 1.2 * night_dark.x);
  }

  // (the eyes adapt to the dark, not to the lights below: on those they burned the ground white)
  c *= exposure;

  // light spill: at night what shines by itself (windows, signs, fires, the lamps' bulbs) lights
  // the surfaces near it on screen, in their own color: the bloom (the bright pixels spread wide)
  // taken as the light falling there. Not on what shines itself.
  if (light_spill > 0.0 && night_dark.x > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 spill = texture(tex_T1, tex_coord).rgb;
    float self_lit = smoothstep(0.4, 0.85, luma(albedo));
    // (an effect's colored cloud in the bloom, a fight's violet smoke around Jak, lit the whole
    // character in its hue: magenta Jak in the slums at 21h. The spill's own color held to a pale
    // tint (violet to magenta only: the windows' and fires' warm spill stays), and on characters (in front of the cloud, not lit by it) a part of it)
    float sp_top = max(spill.r, max(spill.g, spill.b));
    float sp_chroma = sp_top > 1e-4 ? (sp_top - min(spill.r, min(spill.g, spill.b))) / sp_top : 0.0;
    // (only a violet spill is cut on characters: a fire's or a window's warm side stays on them)
    float sp_violet = spill.g < min(spill.r, spill.b) ? smoothstep(0.1, 0.35, sp_chroma) : 0.0;
    if (tune_spill_sat < 1.0 && sp_chroma > tune_spill_sat && spill.g < min(spill.r, spill.b)) {
      spill = mix(vec3(luma(spill)), spill, tune_spill_sat / sp_chroma);
    }
    if (pix_tag > 0.3 && pix_tag < 0.55) {
      spill *= mix(1.0, tune_spill_chr, sp_violet);
    }
    c += albedo * min(spill, vec3(0.5)) * 1.3 * light_spill * night_dark.x * (1.0 - self_lit);
  }

  // dynamic lights: each glow lights the surfaces around it that face it, tinted by the surface's
  // own color (the scene's color stands in for its albedo), with a smooth falloff to its radius
  if ((light_count > 0 || spot_count > 0) && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surf_n;
    vec3 added = vec3(0.0);
    // per-pixel jitter of the shadow steps (the TAA averages it)
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                         haze.w * 7.13);
    vec3 from = p + nn * (0.05 * 4096.0 + z * 0.003);
    // headlights: a cone of warm white down the road out to 100 m, and a dim spill around it
    for (int i = 0; i < spot_count; i++) {
      vec3 tl = spot_pos[i].xyz - p;
      float d = length(tl);
      vec3 l = tl / max(d, 1.0);
      float facing = dot(-l, spot_dir[i].xyz);
      float dm = d / 4096.0;
      float ndl = clamp(dot(nn, l) * 0.85 + 0.15, 0.0, 1.0);
      float cone = smoothstep(0.74, 0.95, facing) + 0.12 * smoothstep(0.4, 0.8, facing);
      // gentler in the first meters (the ground under the lamps would burn white)
      float fall = 1.0 / (1.0 + dm * dm / 700.0) * (1.0 - smoothstep(65.0, 100.0, dm)) *
                   (0.4 + 0.6 * smoothstep(2.0, 12.0, dm));
      float lit = cone * fall * ndl * spot_pos[i].w;
      // shadows: rocks, walls and people block the beam (looking up to 12 m back toward the lamp,
      // stopping 3 m short of it: the vehicle itself is in the depth buffer too)
      if (light_shadow > 0.0 && lit > 0.01 && dm > 4.0) {
        float reach = min(d - 3.0 * 4096.0, 12.0 * 4096.0);
        lit *= 1.0 - light_shadow * light_blocked_by(from, spot_pos[i].xyz, jitter, reach, 10);
      }
      added += vec3(1.0, 0.9, 0.72) * lit * 4.0;
    }
    vec3 glints = vec3(0.0);  // the lights' highlights on wet ground
    bool full_day = night_dark.x <= 0.0 && sky_sun.w > 0.05 && clouds.w < 0.3 && weather.x <= 0.0;
    vec3 vdir = normalize(-p);
    // and on dry ground: polished stone, tiles and metal catch a torch too (by the material class in
    // the normals' alpha: metal, glossy, else a little for plain stone; never the characters)
    vec3 dry = vec3(0.0);
    float gma = pix_normal.a;
    int gmat = gma > 0.83 ? 0 : (gma > 0.5 ? 1 : (gma > 0.17 ? 2 : 0));
    float gloss = tune_dry_glints <= 0.0 || (pix_tag > 0.3 && pix_tag < 0.55) ? 0.0
                : (gmat == 1 ? 1.0 : (gmat == 2 ? 0.6 : tune_glint_stone));
    float gexp = aa_exp(mix(24.0, 60.0, gloss), n_var);
    float wexp = aa_exp(70.0, n_var);
    // (Jak's own shadow from a lamp on the floor right around him: the depth march blurs his body into
    // a dark box twice his width under his feet at night (the port's pier); his contact shadow is there
    // for that, as vanilla's blob)
    float lamp_jak_keep = 1.0;
    vec3 flash_add = vec3(0.0);  // the explosions' and shots' flash, apart from the lamps
    if (tune_lamp_jak_m > 0.0 && dot(jak_fx.xyz, jak_fx.xyz) > 4096.0 * 4096.0) {
      vec3 jd = (p - jak_fx.xyz) / 4096.0;
      float jup = dot(jd, up_view);
      float jr = length(jd - up_view * jup);
      lamp_jak_keep = smoothstep(0.5 * tune_lamp_jak_m, tune_lamp_jak_m, jr) + smoothstep(0.4, 1.0, abs(jup));
      lamp_jak_keep = min(lamp_jak_keep, 1.0);
    }
    // (light tiles: only the lights whose reach may touch this pixel's 32x18 screen tile, lowest
    // index first as before, so the strongest still cast the shadows)
    uint lbits = light_count >= 32 ? 0xffffffffu : (1u << uint(light_count)) - 1u;
#ifdef ENH_LIGHT_TILES
    if (light_tiles_on > 0.5) {
      ivec2 lt = clamp(ivec2(tex_coord * vec2(32.0, 18.0)), ivec2(0), ivec2(31, 17));
      lbits &= texelFetch(tex_T28, lt, 0).r;
    }
#endif
    while (lbits != 0u) {
      int i = findLSB(lbits);
      lbits &= lbits - 1u;
      vec3 to_light = light_pos[i].xyz - p;
      float d = length(to_light);
      float r = light_pos[i].w;
      if (d < r) {
        float k = 1.0 - (d * d) / (r * r);
        float ndl = dot(soft_n, to_light / max(d, 1.0));
        // (a light Jak carries stands low over the ground: mostly wrapped around the shapes, or it
        // draws every triangle of the terrain and leaves the steps black)
        bool flash = light_color[i].a > 1.5 && tune_flash > 0.0;
        bool lamp = light_color[i].a < 0.5 || light_color[i].a > 1.5;  // (2: a flash, lit as a lamp)
        float lit = lamp ? clamp(ndl * 0.85 + 0.15, 0.0, 1.0)
                                 : clamp(ndl * 0.3 + 0.7, 0.0, 1.0);
        // the strongest lights (sorted first) cast shadows where they still matter
        // (not a light Jak carries: alpha 1, his own shadow from it is a blur across the screen)
        // (not in full daylight: a torch's shadow doesn't show under the sun)
        if (light_shadow > 0.0 && i < 8 && k * k * lit > 0.02 && lamp && !full_day) {
          lit *= 1.0 - light_shadow * light_blocked(from, light_pos[i].xyz, jitter) * lamp_jak_keep;
        }
        added += light_color[i].rgb * (k * k) * lit * 1.35;
        if (flash) {
          flash_add += light_color[i].rgb * (k * k) * lit * 1.35;
        }
        if (wet > 0.0) {
          vec3 hv = normalize(to_light / max(d, 1.0) + vdir);
          glints += light_color[i].rgb * k * pow(max(dot(nn, hv), 0.0), wexp) * lit;
        }
        if (gloss > 0.0) {
          vec3 hv = normalize(to_light / max(d, 1.0) + vdir);
          dry += light_color[i].rgb * (k * k) * lit * pow(max(dot(nn, hv), 0.0), gexp) * (gexp + 8.0) / 32.0;
        }
      }
    }
    if (gloss > 0.0) {
      // (metal tinted by its own color; rolled off, a few torches don't burn a white hole)
      dry *= gloss * (gmat == 1 ? mix(vec3(1.0), albedo * 2.0, 0.6) : vec3(1.0));
      dry /= 1.0 + max(dry.r, max(dry.g, dry.b));
      // (never far past the surface as lit: grain times a glint lit one pixel of dark stone white)
      if (tune_spec_clamp > 0.0) {
        dry = min(dry, vec3(tune_spec_clamp * (luma(c) + luma(added) * (luma(c) + 0.12)) + 0.01));
      }
      c += dry * ao * tune_dry_glints * (1.0 - min(wet, 1.0));
    }
    // many lamps together (a factory's red floods) saturate softly instead of adding up without end
    added /= 1.0 + mix(0.35, 0.8, night_dark.x) * max(added.r, max(added.g, added.b));
    if (night_dark.x > 0.0) {
      // at night the light shows the surface's own color, brighter
      // (the game tints the night blue: the surface's own color, warmer, under a lamp)
      // (only near the flame: the faint outer part of a lamp's pool loses its hue toward the moon's,
      // as dim light does to the eye. Jak's lantern and Spargus' torches laid salmon over all the
      // moonlit ground in front of the camera, where the game has blue; tune_night_lamp_cool 0: off)
      float lamp_warm = mix(1.0, smoothstep(tune_night_lamp_lo, tune_night_lamp_hi, luma(added)),
                            tune_night_lamp_cool * night_dark.x);
      added = mix(vec3(luma(added)) * vec3(0.86, 0.96, 1.12), added, lamp_warm);
      vec3 own = albedo * mix(vec3(1.0), vec3(1.3, 1.05, 0.72), night_dark.x * lamp_warm);
      c += (own + 0.12) * added * ao;
    } else {
      c += (c + 0.12) * added * ao;
    }
    c += glints * wet * 1.6 * (1.0 + night_dark.x) * wet_gloss;
    // (an explosion's flash: the lamps' soft ceiling above kept it to a torch's light, and in a
    // torch-lit room at night a blast lit nothing. On top of that, its own much higher roll-off:
    // the room flashes)
    float fm = max(flash_add.r, max(flash_add.g, flash_add.b));
    if (fm > 0.0) {
      flash_add *= tune_flash / (1.0 + 0.25 * fm * tune_flash);
      c += (night_dark.x > 0.0 ? albedo * mix(1.0, 1.6, night_dark.x) : c) * flash_add * ao +
           0.12 * flash_add * ao;
    }
  }

  // indirect light: what the surroundings send, tinted by this surface's own color
  if (gi_strength > 0.0 && wsurf == 0.0) {
    vec3 gi = texture(tex_T7, tex_coord).rgb;
    c += gi * gi_strength * (0.35 + c) * ao * (1.0 - 0.8 * night_dark.x);
  }

  // Jak's contact shadow: the ground right under his feet darker (indoors, by torchlight and at
  // night nothing else shades it: no sun shadow, the point lights' march is short and the traced
  // AO knows only the level, so he floated). Soft, about his footprint; less where the sun's map
  // already draws his shadow
  if (tune_chr_contact > 0.0 && z > 0.0 && wsurf == 0.0 && !(pix_tag > 0.3 && pix_tag < 0.55) &&
      dot(jak_fx.xyz, jak_fx.xyz) > 4096.0 * 4096.0 && z < 40.0 * 4096.0) {
    vec3 dv = (p - jak_fx.xyz) / 4096.0;  // meters
    float up_m = dot(dv, up_view);
    float r_m = length(dv - up_view * up_m);
    float blob = (1.0 - smoothstep(0.2, 0.75, r_m)) * (1.0 - smoothstep(0.15, 0.5, abs(up_m)));
    float in_sun = sun_dir.w > 0.0 ? sun_lit * clamp(sun_dir.w * 4.0, 0.0, 1.0) : 0.0;
    c *= 1.0 - tune_chr_contact * blob * (1.0 - 0.6 * in_sun) * (chr_occ_on > 0.0 && tune_chr_occ > 0.0 ? 0.4 : 1.0);
  }

  // low-sun-sheen: looking toward a low sun, level stone and flagstones catch a broad grazing sheen
  // and a rough highlight (the sand has its own, below). Only in the sun, only with it low; held
  // under the surface's own brightness so the dark look stays (tune_low_sheen 0: off)
  if (tune_low_sheen > 0.0 && sun_dir.w > 0.0 && sun_lit > 0.0 && z > 0.0 && wsurf == 0.0 &&
      z < 150.0 * 4096.0 && pix_tag > 0.96 && night_dark.x < 1.0) {
    vec3 lw = normalize(world_dir(sun_dir.xyz));
    vec3 wn = normalize(world_dir(surf_n));
    float lowk = (1.0 - smoothstep(0.1, 0.45, lw.y)) * smoothstep(0.0, 0.03, lw.y) * smoothstep(0.55, 0.85, wn.y);
    if (lowk > 0.0) {
      float ga = pix_normal.a;
      int smat = ga > 0.83 ? 0 : (ga > 0.5 ? 1 : (ga > 0.17 ? 2 : 0));
      vec3 vw = -normalize(world_dir(p));  // toward the eye
      float fres = pow(1.0 - clamp(dot(vw, wn), 0.0, 1.0), 5.0);
      float toward = pow(max(dot(normalize(-vw.xz + 1e-4), normalize(lw.xz + 1e-4)), 0.0), 3.0);
      float sr = max(dot(reflect(-vw, wn), lw), 0.0);
      float lobe = pow(sr, aa_exp(18.0, n_var)) * (0.05 + 0.6 * fres);
      float k = (fres * toward * 0.7 + lobe) * (smat == 2 ? 1.6 : (smat == 1 ? 0.6 : 1.0));
      vec3 sheen = sun_color() * k * lowk * sun_lit * sun_dir.w * ao * (1.0 - night_dark.x) *
                   (1.0 - min(wet, 1.0)) * tune_low_sheen;
      c += min(sheen, vec3(luma(c) * 0.5 + 0.01));
    }
  }

  // char-ground-occlusion: every character around Jak darkens what lies just below it (in the temple
  // at noon and in the slums at night they stood on the flagstones pasted on). Their lowest point per
  // column, seen from below: 8 taps over a soft reach, each by the gap down to this surface; less
  // where the sun map already draws their shadow
  if (tune_chr_occ > 0.0 && chr_occ_on > 0.0 && z > 0.0 && wsurf == 0.0 &&
      !(pix_tag > 0.3 && pix_tag < 0.55) && z < 30.0 * 4096.0) {
    vec3 s = (chr_occ_mvp * vec4(view_to_world * p + cam_pos, 1.0)).xyz * 0.5 + 0.5;
    if (all(greaterThan(s, vec3(0.0))) && all(lessThan(s, vec3(1.0)))) {
      // (the map's half side in m: the mvp's x scale is 1 / half in game units)
      float half_m = 1.0 / (chr_occ_mvp[0][0] * 4096.0);
      float rr = tune_chr_occ_r / (2.0 * half_m);
      float rot = pixel_angle();
      float occ = 0.0;
      for (int i = 0; i < 8; i++) {
        float d = texture(tex_T29, s.xy + vogel(i, 8, rot) * rr).r;
        float gap = (d - s.z) * chr_occ_on;  // m: their lowest point over this surface
        occ += d < 0.9999 ? step(-0.12, gap) * (1.0 - smoothstep(0.0, tune_chr_occ_h, gap)) : 0.0;
      }
      occ /= 8.0;
      float in_sun = sun_dir.w > 0.0 ? sun_lit * clamp(sun_dir.w * 4.0, 0.0, 1.0) : 0.0;
      c *= 1.0 - tune_chr_occ * occ * (1.0 - 0.7 * in_sun);
    }
  }

  // materials (from the texture names, in the normals' alpha): metal reflects its surroundings
  // tinted by its own color with a broad highlight; glass, marble and tiles reflect them untinted
  // at grazing angles with a tight highlight
  if (materials < 0.0 && z > 0.0) {
    float ga = pix_normal.a;
    int mat = ga > 0.83 ? 0 : (ga > 0.5 ? 1 : (ga > 0.17 ? 2 : 0));
    // (and the finer classes: rusty orange, painted pink, tile cyan, wood brown, walls green)
    float ml = pix_tag;
    int mf = ml > 0.9975 || ml <= 0.985 ? 0 : (ml > 0.9925 ? 1 : 2);
    c = mat == 1 ? (mf == 1 ? vec3(1.0, 0.5, 0.05) : (mf == 2 ? vec3(1.0, 0.3, 0.8) : vec3(1.0, 0.1, 0.1)))
                 : (mat == 2 ? (mf == 1 ? vec3(0.1, 0.9, 0.9) : vec3(0.1, 0.3, 1.0))
                             : (mf == 1 ? vec3(0.45, 0.25, 0.1) : (mf == 2 ? vec3(0.2, 0.7, 0.2) : c * 0.4)));
  }
  float mk = materials > 1.5 ? (tex_coord.x < 0.5 ? 1.0 : 0.0) : materials;
  if (mk > 0.0 && z > 0.0 && z < 120.0 * 4096.0 && wsurf == 0.0) {
    float ga = pix_normal.a;
    int mat = ga > 0.83 ? 0 : (ga > 0.5 ? 1 : (ga > 0.17 ? 2 : 0));
    if (mat != 0) {
      // the finer class (tfrag3.frag), the normal's length a step under 1.0: metal 1 rusty, 2
      // painted; glossy 1 tile
      float mlen = pix_tag;
      int mfine = mlen > 0.9975 || mlen <= 0.985 ? 0 : (mlen > 0.9925 ? 1 : 2);
      vec3 nn = surf_n;
      vec3 vdir = normalize(p);
      // how rough each is: bare metal the most, then tile and paint; glass stays the clearest
      float rough = (mat == 1 ? (mfine == 2 ? 0.07 : 0.13) : (mfine == 1 ? 0.08 : 0.035)) * reflect_rough;
      vec3 r = rough_reflection(p, vdir, nn, z, sqrt(rough * rough + min(n_var, tune_spec_aa_max)));
      float facing = clamp(dot(-vdir, nn), 0.0, 1.0);
      float hit;
      // (rust reflects nothing: no trace)
      vec3 refl = mat == 1 && mfine == 1 ? c : trace_reflection(p, r, z, hit);
      float fade = (1.0 - smoothstep(60.0 * 4096.0, 120.0 * 4096.0, z)) * reflect_amount;
      // specular occlusion (Lagarde): crevices and undersides kept a sky-bright reflection, plastic in
      // the shaded corners; the AO takes it away, more where the gloss is rough
      fade *= mix(1.0, clamp(pow(facing + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0), tune_spec_occ);
      // the sun's highlight, where the sun reaches
      float sunlit = 0.0;
      if (sun_dir.w > 0.0 && dot(nn, sun_dir.xyz) > 0.0) {
        sunlit = 1.0;
        if (shadow_strength > 0.0) {
          sunlit -= sun_shadow(view_to_world * (p + nn * (0.08 * 4096.0 + z * 0.002)) + cam_pos);
        }
        sunlit *= ao;
      }
      float sr = max(dot(r, sun_dir.xyz), 0.0);
      float lum = max(dot(c, vec3(0.3, 0.59, 0.11)), 0.02);
      if (mat == 1 && mfine == 1) {
        // rust: only a dull, wide sheen in its own color
        c += c * (clamp(c / lum, 0.0, 2.0) * 0.5 + 0.5) * pow(sr, 6.0) * 0.25 * sunlit * mk;
      } else if (mat == 1 && mfine == 2) {
        // paint: a coat over the color: the surroundings only at grazing angles, untinted
        float f = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
        c = mix(c, refl, clamp(f * 0.5 + 0.05, 0.0, 1.0) * mk * fade);
        c += vec3(1.0, 0.97, 0.92) * (pow(sr, 120.0) * 0.7 + pow(sr, 20.0) * 0.06) * sunlit * mk;
      } else if (mat == 1) {
        vec3 tint = clamp(c / lum, 0.0, 2.0) * 0.5 + 0.5;
        float f = 0.5 + 0.5 * pow(1.0 - facing, 5.0);
        c = mix(c, refl * tint * (0.5 + lum), f * 0.55 * mk * fade);
        c += c * tint * (pow(sr, 64.0) * 2.0 + pow(sr, 12.0) * 0.45) * sunlit * mk;
      } else if (mfine == 1) {
        // tile: glazed, softer than glass
        float f = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
        c = mix(c, refl, clamp(f * 0.35 + 0.03, 0.0, 1.0) * mk * fade);
        c += vec3(1.0, 0.95, 0.85) * pow(sr, 150.0) * sunlit * mk;
      } else {
        float f = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
        c = mix(c, refl, clamp(f * 0.45 + 0.06, 0.0, 1.0) * mk * fade);
        c += vec3(1.0, 0.95, 0.85) * pow(sr, 300.0) * 2.0 * sunlit * mk;
      }
      // the lamps' highlights on it (a factory's pipes, a car's paint at night): metal's broad and
      // tinted by the metal, gloss tight and in the lamp's own color
      for (int i = 0; i < light_count; i++) {
        vec3 tl = light_pos[i].xyz - p;
        float d = length(tl);
        float rr = d / max(light_pos[i].w, 1.0);
        if (rr < 1.6) {
          float lr = max(dot(r, tl / max(d, 1.0)), 0.0);
          float near = 1.0 - smoothstep(0.5, 1.6, rr);
          vec3 tint = mat == 1 && mfine != 2 ? clamp(c / lum, 0.0, 2.0) * 0.5 + 0.5 : vec3(1.0);
          float sp = mat == 1 ? (mfine == 1 ? pow(lr, 8.0) * 0.25 : (mfine == 2 ? pow(lr, 80.0) * 0.8 : pow(lr, 40.0) * 0.9))
                              : (mfine == 1 ? pow(lr, 120.0) : pow(lr, 150.0) * 1.6);
          c += light_color[i].rgb * tint * sp * near * mk * fade * (1.0 + night_dark.x);
        }
      }
    }
  }

  // wet ground: puddles in the dips of a world-space noise, raindrop rings in them, and the
  // surroundings reflected (strongest in the puddles and at grazing angles: the neon on a wet street)
  if (wet > 0.01 || wet_flat * puddle_level > 0.01) {
    vec3 ww = (view_to_world * p + cam_pos) / 4096.0;
    // wide puddles: low-frequency dips with a ragged rim. A thunderstorm floods them wider; as they
    // dry they draw back into the deepest dips, still water while the street around has dried
    float pd = gnoise(ww.xz * 0.22) + 0.45 * gnoise(ww.xz * 0.7 + 7.1) + 0.12 * gnoise(ww.xz * 3.1 + 2.3);
    float spread = 0.22 * weather2.x - 0.3 * (1.0 - puddle_level);
    float puddle = smoothstep(-0.12 - spread, 0.02 - spread, pd) * smoothstep(0.85, 0.95, dot(wet_n, up_view)) *
                   (1.0 - smoothstep(350.0 * 4096.0, 700.0 * 4096.0, z)) * step(0.001, puddle_level);
    float soaked = wet;
    float water = min(puddle_level * 3.0, 1.0);
    wet = max(wet, puddle * wet_flat * water);
    // (a damp dark rim around a drying puddle)
    float rim = smoothstep(-0.2 - spread, -0.12 - spread, pd) * wet_flat * water * (1.0 - puddle);
    c *= 1.0 - 0.25 * puddle * wet - 0.3 * max(wet - soaked, 0.0) - 0.15 * max(rim - soaked, 0.0);
    vec2 rip = rain_ripples(ww.xz, weather.z) * weather.x * (1.0 - smoothstep(8.0 * 4096.0, 20.0 * 4096.0, z));
    // outside the puddles the wet ground is rough: a fine noise breaks its reflection up
    vec2 rough = vec2(gnoise(ww.xz * 9.0), gnoise(ww.xz * 9.0 + 5.3)) * (1.0 - puddle) *
                 (1.0 - smoothstep(10.0 * 4096.0, 30.0 * 4096.0, z));
    float tw = weather.z;
    vec2 wave = vec2(gnoise(ww.xz * 2.2 + vec2(tw * 0.35, tw * 0.2)),
                     gnoise(ww.xz * 2.2 + vec2(5.0 - tw * 0.25, tw * 0.3))) * 0.07 +
                vec2(gnoise(ww.xz * 7.0 + tw * 0.6), gnoise(ww.xz * 7.0 + 3.3 - tw * 0.5)) * 0.025;
    vec2 bend = rip * (0.15 + 0.6 * puddle) + rough * 0.35 + wave * puddle;
    if (splash_count > 0 && z < 25.0 * 4096.0) {
      bend += step_ripples(ww.xz) * puddle;
    }
    vec3 n2 = normalize(wet_n + right_view * bend.x + fwd_view * bend.y);
    vec3 vdir = normalize(p);
    vec3 r = reflect(vdir, n2);
    float facing = clamp(dot(-vdir, n2), 0.0, 1.0);
    float f = 0.02 + 0.98 * pow(1.0 - facing, 5.0);
    float hit;
    vec3 refl = trace_reflection(p, r, z, hit);
    // a sheen, not a mirror: the ground's own color stays, dimmer reflections outside the puddles
    refl = mix(refl, c, 0.25 * (1.0 - puddle));
    float amount = wet * (0.2 + 0.4 * puddle) * clamp(f * 1.3 + 0.12 * puddle + 0.03, 0.0, 1.0) *
                   mix(wet_gloss, 1.0, puddle);
    // (specular occlusion, as on the glossy materials: no sky in a wet crevice)
    amount *= mix(1.0, clamp(pow(facing + ao, exp2(-16.0 * 0.1 - 1.0)) - 1.0 + ao, 0.0, 1.0), tune_spec_occ);
    c = mix(c, refl, amount);
    // splashes: drops hitting the ground, each a bright crown spreading for a tenth of a second
    if (weather.x > 0.0 && z < 15.0 * 4096.0) {
      float spl = 0.0;
      for (int k = 0; k < 4; k++) {
        float cs = k == 0 ? 0.3 : 0.45;
        vec2 cell = floor(ww.xz / cs + float(k) * 0.5);
        float h = hash1(cell + float(k) * 13.0);
        float cyc = weather.z * (2.2 + h) + h * 9.0;
        float ph = fract(cyc);
        if (hash1(cell + floor(cyc) * 1.91) < 0.5) {
          vec2 ctr = (cell + 0.2 + 0.6 * vec2(fract(h * 7.3), fract(h * 31.7)) - float(k) * 0.5) * cs;
          float dd = length(ww.xz - ctr);
          float rad = 0.015 + ph * 0.07;
          float life = 1.0 - smoothstep(0.0, 0.12, ph * 0.45);
          spl += exp(-sq((dd - rad) / 0.01)) * life + exp(-dd * dd / 0.0002) * life * 0.6;
        }
      }
      float fade = 1.0 - smoothstep(6.0 * 4096.0, 15.0 * 4096.0, z);
      c += (fog_color * 0.5 + 0.22) * spl * fade * weather.x * 0.55;
    }
    // further out: the drops as sparkles about a pixel big, on what faces up, out to 350 m
    if (weather.x > 0.0 && z > 6.0 * 4096.0 && z < 350.0 * 4096.0) {
      float spk = rain_sparkles(ww.xz, weather.z, z / 4096.0 * pixel_rad);
      float fade = smoothstep(6.0 * 4096.0, 15.0 * 4096.0, z) *
                   (1.0 - smoothstep(220.0 * 4096.0, 350.0 * 4096.0, z)) *
                   smoothstep(0.7, 0.9, dot(wet_n, up_view));
      c += (fog_color * 0.5 + 0.22) * min(spk, 1.0) * fade * weather.x * 0.6;
    }
  }

  // relief: the texture's light and dark read as small bumps (bright = raised), lit by the sun:
  // the normal tilts along the brightness slope on the surface; near only, not across edges
  float rk = relief > 1.5 ? (tex_coord.x < 0.5 ? 1.0 : 0.0) : relief;
  if (rk > 0.0 && sun_dir.w > 0.0 && z > 0.0 && z < 30.0 * 4096.0 && wsurf == 0.0) {
    vec2 tx = vec2(texel.x, 0.0), ty = vec2(0.0, texel.y);
    float dr = texture(tex_T3, tex_coord + tx).r, dl = texture(tex_T3, tex_coord - tx).r;
    float du = texture(tex_T3, tex_coord + ty).r, dd = texture(tex_T3, tex_coord - ty).r;
    vec3 pr = view_pos(tex_coord + tx, dr), pl = view_pos(tex_coord - tx, dl);
    vec3 pu = view_pos(tex_coord + ty, du), pd = view_pos(tex_coord - ty, dd);
    float span = 0.004 * z + 0.05 * 4096.0;
    if (abs(pr.z - pl.z) < span && abs(pu.z - pd.z) < span) {
      vec3 nn = surf_n;
      float gx = luma(texture(tex_T0, tex_coord + tx).rgb) - luma(texture(tex_T0, tex_coord - tx).rgb);
      float gy = luma(texture(tex_T0, tex_coord + ty).rgb) - luma(texture(tex_T0, tex_coord - ty).rgb);
      vec3 t = normalize(pr - pl), b = normalize(pu - pd);
      // slope per pixel, a little more where the surface is seen at an angle (its texels stretch)
      vec3 nb = normalize(nn - (t * gx + b * gy) * 1.6);
      float fade = 1.0 - smoothstep(12.0 * 4096.0, 30.0 * 4096.0, z);
      float shade = dot(nb, sun_dir.xyz) - dot(nn, sun_dir.xyz);
      c *= 1.0 + clamp(shade, -0.5, 0.5) * 0.75 * rk * fade * ao;
    }
  }

  // sun relight: the game's light is baked per vertex; shade each pixel a little more by how it
  // faces the sun, with normals rebuilt from the depth buffer (the levels have none)
  if (relight > 0.0 && sun_dir.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    vec3 nn = surf_n;
    float ndl = dot(nn, sun_dir.xyz);
    // (characters, merc2.frag's 0.45-long normals: the game shades their dark side already)
    float ctag = pix_tag;
    if (ctag > 0.3 && ctag < 0.55) {
      ndl = max(ndl, -0.1);
    }
    float k = relight * (1.0 - smoothstep(150.0 * 4096.0, 300.0 * 4096.0, z));
    c *= 1.0 + k * clamp(ndl, -0.6, 1.0) * 0.35;
  }

  // the sky: ours by day over the game's (its painted clouds go), the game's at night; the lightning
  // bolt behind the clouds; the cloud volume over both; greyer under rain
  // (the game's painted cloud cards kilometers out above the horizon count as sky: ours replaces
  // them by day, by night they're smoothed into the game's sky)
  float bolt_light = 0.0;
  float sky_kept = 0.0;  // the game's lights over its night sky, kept as drawn
  vec3 sky_light = vec3(0.0);
  float sky_k = z == 0.0 ? 1.0
                         : smoothstep(2800.0 * 4096.0, 4000.0 * 4096.0, z) *
                               smoothstep(0.04, 0.09, world_dir(ray).y) * step(0.0, clouds.x - 0.001);
  // (under a roof the game's own sky through the windows: ours, pale and bright at noon, filled the
  // palace's honeycomb panes between their lines, a white pane where vanilla shows the clouds)
  sky_k *= 1.0 - enclosed * tune_sky_enclosed;
  if (sky_k > 0.0 && wsurf == 0.0) {
    vec3 c0 = c;
    if (clouds.x > 0.0) {
      vec3 sky = sky_color(ray);
      // (the desert's morning: ours, not the game's brown dawn sky)
      float game_w = clouds.w * (1.0 - sky_day);
      c = mix(c, sky, clouds.x * (1.0 - game_w));
      // the game's own sky by night, dawn and dusk: its painted cloud cards (far geometry, here as
      // sky at any hour) smoothed away: sharp dark cut-outs drifting across it; its colors and its
      // gradient kept, ours are the clouds
      if (game_w > 0.0) {
        c = mix(c, game_sky_around(uv, c, vec2(0.3, 0.035)), clouds.x * game_w);
      }
      vec4 cl = cloud_info.x > 0.0 ? cloud_fetch(uv) : vec4(0.0, 0.0, 0.0, -1.0);
      if (cl.a < 0.0) {
        cl = cloud_volume(ray, pixel_angle() / 6.2832, mix(sky, c, game_w));
      }
      c = mix(c, c * cl.a + cl.rgb, clouds.x);
      // (the sprites in front of it: the chimneys' smoke, glows)
      // (the sun's and moon's glows among them: their faint outskirts stopped at the quad's edge, a
      // square over our sky, so the faintest light is let go; and their core, clipped white in the
      // game's picture, had nothing left to add over a sky as bright: a hollow lilac ring. Where the
      // sprites clipped the game's sky to white it stays white)
      vec3 fr_pos = max(fx_rest, 0.0);
      vec3 fr = fx_rest - fr_pos + max(fr_pos - tune_fx_sky_edge, 0.0) * (1.0 + tune_fx_sky_edge * 4.0);
      c = max(c + fr * clouds.x * (1.0 - game_w), 0.0);
      if (fx_day > 0.0 && z == 0.0 && tune_fx_sky_core > 0.0) {
        vec3 f1s = textureLod(tex_T24, uv, 0.0).rgb;
        float core = smoothstep(0.93, 0.99, min(f1s.r, min(f1s.g, f1s.b))) * smoothstep(0.01, 0.04, luma(fx_all));
        c = max(c, vec3(tune_fx_sky_core * core * clouds.x * (1.0 - game_w)));
      }
      // (and what the game drew over its sky without depth: the palace's honeycomb windows, glass
      // and grates, went with the sky, blown to a white pane. Their lines put back over ours: where
      // the levels changed the game's sky (sky_over), and sharply, against what they did 3 px
      // around. Not by the picture's own contrast: that also took every sky pixel at a mountain's
      // edge (grey halos) and the game's painted cloud wisps. A soft cloud card the level draws
      // changes the sky evenly; a mountain's pixels around don't count)
      if (z == 0.0 && tune_sky_keep > 0.0 && sky_copy > 0.0) {
        vec3 dc = sky_over(uv);
        float sharp = 0.0;
        for (int i = 0; i < 4; i++) {
          vec2 q = uv + vec2(i == 0 ? 3.0 : i == 1 ? -3.0 : 0.0, i == 2 ? 3.0 : i == 3 ? -3.0 : 0.0) * texel;
          if (textureLod(tex_T3, q, 0.0).r > 1e-7) {
            continue;
          }
          sharp = max(sharp, length(sky_over(q) - dc));
        }
        float keep = smoothstep(tune_sky_keep_lo, tune_sky_keep_hi, min(length(dc), sharp)) *
                     clouds.x * (1.0 - game_w) * tune_sky_keep;
        c = mix(c, c0, keep);
      }
      // (by night the same for the game's own sky: smoothing its cloud cards away took the lights
      // drawn over it too (Haven's tower beacon, its searchlights, the tower's lamps). Kept where
      // something brighter than the sky was drawn over it, sprites included (the game's whole
      // picture), added back over the night sky below (light: replacing the pixel drew a dark box
      // around the beacon's glow). The cards are darker cut-outs, and a sharpness test lost the
      // beacon's even core: a hollow ring)
      if (z == 0.0 && tune_sky_keep_night > 0.0 && sky_copy > 0.0 && game_w > 0.0) {
        vec3 dc = max(textureLod(tex_T0, uv, 0.0).rgb - textureLod(tex_T25, uv, 0.0).rgb, 0.0);
        sky_kept = smoothstep(tune_sky_night_lo, tune_sky_night_hi, max(dc.r, max(dc.g, dc.b))) *
                   clouds.x * game_w * tune_sky_keep_night;
        sky_light = dc * sky_kept;
      }
      if (bolt.w > 0.0 && strike.w == 0.0) {
        bolt_light = d <= 1e-7 ? sky_lightning(ray, pixel_rad) : 0.0;
      }
    }
    c = mix(c, vec3(luma(c)) * 0.8 * vec3(0.82, 0.94, 1.15), weather.x * 0.3);
    c = mix(c0, c, sky_k);
    bolt_light *= sky_k;
    if (sky_k > 0.5) {
      z = 0.0;  // the weather below treats it as sky
    }
  }
  // a bolt striking the ground: over the sky and whatever is behind it
  if (strike.w > 0.0 && bolt.w > 0.0 && wsurf == 0.0 && (z == 0.0 || z > strike.z * 0.97)) {
    bolt_light = max(bolt_light, sky_lightning(ray, pixel_rad));
  }
  // the night sky: stars, the moon, a city's glow (over the game's sky)
  if (clouds.w > 0.01 && z == 0.0 && wsurf == 0.0) {
    vec3 base = c;
    c *= 1.0 - 0.75 * night_dark.x;
    c += night_sky(ray, base, pixel_rad);
    // (a faint glow along the horizon, as a night sky has: the mountains' and the dunes'
    // silhouettes stand out against it instead of going black on black)
    float up_n = world_dir(ray).y;
    c += tune_night_horizon * vec3(0.55, 0.7, 1.0) * night_dark.x *
         smoothstep(-0.02, 0.03, up_n) * (1.0 - smoothstep(0.03, 0.3, up_n));
    c *= 1.0 - 0.5 * sky_kept;  // (the stars behind a beam dimmed)
  }
  c += sky_light;
  // the rainbow, where the sky shows past the rain (or far behind)
  if (weather2.z > 0.0 && sky_sun.w > 0.03 && wsurf == 0.0 && (z == 0.0 || z > 150.0 * 4096.0)) {
    float up = world_dir(ray).y;
    c += rainbow(ray) * weather2.z * 0.13 * smoothstep(-0.02, 0.06, up) *
         clamp(1.0 - sky_sun.w * 1.6, 0.0, 1.0) * (1.0 - clouds.w);
  }

  // fog: thicker with distance and below the camera
  if (fog.z > 0.0 && z > 0.0) {
    float amount = 1.0 - exp(-z * fog.x);
    amount *= clamp(exp(-height * fog.y), 0.0, 1.0);
    vec3 fc = mix(fog_color, vec3(luma(fog_color)) * vec3(0.82, 0.94, 1.15), 0.55 * weather.x);
    fc *= 1.0 - 0.85 * night_dark.x;
    fc += vec3(1.0, 0.55, 0.25) * city_glow * 0.06 * clamp(exp(-height * fog.y), 0.0, 1.0);
    c = mix(c, fc, amount * fog.z);
  }
  // the desert's air: a warm haze lit by the sun, thicker with distance, glowing toward the sun;
  // over the low sky too (the horizon goes golden), not up high
  if (sun_haze > 0.0 && wsurf == 0.0) {
    vec3 c_pre_haze = c;
    float dist = z > 0.0 ? length(p) / 4096.0 : 5000.0;
    float up = max(world_dir(ray).y, 0.0);
    float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w * (1.0 - day_look)) * (1.0 - weather.x);
    float toward = max(dot(ray, sky_sun.xyz), 0.0);
    // the mountains' shadows in the haze (the god rays pass, alpha), stronger toward the sun
    float lit = 1.0;
    if (haze_shadow > 0.0) {
      // (smoothed over a few of its texels: the march's noise)
      vec2 st = 2.0 / vec2(textureSize(tex_T4, 0));
      float a = texture(tex_T4, uv).a * 0.2 +
                (texture(tex_T4, uv + vec2(st.x, st.y)).a + texture(tex_T4, uv + vec2(-st.x, st.y)).a +
                 texture(tex_T4, uv + vec2(st.x, -st.y)).a + texture(tex_T4, uv + vec2(-st.x, -st.y)).a) * 0.2;
      lit = 1.0 - (1.0 - a) * (0.55 + 0.45 * toward);
    }
    vec3 hc = mix(fog_color, sun_color() * vec3(1.0, 0.76, 0.48), 0.85);
    // (at noon a pale glare, the heat's white haze, not the evening's gold)
    // (a little of it: more washed the noon's colors out, the mountains to a pale beige)
    hc = mix(hc, vec3(0.9, 0.88, 0.83) * max(luma(hc) * 1.25, 0.55), desert_noon() * 0.5);
    // (the morning's air: warm toward the sun, a pale blue-white away from it; not the game's pink dawn fog)
    {
      vec2 shm = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
      float azm = 0.5 + 0.5 * dot(normalize(world_dir(ray).xz + 1e-4), shm);
      hc = mix(hc, mix(vec3(0.76, 0.85, 0.96), vec3(1.0, 0.84, 0.64), azm * azm * clamp(morning / max(day_look, 1e-3), 0.0, 1.0)), day_look);
    }
    hc *= (0.9 + (0.5 * toward * toward + 0.6 * pow(toward, 12.0)) * lit) * mix(0.62, 1.0, lit);
    // and dust hanging low over the sand far off: the feet of the mountains and the far dunes
    // fade into it, their tops stand out (height over the camera's, m)
    float rel = z > 0.0 ? world_dir(p).y / 4096.0 : 0.0;
    // (the haze and the dust are layers over the ground: seen from high above (the Hellcat, a cliff)
    // a ray crosses only their thickness, not its whole length. Without this, in flight every term
    // took the whole way down as far air and the desert went one pale beige, the rocks chalk white)
    float drop = tune_haze_layer > 0.5 && (tune_haze_layer < 1.5 || tex_coord.x > 0.5) ? max(-rel, 0.0) : 0.0;
    float amount = (1.0 - exp(-dist * 250.0 / max(drop, 250.0) / 1600.0)) * exp(-up * (z > 0.0 ? 3.0 : 3.5));
    // (the morning's sky clear: half the haze over it)
    c = mix(c, hc, clamp(amount * sun_haze * daylight * 0.75 * (z > 0.0 ? 1.0 : 1.0 - 0.5 * day_look), 0.0, 1.0) * (tune_far_dbg == 4.0 ? 0.0 : 1.0));
    // the late afternoon: the air toward the sun's side of the horizon glows, the far mountains and
    // dunes there melt into a bright gold (by the sun's bearing, not its height: it can stand high)
    if (golden > 0.0 && tune_far_dbg != 1.0) {
      vec2 sh = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
      float az = max(dot(normalize(world_dir(ray).xz + 1e-4), sh), 0.0);
      // (low down: the mountains' feet sink into it, their peaks stand out)
      float hrel = z > 0.0 ? world_dir(p).y / 4096.0 : 0.0;
      float g = pow(az, 3.0) * (1.0 - exp(-dist / 900.0)) * exp(-up * 6.0) * exp(-max(hrel, 0.0) / 90.0) *
                golden * daylight;
      c = mix(c, vec3(1.0, 0.86, 0.6), clamp(g * 0.45, 0.0, 1.0));
    }
    // (the dust in a pale sand color: in the haze's own, under an afternoon sun, it laid a band of
    // bright orange over the middle distance)
    float low = (1.0 - exp(-dist * 25.0 / max(drop, 25.0) / 450.0)) * exp(-max(rel + 5.0, 0.0) / 18.0) * 0.6;
    vec3 dc = mix(vec3(luma(hc)) * vec3(1.06, 0.98, 0.86), hc, 0.3);
    c = mix(c, dc, clamp(low * sun_haze * daylight * 0.75 * (1.0 - 0.3 * desert_noon()), 0.0, 1.0) * (tune_far_dbg == 2.0 ? 0.0 : 1.0));
    // a light sand haze far off, always in the air: the far dunes and mountains' feet sink into a
    // pale sand color, the sun shining through it (bright toward it, the mountains' shadows in it);
    // at night only a faint grey-blue, the lamps glowing in it (the halos below)
    {
      float far_k = 1.0 - exp(-max(dist - 60.0, 0.0) * 80.0 / max(drop, 80.0) / 700.0);
      float sand_low = exp(-max(rel + 8.0, 0.0) / 140.0) * (z > 0.0 ? 1.0 : exp(-up * 9.0));
      vec3 sand_c = vec3(0.9, 0.76, 0.56);
      vec3 sun_lit = sun_color() * (0.7 + 0.5 * toward * toward + 0.8 * pow(toward, 10.0)) * mix(0.6, 1.0, lit);
      vec3 sc = sand_c * mix(vec3(0.05, 0.06, 0.08), sun_lit, daylight);
      c = mix(c, sc, clamp(far_k * sand_low * 0.38 * sun_haze, 0.0, 1.0) * (tune_far_dbg == 3.0 ? 0.0 : 1.0));
    }
    // (the four layers stacked on far ground: at 18h from the ground the far range and the horizon
    // band went a flat bright beige, +65% over vanilla where the near sand is +30%. Their lift over
    // what the ground was held to tune_far_lift x, the haze's hue kept: the range keeps its relief)
    if (z > 0.0 && tune_far_lift > 0.0) {
      float lpre = max(luma(c_pre_haze), 0.02);
      float lift = luma(c) / lpre;
      float cap = tune_far_lift + 0.5 * (1.0 - smoothstep(0.02, 0.12, lpre));  // (black rock: more)
      // (not at golden hour: there the glow on the far range is the look, the reference picture's;
      // by golden's onset, not its value: the desert's is 0.5 at 18h)
      c *= mix(1.0, cap / max(lift, cap), daylight * (1.0 - smoothstep(0.0, 0.2, golden)));
    }
  }
  // rain mist: the distance goes grey
  if (weather.x > 0.0 && z > 0.0) {
    vec3 grey = vec3(luma(fog_color)) * vec3(0.82, 0.94, 1.15);  // the fog's brightness, cold, not its tint
    grey *= 1.0 - 0.85 * night_dark.x;
    c = mix(c, grey, (1.0 - exp(-z / 4096.0 * (0.007 + 0.006 * weather2.x))) * (0.22 + 0.12 * weather2.x) * weather.x);
  }
  // rays through the gaps in the clouds, when the sun is low: looking back along the line to it,
  // how much open sky lies between here and it (sky pixels with little cloud)
  if (cloud_rays > 0.0 && clouds.x > 0.0 && cloud_info.x > 0.0 && sky_sun.w > 0.0 && sky_sun.w < 0.45 &&
      sky_sun.z > 0.1 && wsurf == 0.0 && weather.x < 0.5) {
    float toward = pow(max(dot(ray, sky_sun.xyz), 0.0), 3.0);
    float cover = smoothstep(0.1, 0.35, clouds.y) * (1.0 - smoothstep(0.75, 1.0, clouds.y));
    if (toward * cover > 0.01) {
      vec2 suv = vec2(sky_sun.x * proj.z, sky_sun.y * proj.w) / sky_sun.z * 0.5 + 0.5;
      vec2 to_sun = suv - tex_coord;
      ivec2 csize = textureSize(tex_T15, 0);
      float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
      float lit = 0.0;
      for (int i = 0; i < 10; i++) {
        vec2 q = tex_coord + to_sun * (float(i) + jit) / 10.0;
        if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 || texture(tex_T3, q).r > 1e-7) {
          continue;
        }
        vec4 cl = texelFetch(tex_T15, ivec2(q * vec2(csize)), 0);
        if (cl.a >= 0.0 && !isnan(cl.a)) {
          lit += cl.a * cl.a;
        }
      }
      c += sun_color() * smoothstep(0.15, 0.9, lit / 10.0) * toward * cover * cloud_rays * 0.45 *
           (1.0 - smoothstep(0.25, 0.45, sky_sun.w));
    }
  }
  // low fog on the ground: mornings, and steam off the wet ground when the sun comes back out
  float low_fog = weather2.y + 0.1 * weather2.z * (1.0 - clouds.w) + 0.12 * dawn_fog * (1.0 - clouds.w);
  if (low_fog > 0.0) {
    float dist = z > 0.0 ? length(p) / 4096.0 : 2500.0;
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w);
    vec3 fc = mix(fog_color, vec3(luma(fog_color)), 0.3) * 0.85 + vec3(0.06, 0.07, 0.08);
    // brighter looking toward the sun
    fc += vec3(1.0, 0.9, 0.75) * pow(max(dot(ray, sky_sun.xyz), 0.0), 6.0) * 0.35 * daylight;
    c = mix(c, fc * (1.0 - 0.85 * night_dark.x), ground_fog(ray, dist, low_fog) * 0.9);
  }
  // dust: the game's sandstorm (its intensity and its wind), as a volume in the world: rolling
  // clouds of sand carried by the wind, streamers low over the ground close by; lit by the sun,
  // brighter looking toward it
  // (how much of the pixel the sandstorm's dust hides: the material relights below are the surface's,
  // not the dust's; on the veil they made the storm's rocks chalk white)
  float dust_veil = 0.0;
  if (weather.y > 0.0) {
    float dust = weather.y;
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w) + 0.15;
    vec3 sand = mix(vec3(0.8, 0.63, 0.42) * (0.3 + 0.8 * daylight), fog_color, 0.25);
    sand *= 1.0 + pow(max(dot(ray, sky_sun.xyz), 0.0), 4.0) * 0.6 * daylight;
    float dist = z > 0.0 ? length(p) / 4096.0 : 3000.0;
    float cover = dust_cover(ray, dist, dust, pixel_angle() / 6.2832, z > 0.0);
    // (on a character close by the near streamers' sand over his darker colors read as the dunes
    // showing through him: Jak at 10 m at desert-start 18h washed out; tune_dust_chr 0: as before)
    if (pix_tag > 0.3 && pix_tag < 0.55) {
      cover *= 1.0 - tune_dust_chr * (1.0 - smoothstep(20.0, 60.0, dist));
    }
    c = mix(c, sand * (1.0 - 0.8 * night_dark.x), cover * 0.94 * (tune_far_dbg == 6.0 ? 0.0 : 1.0));
    dust_veil = tune_haze_layer > 0.5 ? cover * 0.94 : 0.0;
  }
  if (puff_count > 0) {
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w) + 0.12;
    vec3 sand = mix(vec3(0.66, 0.54, 0.4) * (0.2 + 0.65 * daylight), fog_color * 0.9, 0.3);
    vec3 spray = mix(vec3(0.82, 0.87, 0.9) * (0.2 + 0.75 * daylight), fog_color, 0.15);
    float toward = pow(max(dot(ray, sky_sun.xyz), 0.0), 4.0) * 0.35 * daylight;
    float dist = z > 0.0 ? length(p) / 4096.0 : 1e5;
    vec4 rp = ride_puffs(ray, dist, sand * (1.0 + toward), spray * (1.0 + toward),
                         view_to_world * sky_sun.xyz);
    c = c * (1.0 - rp.a) + rp.rgb;
  }

  // volumetric mist: the half-resolution march, upsampled with weights from the depth so the mist
  // behind Jak doesn't bleed over him
  if (volume != 0) {
    vec2 base = (floor(tex_coord / vol_texel - 0.5) + 0.5) * vol_texel;
    vec2 f = (tex_coord - base) / vol_texel;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int j = 0; j < 2; j++) {
      for (int i = 0; i < 2; i++) {
        vec2 tuv = base + vec2(i, j) * vol_texel;
        float tz = view_z(max(texture(tex_T3, tuv).r, texture(tex_T5, tuv).r));  // near sea too
        float bw = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
        float dz = (z > 0.0 && tz > 0.0) ? abs(tz - z) / max(z, 1.0) : (z == tz ? 0.0 : 1.0);
        float w = bw * exp(-dz * 20.0) + 1e-4;
        sum += texture(tex_T8, tuv) * w;
        wsum += w;
      }
    }
    vec4 v = sum / wsum;
    // under rain the mist is grey, whatever the area's fog tint
    v.rgb = mix(v.rgb, vec3(luma(v.rgb)) * vec3(0.82, 0.94, 1.15), weather.x * 0.6);
    c = c * mix(1.0, v.a, volume_fade) +
        v.rgb * volume_fade * (vol_lights_only > 0.5 ? 1.0 : 1.0 - 0.85 * night_dark.x);
  }

  // fireflies (a green-gold glow that swells and dies every few seconds) or seeds and pollen (pale
  // specks catching the light), hidden behind the scene
  for (int i = 0; i < mote_count; i++) {
    vec3 mp = mote[i].xyz;
    if (mote_style.y <= 0.0 || (z > 0.0 && mp.z > z)) {
      continue;
    }
    vec2 muv = vec2(mp.x * proj.z / mp.z, mp.y * proj.w / mp.z) * 0.5 + 0.5;
    vec2 dpx = (tex_coord - muv) / texel;
    if (dot(dpx, dpx) > 1600.0) {
      continue;
    }
    float seed = mote[i].w;
    if (mote_style.x < 0.5) {
      // a blink: on for about a second out of every 2-5
      float period = 2.0 + 3.0 * fract(seed * 0.618);
      float ph = fract(mote_style.z / period + seed * 0.37);
      float on_k = smoothstep(0.0, 0.1, ph) * (1.0 - smoothstep(0.3, 0.55, ph));
      float radius = clamp(0.05 * 4096.0 * abs(proj.z) * 0.5 / (mp.z * texel.x), 1.3, 4.0);
      float d2 = dot(dpx, dpx);
      float glow = exp(-d2 / (radius * radius)) + 0.35 * exp(-d2 / (radius * radius * 25.0));
      c += vec3(0.75, 1.0, 0.3) * glow * on_k * mote_style.y * 2.6;
    } else {
      float radius = clamp(0.02 * 4096.0 * abs(proj.z) * 0.5 / (mp.z * texel.x), 0.9, 2.2);
      float glow = exp(-dot(dpx, dpx) / (radius * radius));
      // (they twinkle as they turn in the light)
      float tw = 0.6 + 0.4 * sin(mote_style.z * (2.0 + fract(seed) * 3.0) + seed);
      vec3 lit = vec3(1.0, 0.96, 0.86) * (0.35 + 0.9 * clamp(sky_sun.w * 2.5, 0.0, 1.0));
      c = mix(c, lit, clamp(glow * tw * mote_style.y * 1.1, 0.0, 1.0));
    }
  }

  // embers rising from the fires: small sparks drifting up and fading, hidden behind the scene
  for (int i = 0; i < fire_count; i++) {
    vec3 fp = fire_pos[i].xyz;
    float fs = fire_pos[i].w;
    if (fp.z < 1.0 * 4096.0 || fire.y <= 0.0) {
      continue;
    }
    // skip fires far from this pixel on screen (embers rise up to ~5 flame sizes)
    vec2 rel = tex_coord - fire_uv(i);
    if (dot(rel, rel) > dot(fire_axis(i), fire_axis(i)) * 36.0) {
      continue;
    }
    for (int j = 0; j < 24; j++) {
      float s = fire_seed[i] * 17.0 + float(j) * 7.31;
      float h1 = fract(sin(s * 12.9898) * 43758.5453);
      float h2 = fract(sin(s * 78.233) * 43758.5453);
      float h3 = fract(sin(s * 37.719) * 43758.5453);
      float h4 = fract(sin(s * 93.989) * 43758.5453);
      // the last six: the pops, a spark the wood spits out fast now and then
      bool pop = j >= 18;
      float life = pop ? 0.5 + h1 * 0.4 : 1.4 + h1 * 1.6;  // seconds
      float phase = fract(fire.z / life + h2);
      // a new ember each cycle starts somewhere else in the flame
      float cycle = floor(fire.z / life + h2);
      float k1 = fract(sin((s + cycle) * 41.17) * 43758.5453) - 0.5;
      float k2 = fract(sin((s + cycle) * 19.31) * 43758.5453) - 0.5;
      // (a pop only some cycles, flying out sideways too and slowing as it climbs)
      if (pop && fract(sin((s + cycle) * 7.77) * 43758.5453) > 0.35) {
        continue;
      }
      float rise = pop ? (1.0 - (1.0 - phase) * (1.0 - phase)) * fs * (4.0 + 3.0 * h3)
                       : phase * fs * (2.0 + 3.0 * h3);
      float sway = sin(fire.z * (1.5 + h4 * 2.0) + h1 * 6.283) * fs * 0.3 * phase;
      float out_k = pop ? 1.0 + 3.0 * phase : 1.0;
      vec3 ep = fp + up_view * (rise + fs * 0.2) + right_view * (k1 * fs * 0.9 * out_k + sway) +
                fwd_view * (k2 * fs * 0.9 * out_k);
      if (ep.z < 0.3 * 4096.0 || (z > 0.0 && ep.z > z)) {
        continue;
      }
      vec2 euv = vec2(ep.x * proj.z / ep.z, ep.y * proj.w / ep.z) * 0.5 + 0.5;
      vec2 dpx = (tex_coord - euv) / texel;
      float radius = clamp(0.07 * 4096.0 * abs(proj.z) * 0.5 / (ep.z * texel.x), 1.0, 3.5);
      float glow = exp(-dot(dpx, dpx) / (radius * radius));
      if (glow < 0.01) {
        continue;
      }
      float fade = (1.0 - phase) * smoothstep(0.0, 0.08, phase);
      float flick = 0.75 + 0.25 * sin(fire.z * (20.0 + h3 * 15.0) + h4 * 50.0);
      vec3 hot = mix(vec3(1.0, 0.32, 0.04), vec3(1.0, 0.7, 0.25), h4);
      if (pop) {
        hot = mix(vec3(1.0, 0.85, 0.5), hot, phase);  // white-hot as it leaves the flame
      }
      c += hot * (glow * fade * flick * fire.y * (pop ? 3.2 : 2.4));
    }
  }

  // an explosion's dust ring and fireball
  if (fx_detail > 0.0 && blast_count > 0) {
    float daylight = clamp(sky_sun.w * 2.5, 0.0, 1.0) * (1.0 - clouds.w) + 0.12;
    vec3 dust = mix(vec3(0.62, 0.52, 0.4) * (0.25 + 0.65 * daylight), fog_color * 0.9, 0.25);
    float scene_dist = z > 0.0 ? length(p) : 1e12;
    for (int i = 0; i < blast_count; i++) {
      if (blast_dir[i].w < 0.5) {
        vec4 l = blast_layers(i, ray, scene_dist, dust);
        c = c * (1.0 - l.a) + l.rgb;
        if (blast_sparks > 0.0) {
          blast_chunks(i, z, c);
        }
      }
    }
  }

  // sparks thrown by the explosions: each flies out on its own arc (gravity, a little drag), a
  // short streak along its motion, white-hot to orange as it cools, hidden behind the scene
  for (int i = 0; i < blast_count; i++) {
    float age = blast_age[i];
    if (blast_sparks <= 0.0 || age > 1.5) {
      continue;
    }
    vec3 bp = blast_pos[i].xyz;
    if (bp.z < 1.0 * 4096.0) {
      continue;
    }
    // skip blasts far from this pixel on screen (sparks fly up to ~20 m)
    vec2 cuv = vec2(bp.x * proj.z / bp.z, bp.y * proj.w / bp.z) * 0.5 + 0.5;
    vec2 reach = abs(vec2(proj.z, proj.w)) * 0.5 * (22.0 * 4096.0) / max(bp.z - 20.0 * 4096.0, 2.0 * 4096.0);
    vec2 off = abs(tex_coord - cuv);
    if (off.x > reach.x || off.y > reach.y) {
      continue;
    }
    bool gun = blast_dir[i].w > 0.5;
    bool charging = abs(blast_dir[i].w - 2.0) < 0.5;
    bool charged = blast_dir[i].w > 2.5;
    for (int j = 0; j < (charging ? 10 : 40); j++) {
      float s = blast_seed[i] * 13.7 + float(j) * 3.17;
      float h1 = fract(sin(s * 12.9898) * 43758.5453);
      float h2 = fract(sin(s * 78.233) * 43758.5453);
      float h3 = fract(sin(s * 37.719) * 43758.5453);
      float h4 = fract(sin(s * 93.989) * 43758.5453);
      float life = 0.5 + h4 * 0.9;
      // a gun's blast: the first ones crackle around the muzzle (slow, short), the rest fly
      // forward in a wide fan
      bool crackle = gun && j < 14;
      if (gun) {
        life = crackle ? 0.12 + h4 * 0.25 : 0.25 + h4 * 0.4;
      }
      if (charging) {
        life = 0.06 + h4 * 0.14;
      }
      if (age > life) {
        continue;
      }
      // out and mostly up, in world axes
      vec3 dw = normalize(vec3(h1 - 0.5, 0.25 + h2 * 0.75, h3 - 0.5));
      vec3 dir = right_view * dw.x + up_view * dw.y + fwd_view * dw.z;
      float speed = (7.0 + 13.0 * fract(h1 * 7.3)) * 4096.0;
      if (charging) {
        // all round the gun, short and quick (the radius grows with the charge)
        dir = normalize(vec3(h1 - 0.5, h2 - 0.5, h3 - 0.5) + 1e-4);
        speed = (2.0 + 5.0 * h3) * blast_pos[i].w;
      } else if (crackle) {
        dir = normalize(vec3(h1 - 0.5, h2 - 0.4, h3 - 0.5) + blast_dir[i].xyz * 0.6);
        speed = (2.0 + 4.0 * h3) * 4096.0;
      } else if (gun) {
        vec3 side = normalize(cross(blast_dir[i].xyz, up_view) + 1e-6);
        dir = normalize(blast_dir[i].xyz + side * (h1 - 0.5) * (charged ? 4.5 : 1.8) +
                        up_view * (h2 - 0.3) * 0.5);
        speed = (14.0 + 18.0 * h3) * 4096.0;
      }
      float drag = (1.0 - exp(-age * 2.0)) / 2.0;  // distance flown with drag, per unit speed
      vec3 sp = bp + dir * speed * drag - up_view * (0.5 * 9.8 * 4096.0 * age * age * 0.7);
      vec3 vel = dir * speed * exp(-age * 2.0) - up_view * (9.8 * 4096.0 * age * 0.7);
      // (an explosion's bounce off the ground, gone at the second landing)
      if (!gun && fx_detail > 0.0 && blast_ground[i] <= 0.0) {
        bool rest;
        sp = blast_arc(bp, dir, speed, age, blast_ground[i], vel, rest);
        if (rest) {
          continue;
        }
      }
      vec3 tail = sp - vel * 0.03;
      if (sp.z < 0.5 * 4096.0 || tail.z < 0.5 * 4096.0 || (z > 0.0 && sp.z > z)) {
        continue;
      }
      vec2 a = vec2(sp.x * proj.z / sp.z, sp.y * proj.w / sp.z) * 0.5 + 0.5;
      vec2 b = vec2(tail.x * proj.z / tail.z, tail.y * proj.w / tail.z) * 0.5 + 0.5;
      vec2 pa = (tex_coord - a) / texel, ba = (b - a) / texel;
      float hseg = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-4), 0.0, 1.0);
      vec2 dd = pa - ba * hseg;
      float w = clamp(0.04 * 4096.0 * abs(proj.z) * 0.5 / (sp.z * texel.x), 0.7, 2.5);
      float glow = exp(-dot(dd, dd) / (w * w)) * (1.0 - hseg * 0.6);
      if (glow < 0.01) {
        continue;
      }
      float cool = age / life;
      vec3 hot = gun ? mix(vec3(1.0, 0.85, 0.55), vec3(1.0, 0.18, 0.04), cool)
                     : mix(vec3(1.0, 0.92, 0.65), vec3(1.0, 0.35, 0.05), cool);
      if (charged && !crackle) {
        // red in the middle of the spread, fuchsia toward its edges
        hot = mix(hot, mix(vec3(1.0, 0.5, 0.85), vec3(1.0, 0.1, 0.6), cool), smoothstep(0.1, 0.45, abs(h1 - 0.5)));
      }
      c += hot * glow * (1.0 - cool) * 2.2 * blast_sparks;
    }
  }

  // headlight beams in the air: a few steps along the view through their cones
  if (spot_count > 0) {
    float air_b = weather.x * 0.8 + weather2.y + weather.y * 0.6 + weather2.x * 0.3 + clouds.w * 0.3 +
                  0.35 * night_dark.x;
    float tmax = min(z > 0.0 ? length(p) : 1e9, 70.0 * 4096.0);
    float beam = 0.0;
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    for (int i = 0; i < spot_count; i++) {
      for (int k = 0; k < 10; k++) {
        float t = tmax * (float(k) + jit) / 10.0;
        vec3 q = ray * t - spot_pos[i].xyz;
        float dq = length(q);
        float cone = smoothstep(0.86, 0.97, dot(q / max(dq, 1.0), spot_dir[i].xyz));
        float dm = dq / 4096.0;
        beam += cone * spot_pos[i].w / (1.0 + dm * dm / 80.0) * (1.0 - smoothstep(35.0, 70.0, dm)) *
                smoothstep(2.0, 12.0, dm) * tmax / 10.0 / 4096.0;
      }
    }
    // (faint near the lamp: the beam shows down the road, not as a ball of light on the car)
    c += vec3(1.0, 0.9, 0.75) * beam * min(air_b, 1.2) * 0.012;
    // dust hanging in the beams: always a little, a lot in the desert's dust, some in the rain
    if (spot_pos[0].w > 0.0) {
      float motes = beam_motes(ray, z, pixel_rad);
      c += vec3(1.0, 0.93, 0.8) * min(motes, 1.5) * (0.5 + 2.0 * weather.y + 0.4 * weather.x) * 2.6 *
           min(spot_pos[0].w, 1.0);
    }
  }

  // halos: in rain, fog or dust the air around each light glows (scattering), less where the
  // light is behind something
  // (at night the air always holds some haze: the lamps glow in it, and light the air along the
  // view between them and the scene)
  // (and the desert's sand haze: a torch or a town's lamps glow in it)
  float air = weather.x * 0.8 + weather2.y + weather.y * 0.5 + weather2.x * 0.3 + clouds.w * 0.35 +
              sun_haze * 0.45;
  if (air > 0.02 && light_count > 0) {
    vec3 halo = vec3(0.0);
    vec3 shine = vec3(0.0);
    float scene_dist = z > 0.0 ? length(p) : 1e12;
    for (int i = 0; i < min(light_count, 32); i++) {
      // the light scattered toward the camera along the ray up to the scene: the integral of
      // 1 / (h^2 + (t - tc)^2), h the light's distance off the ray
      vec3 lp = light_pos[i].xyz;
      float lr = light_pos[i].w;
      float tc = dot(lp, ray);
      float h = sqrt(max(dot(lp, lp) - tc * tc, 1.0));
      // (lamps, not the big area lights: those would fill the whole view)
      if (h > lr * 1.5 || lr > 20.0 * 4096.0) {
        continue;
      }
      float t1 = min(scene_dist, tc + lr * 3.0);
      float t0 = 0.0;
      if (t1 <= t0) {
        continue;
      }
      float hs = max(h, lr * 0.08);
      // (capped: a bright light passing right by the ray, a dark eco blast in the rain, would
      // otherwise flood the view in its color)
      float inte = min((atan((t1 - tc) / hs) - atan((t0 - tc) / hs)) * lr / hs, 6.0);
      // (not the lights by the camera: a vehicle's own engines and lamps fogged the whole view in
      // front of it, and their flicker, the engines' glows changing size every frame, flashed it)
      shine += light_color[i].rgb * inte * (1.0 - smoothstep(lr * 0.5, lr * 1.5, h)) *
               smoothstep(10.0 * 4096.0, 25.0 * 4096.0, length(lp));
    }
    shine *= min(air, 1.0) * 0.008;
    // all of them together at most a soft glow (scaled down, its hue kept)
    float shine_max = max(shine.r, max(shine.g, shine.b));
    c += shine * min(1.0, 0.15 / max(shine_max, 1e-4));
    for (int i = 0; i < min(light_count, 16); i++) {
      vec3 lp = light_pos[i].xyz;
      float ld = length(lp);
      if (lp.z <= 0.0 || ld < 1.0) {
        continue;
      }
      float cosang = dot(ray, lp / ld);
      float spread = clamp(light_pos[i].w * 0.4 / ld, 0.01, 0.1);
      // (not the lights right by the camera: their glow would fill the screen)
      float g = exp(-(1.0 - cosang) / (spread * spread * 0.5)) *
                smoothstep(4.0 * 4096.0, 10.0 * 4096.0, ld);
      if (g < 0.003) {
        continue;
      }
      // dimmer where the scene is in front of the light, fading in over its radius (no hard edge)
      float hidden = mix(0.05, 1.0, smoothstep(ld - light_pos[i].w * 1.5, ld, scene_dist));
      halo += light_color[i].rgb * g * hidden;
    }
    c += halo * min(air, 1.5) * 0.25 * (0.25 + 0.75 * clouds.w);
  }

  // the drops Jak's steps throw up from the puddles, lit like the rain
  if (splash_count > 0) {
    float drops = step_drops(tex_coord, z);
    if (drops > 0.0) {
      vec3 glow = texture(tex_T1, tex_coord).rgb;
      vec3 lit = c * 1.15 + vec3(0.08, 0.09, 0.1) + mix(fog_color, vec3(luma(fog_color)), 0.5) * 0.25 +
                 glow * 1.2;
      c = mix(c, lit, drops * 0.45);
    }
  }

  // rain streaks in front of everything, lit by the sky and by the lights around (the bloom)
  if (weather.x > 0.0) {
    float streaks = rain_drops(ray, z, pixel_rad) * (1.0 + 0.9 * weather2.x);
    if (streaks > 0.0) {
      vec3 glow = texture(tex_T1, tex_coord).rgb;
      vec3 lit = c * 1.25 + vec3(0.07, 0.08, 0.1) + mix(fog_color, vec3(luma(fog_color)), 0.5) * 0.25 +
                 glow * 1.6;
      float show = 1.0 - 0.65 * smoothstep(0.45, 0.9, luma(c));
      c = mix(c, lit, clamp(streaks * 0.32 * weather.x * show, 0.0, 0.45));
    }
  }

  // snowflakes in front of everything, white, a little dimmer in dim light
  if (snow_heat.x > 0.0) {
    float flakes = snow_flakes(ray, z, pixel_rad);
    if (flakes > 0.0) {
      vec3 white = vec3(0.92, 0.95, 1.0) * (0.55 + 0.45 * smoothstep(0.05, 0.5, luma(fog_color))) +
                   texture(tex_T1, tex_coord).rgb;
      c = mix(c, white, clamp(flakes * snow_heat.x, 0.0, 0.9));
    }
  }

  // bloom and god rays
  c += texture(tex_T1, tex_coord).rgb * bloom_strength;
  // (god rays a quarter softer than the setting reads: they washed the low sun's side out)
  // (softened over a few of its own texels: it is drawn small and shimmered when stretched)
  {
    vec2 st = 1.5 / vec2(textureSize(tex_T4, 0));
    vec3 shafts = texture(tex_T4, tex_coord).rgb * 0.4 +
                  (texture(tex_T4, tex_coord + vec2(st.x, st.y)).rgb +
                   texture(tex_T4, tex_coord + vec2(-st.x, st.y)).rgb +
                   texture(tex_T4, tex_coord + vec2(st.x, -st.y)).rgb +
                   texture(tex_T4, tex_coord + vec2(-st.x, -st.y)).rgb) * 0.15;
    c += shafts * shafts_strength * 0.75;
  }

  // a town's glow over the horizon at night: warm, low, spread over the town's width, on the sky
  // and the far haze
  if (town_glow.z > 0.0) {
    vec3 wd = normalize(view_to_world * ray);
    vec2 to = town_glow.xy - cam_pos.xz;
    float dist = length(to);
    vec2 hd = normalize(wd.xz + vec2(1e-5));
    float ang = acos(clamp(dot(hd, to / dist), -1.0, 1.0));
    // the town's half width seen from here, at least a few degrees
    float half_w = max(atan(300.0 * 4096.0 / dist), 0.06);
    float across = exp(-pow(ang / (half_w * 1.6), 2.0));
    float elev = wd.y;
    float above = exp(-max(elev + 0.01, 0.0) / 0.16) * smoothstep(-0.06, 0.0, elev);
    float far_w = d <= 1e-7 ? 1.0 : smoothstep(400.0 * 4096.0, 1500.0 * 4096.0, z) * 0.6;
    c += vec3(1.0, 0.55, 0.25) * 0.5 * across * above * far_w * town_glow.z;
  }

  // far lights at night: each far glow a point of light with a soft halo that keeps a few pixels
  // however far it is (the game's own glow shrinks to nothing), hidden behind what's in front
  float aspect_px = texel.y / texel.x;
  float night_air = weather.x * 0.8 + weather2.y + weather.y * 0.5 + weather2.x * 0.3 + sun_haze * 0.4;
  for (int i = 0; i < beacon_count; i++) {
    vec3 bp = beacon_pos[i].xyz;
    vec2 buv = 0.5 * vec2(proj.z * bp.x, proj.w * bp.y) / bp.z + 0.5;
    vec2 dpx = (tex_coord - buv) / texel.y;
    dpx.x *= aspect_px;
    float r2 = dot(dpx, dpx);
    if (r2 < 40.0 * 40.0 && all(greaterThan(buv, vec2(0.0))) && all(lessThan(buv, vec2(1.0)))) {
      float bd = texture(tex_T3, buv).r;
      float front = bd <= 1e-7 ? 1e12 : view_z(bd);
      if (front > bp.z * 0.97 - 2.0 * 4096.0) {
        float dist_m = length(bp) / 4096.0;
        // the glow's own size on screen, at least a couple of pixels
        float core_px = max(1.6, beacon_pos[i].w * 0.35 * abs(proj.w) * 0.5 / bp.z / texel.y);
        float core = exp(-r2 / (core_px * core_px));
        float halo = exp(-sqrt(r2) / (5.0 + 6.0 * min(night_air, 1.0))) * 0.18;
        float fade = exp(-dist_m / 1800.0);
        c += beacon_color[i].rgb * (core * 1.4 + halo) * beacon_color[i].w * fade;
      }
    }
  }
  // Jak's lantern bulb: a warm point hovering by him in a thin metal rim, its glow in the night air
  // around it (the air glows even where something hides the bulb)
  if (lantern.w > 0.0 && lantern.z > 0.5 * 4096.0) {
    vec3 lp = lantern.xyz;
    vec2 luv = 0.5 * vec2(proj.z * lp.x, proj.w * lp.y) / lp.z + 0.5;
    vec2 dpx = (tex_coord - luv) / texel.y;
    dpx.x *= aspect_px;
    float r = length(dpx);
    float core_px = max(2.0, 0.08 * 4096.0 * abs(proj.w) * 0.5 / lp.z / texel.y);
    float ld = texture(tex_T3, clamp(luv, vec2(0.0), vec2(1.0))).r;
    float vis = (ld <= 1e-7 || view_z(ld) > lp.z - 0.3 * 4096.0) ? 1.0 : 0.0;
    float bulb = exp(-r * r / (core_px * core_px)) * 2.2 * vis;
    // its casing: a thin dark band with a bright edge where the bulb's light catches the metal
    float band = exp(-sq((r - core_px * 1.7) / (core_px * 0.35))) * vis;
    float edge = exp(-sq((r - core_px * 2.05) / (core_px * 0.18))) * vis *
                 (0.6 + 0.4 * dpx.y / max(r, 1e-3));
    // (the wide part held low: at 0.2 it and the bloom on top made a big orange orb beside Jak, a
    // stranger to the dark desert night, G3)
    float halo = exp(-r / (core_px * 4.0)) * 0.5 + exp(-r / (core_px * 16.0)) * tune_lantern_halo;
    c *= 1.0 - 0.55 * band * lantern.w;
    // white-yellow in its heart, the glow deep orange
    c += (vec3(1.0, 0.82, 0.5) * bulb + vec3(1.0, 0.7, 0.4) * edge * 0.5 +
          vec3(1.0, 0.5, 0.18) * halo * (0.4 + 0.6 * vis)) * lantern.w;
  }

  // the headlamps themselves seen from in front: a blinding point and a short streak
  for (int i = 0; i < spot_count; i++) {
    vec3 sp = spot_pos[i].xyz;
    if (sp.z > 1.0 * 4096.0) {
      float into = dot(normalize(-sp), spot_dir[i].xyz);
      float glare = smoothstep(0.55, 0.95, into) * spot_pos[i].w;
      if (glare > 0.0) {
        vec2 suv = 0.5 * vec2(proj.z * sp.x, proj.w * sp.y) / sp.z + 0.5;
        vec2 dpx = (tex_coord - suv) / texel.y;
        dpx.x *= aspect_px;
        float bd = texture(tex_T3, suv).r;
        float front = bd <= 1e-7 ? 1e12 : view_z(bd);
        if (front > sp.z * 0.95 - 1.0 * 4096.0) {
          float r = length(dpx);
          float core_px = max(2.0, 0.12 * 4096.0 * abs(proj.w) * 0.5 / sp.z / texel.y);
          float g = exp(-r * r / (core_px * core_px)) * 1.2 + exp(-r / 10.0) * 0.15 +
                    exp(-abs(dpx.y) * 0.8) * exp(-abs(dpx.x) / 60.0) * 0.12;
          c += vec3(1.0, 0.94, 0.82) * g * glare;
        }
      }
    }
  }

  // lens flare: looking toward the sun, the lens's glass throws ghosts of it along the line through
  // the middle of the picture, a faint halo ring and a short streak. Only as much as the sun itself
  // shows (the sky around its disk, not hidden behind a wall or the clouds).
  if (flare_vis > 0.0) {
    vec2 suv = 0.5 * vec2(proj.z * sky_sun.x, proj.w * sky_sun.y) / sky_sun.z + 0.5;
    {
      float vis = flare_vis;
      {
        float aspect = texel.y / texel.x;
        vec2 axis = vec2(0.5) - suv;
        vec3 fl = vec3(0.0);
        // ghosts: discs of a few sizes and tints, some behind the middle, one past it
        fl += flare_ghost(suv + axis * 0.45, aspect, 0.025) * vec3(1.0, 0.75, 0.4);
        fl += flare_ghost(suv + axis * 0.8, aspect, 0.05) * vec3(0.4, 0.8, 0.6);
        fl += flare_ghost(suv + axis * 1.25, aspect, 0.018) * vec3(0.9, 0.5, 0.9);
        fl += flare_ghost(suv + axis * 1.55, aspect, 0.08) * vec3(0.35, 0.5, 1.0);
        fl += flare_ghost(suv + axis * 2.1, aspect, 0.035) * vec3(1.0, 0.85, 0.5);
        // the halo ring around the middle of the picture, toward the sun
        vec2 dv = (tex_coord - vec2(0.5)) * vec2(aspect, 1.0);
        float ring = exp(-sq((length(dv) - 0.42) / 0.025)) *
                     max(dot(normalize(dv + 1e-5), normalize(-axis * vec2(aspect, 1.0) + 1e-5)), 0.0);
        fl += vec3(0.9, 0.75, 1.0) * ring * 0.025;
        // a short horizontal streak through the sun
        vec2 ds = (tex_coord - suv) * vec2(aspect, 1.0);
        fl += vec3(1.0, 0.9, 0.75) * exp(-abs(ds.y) * 400.0) * exp(-abs(ds.x) * 6.0) * 0.18;
        c += fl * vis * lens_flare * clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w * 2.0) * (1.0 - 0.7 * weather.x);
      }
    }
  }

  // the sky comes here well past 1, and the roll-off below flattens every channel past 0.85 to one
  // pale grey: brought under it as a whole first, half its color kept, so its gradients and the
  // sun's soft glow survive
  {
    float sky_part = z == 0.0 ? 1.0 : smoothstep(2800.0 * 4096.0, 4000.0 * 4096.0, z);
    if (sky_part > 0.0 && wsurf == 0.0) {
      vec3 under = c * min(1.0, 0.9 / max(max(c.r, max(c.g, c.b)), 1e-3));
      under = mix(under, vec3(luma(under)) * vec3(1.0, 0.99, 0.97), 0.5);
      c = mix(c, under, sky_part);
    }
  }
  // highlights roll off instead of clipping (bloom can push past 1): first the brightness, from
  // 0.7 up (the hue kept: sunlit sand and walls keep their color instead of burning to yellow and
  // white), then each channel
  {
    // a strongly colored light past full (a red lamp on a wall) bleaches toward white as film and
    // eyes see it, instead of flattening into one pure, detail-less color
    float mx = max(c.r, max(c.g, c.b));
    c = mix(c, vec3(mx), clamp((mx - 1.0) * 0.35, 0.0, 0.45) * (1.0 - flame_keep));
    float hl = luma(c);
    if (hl > 0.7 && flame_keep < 0.99) {
      float over = hl - 0.7;
      c *= (0.7 + over / (1.0 + over * 2.2)) / hl;
    }
  }
  c = c / (1.0 + max(c - 0.85, 0.0) * 1.2);
  // by day, a white balance: the scene's average color (a few points of it) brought part of the
  // way to grey, so a strong cast (the desert's yellow afternoons) doesn't tint everything
  if (clouds.w < 0.99) {
    c *= mix(vec3(1.0), white_balance, 0.35 * (1.0 - clouds.w));
  }

  // lawns (plain level ground, facing up, green in the game's colors: the forest's floor): one flat
  // evenly lit carpet. A slow light and dark and an olive to deep-green drift over tens of meters,
  // and moss-dark dirt where the occlusion is (the feet of rocks, trunks and grass tufts)
  // (test: tune_lawn >= 10 shows the lawn's weight)
  // (the forest's floor reads in the rock band, 0.75: kept to green pixels, mossy rock gets it too)
  if (tune_lawn > 0.0 && z > 0.0 && wsurf == 0.0 && pix_normal.a > 0.83 &&
      (pix_tag > 0.955 || (pix_tag > 0.7 && pix_tag < 0.8)) && dot(surf_n, up_view) > 0.7) {
    vec3 raw = texture(tex_T0, tex_coord).rgb;
    // (by the green over the red: at dusk the game's own light turns the forest's lawn teal)
    float green = smoothstep(0.015, 0.05, raw.g - raw.r) * (1.0 - smoothstep(0.04, 0.12, raw.b - raw.g)) * min(tune_lawn, 1.0);
    if (green > 0.0) {
      vec3 wp = cam_pos / 4096.0 + world_dir(p) / 4096.0;  // m
      float v = gnoise(wp.xz / 23.0 + 4.7) * 0.65 + gnoise(wp.xz / 7.0 + 1.9) * 0.35;
      float h = gnoise(wp.xz / 41.0 + 9.3);
      vec3 tint = mix(vec3(0.92, 1.0, 1.06), vec3(1.1, 1.02, 0.82), smoothstep(-0.5, 0.5, h));
      vec3 lc = c * (1.0 + tune_lawn_var * v) * tint;
      lc = mix(lc, lc * vec3(0.72, 0.66, 0.5), clamp((1.0 - ao) * tune_lawn_ao, 0.0, 0.6));
      c = tune_lawn > 9.0 ? vec3(green * 0.1) : mix(c, lc, green);
    }
  }

  // rock and cliffs (normals written 0.75 long: tfrag3.frag): the game's textures are a blur up
  // close and stretched over whole mountains, so a tiling rock picture (rock_detail.png) is laid
  // over them in the world from three sides (triplanar), three sizes, its brightness only (the
  // game's colors kept). Each read at the mip level of the pixel's size (no derivatives in here):
  // too small for the pixels, a size averages out by itself
  if (rock_detail > 0.0 && z > 0.0) {
    float nl = pix_tag;
    if (nl > 0.55 && nl < 0.8) {
      vec3 wp = cam_pos / 4096.0 + world_dir(p) / 4096.0;  // m
      vec3 wn = world_dir(surf_n);
      // (sharp: the view from above, its strata lines, stays off steep faces, where they ran down
      // the rock)
      // (a plane that barely shows isn't read at all: most pixels need only one of the three)
      vec3 tw = pow(abs(wn), vec3(8.0));
      tw = max(tw / (tw.x + tw.y + tw.z) - 0.03, vec3(0.0));
      tw /= tw.x + tw.y + tw.z;
      float foot = length(p) / 4096.0 * pixel_rad / max(abs(dot(ray, surf_n)), 0.1);  // m per pixel
      float d = 0.0;
      float dist = length(p) / 4096.0;  // m
      // (as big games do it: the picture is close-up detail, each size only within its own reach
      // (the small one near, the big one out to ~150 m); farther the rock keeps only a soft, broad
      // light and dark (macro variation), so big faces far away don't show a repeated pattern that
      // fights their own texture)
      // (and in the late afternoon a third, mountain-sized: the low sun rakes the far faces, their
      // cracks and blocks show out to the mountains)
      // (the picture: cracked blocky rock; its blocks shift a little from place to place, and it
      // comes and goes: strong in some places, fainter in others)
      float wander = gnoise(wp.xz / 9.0 + vec2(0.0, wp.y / 25.0) + 1.3) * 0.8;
      float bands = 0.5 + 0.5 * smoothstep(-0.35, 0.35, gnoise(vec2(wp.x + wp.z, wp.y) / 30.0 + 5.3));
      for (int k = 0; k < 3; k++) {
        vec4 size_k = k == 0 ? vec4(1.6, 0.5, 15.0, 45.0) : k == 1 ? vec4(7.0, 0.85, 60.0, 160.0) : vec4(60.0, 0.8, 900.0, 1600.0);  // (tile m, strength, fade from, to)
        float lod = max(log2(foot * 1024.0 / size_k.x), 0.0);
        float near = 1.0 - smoothstep(size_k.z, size_k.w, dist);
        if (k == 2) {
          near *= golden * smoothstep(60.0, 160.0, dist);
        }
        if (lod > 9.0 || near <= 0.0) {
          continue;  // (one texel over the pixel, or past its reach: nothing to add)
        }
        vec3 q = wp / size_k.x + float(k) * 0.37;
        vec3 qs = q + vec3(wander, 0.0, wander) / size_k.x;
        float t = 0.0;
        if (tw.x > 0.0) {
          t += luma(notile(tex_T10, qs.zy, lod).rgb) * tw.x;
        }
        if (tw.y > 0.0) {
          t += luma(notile(tex_T10, q.xz, lod).rgb) * tw.y;
        }
        if (tw.z > 0.0) {
          t += luma(notile(tex_T10, qs.xy, lod).rgb) * tw.z;
        }
        d += size_k.y * near * (t - rock_detail_avg);
      }
      // (the broad variation: a few tens of meters, gentle, it fades in as the detail fades out)
      float macro = gnoise(vec2(wp.x + wp.y * 0.6, wp.z - wp.y * 0.4) / 45.0 + 2.1) * 0.6 +
                    gnoise(vec2(wp.x - wp.y * 0.5, wp.z + wp.y * 0.7) / 13.0 + 7.4) * 0.4;
      d += macro * 0.12 * (0.4 + 0.6 * smoothstep(30.0, 150.0, dist));
      c *= max(1.0 + rock_detail * 1.6 * d * bands, 0.3);
      // (the rocky ground (normals 0.6 long): the game paints it as pale as the sand around)
      // (and its grain stronger: gravel, mottled light and dark, warm)
      if (nl < 0.65) {
        vec3 c_far = c;
        // (not the blocky picture: over the flat ground its cracks tile into a grid; weathered stone
        // and gravel instead, soft mottling at a few sizes, the finest only near)
        float m = gnoise(wp.xz / 0.9 + 3.1) * 0.35 + gnoise(wp.xz / 3.0 + 8.2) * 0.4 +
                  gnoise(wp.xz / 11.0 + 1.7) * 0.25;
        float close = 1.0 - smoothstep(8.0, 35.0, dist);
        // (the grit: the rock picture small and turned, so its blocks don't line up into a grid)
        vec2 gq = mat2(0.87, 0.5, -0.5, 0.87) * wp.xz / 0.7;
        float glod = max(log2(foot * 512.0 / 0.7), 0.0);
        float grit = glod < 8.0 ? (luma(notile(tex_T10, gq, glod).rgb) - rock_detail_avg) : 0.0;
        // (and crusted plates a few meters across: the same picture bigger, turned the other way)
        vec2 pq = mat2(0.6, -0.8, 0.8, 0.6) * wp.xz / 3.1 + 0.29;
        float plod = max(log2(foot * 512.0 / 3.1), 0.0);
        float plates = plod < 8.0 ? (luma(notile(tex_T10, pq, plod).rgb) - rock_detail_avg) : 0.0;
        // (its dark crack lines kept faint, its light crust strong; and lumps, light and dark)
        grit = grit > 0.0 ? grit * 1.4 : grit * 0.6;
        plates = plates > 0.0 ? plates * 1.0 : plates * 0.4;
        float lumps = smoothstep(-0.15, 0.3, gnoise(wp.xz / 0.45 + 1.9)) - 0.5;
        float fine = (gnoise(wp.xz / 0.22 + 5.5) * 0.6 + grit + plates + lumps * 0.55) * close;
        // (with the ground picture (ground_detail.png: stone, gravel, pebbles) that instead: two sizes,
        // turned against each other, and its colors too, the grey-blue pebbles among the brown)
        vec3 gtint = vec3(1.0);
        if (ground_detail > 0.0) {
          close = 1.0 - smoothstep(15.0, 50.0, dist);
          gq = mat2(0.87, 0.5, -0.5, 0.87) * wp.xz / 2.2;
          glod = max(log2(foot * 2048.0 / 2.2), 0.0);
          pq = mat2(0.6, -0.8, 0.8, 0.6) * wp.xz / 8.5 + 0.29;
          plod = max(log2(foot * 2048.0 / 8.5), 0.0);
          float ga = luma(ground_detail_avg);
          vec3 gc = notile(tex_T16, gq, glod).rgb;
          vec3 pc = notile(tex_T16, pq, plod).rgb;
          float gn = 1.0 - smoothstep(6.0, 9.0, glod);
          fine = ((luma(gc) - ga) * 2.2 * gn + (luma(pc) - ga) * 1.2 + lumps * 0.2) * close;
          vec3 chroma = mix(pc / max(luma(pc), 0.02), gc / max(luma(gc), 0.02), gn) / (ground_detail_avg / ga);
          gtint = mix(vec3(1.0), clamp(chroma, vec3(0.6), vec3(1.5)), 0.8 * close);
        }
        c /= max(1.0 + rock_detail * 1.6 * d * bands, 0.3);
        // (in the late afternoon's low light much darker than the sand; by day nearer its paleness)
        float low_light = min(golden * 2.0, 1.0);
        c *= mix(0.78, 0.5, low_light) * max(1.0 + rock_detail * (2.0 * m + mix(1.1, 2.2, noon) * fine), 0.25);
        // (weathered: grey-brown in the low light, the sky's blue in its shade; by day the warm stone the
        // sand's made of)
        // (not by night: the moon's blue ground went grey-mauve, red +29 over vanilla at 22h)
        c = mix(vec3(luma(c)), c, mix(0.92, 0.9, low_light)) *
            mix(mix(vec3(1.12, 1.0, 0.86), vec3(1.12, 1.0, 0.88), low_light), vec3(1.0),
                max(clouds.w * tune_rock_night, max(morning, sky_morning) * tune_rock_dawn));
        c *= gtint;
        // (lit by the sun: the relief's slope against the light, near only)
        if (close > 0.0 && plod < 6.0) {
          const float e = 0.06;
          float h0 = ground_height(wp.xz, glod, plod);
          vec2 grad = vec2(ground_height(wp.xz + vec2(e, 0.0), glod, plod) - h0,
                           ground_height(wp.xz + vec2(0.0, e), glod, plod) - h0) / e;
          vec3 bn = normalize(wn - vec3(grad.x, 0.0, grad.y) * 1.0);
          vec3 ws = normalize(world_dir(sun_dir.xyz));
          float base = max(dot(wn, ws), 0.05);
          float relief = clamp(max(dot(bn, ws), 0.0) / base, 0.55, 1.6);
          float rk = close * 0.7 * clamp(base * 4.0, 0.0, 1.0);
          // (the sunlit sides warm, the turned-away ones cooler: the sky lights them)
          vec3 tint = relief > 1.0 ? mix(vec3(1.0), vec3(1.1, 0.98, 0.84), min(relief - 1.0, 1.0))
                                   : mix(vec3(1.0), vec3(0.92, 0.96, 1.08), min(1.0 - relief, 1.0) * 2.0);
          c *= mix(vec3(1.0), relief * tint, rk);
        }
        // (and sand blown over it in drifts: lighter, yellower patches with soft edges)
        float drift = smoothstep(0.05, 0.4, gnoise(wp.xz / 4.5 + 6.6) + 0.35 * gnoise(wp.xz / 1.3 + 2.2));
        c = mix(c, c * mix(vec3(1.55, 1.38, 1.12), vec3(1.32, 1.24, 1.12), noon), drift * 0.6 * rock_detail * 2.0);
        // (farther, the game's own: its rocky patches out in the sand blend into it at their edges,
        // darkened they showed as hard-edged slabs)
        // (and where the game blends its rocky ground into the sand, the tag is blended too: there
        // only partly)
        c = mix(c, c_far, max(smoothstep(30.0, 60.0, dist), smoothstep(0.605, 0.645, nl)));
      }
    }
  }

  // the desert's sand: the game paints it olive, and lit at dusk it goes green. Its yellow-greens
  // (and the dull dark greens of its pebbles) turned toward warm gold; the sky and the strong
  // greens of plants kept
  // (the sand ripples' meters across the crests per pixel, from the screen's derivatives, out
  // here where every pixel runs: the crests run nearly toward the camera, and along them the
  // ground isn't squeezed by the grazing view, so a wave stays sharp much farther than the
  // pixel's worst-case size says)
  const vec2 rip_across = vec2(0.82, 0.57);  // the crests run across the wind
  float rip_m = fwidth(dot((cam_pos.xz + world_dir(p).xz) / 4096.0, rip_across));
  if (sand > 0.0 && z > 0.0) {
    // (the land only, by the normals' tag lengths: sand, rocky ground, rock. The palms' leaves,
    // the bushes and the torches' flames are dull greens and pale yellows too: they went beige)
    float land_tag = pix_tag;
    float land = (land_tag > 0.55 && land_tag < 0.65) || (land_tag > 0.7 && land_tag < 0.8) ||
                 (land_tag > 0.85 && land_tag < 0.95) ? 1.0 - flame_keep : 0.0;
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float ch = mx - mn;
    if (ch > 1e-4) {
      // hue (0..1), as hsv
      float hue = mx == c.r ? mod((c.g - c.b) / ch, 6.0) : mx == c.g ? (c.b - c.r) / ch + 2.0 : (c.r - c.g) / ch + 4.0;
      hue /= 6.0;
      float sat = ch / mx;
      // olive and yellow-green fully; the dull greens of the pebbles less (strong greens stay)
      float w = smoothstep(0.07, 0.12, hue) * (1.0 - smoothstep(0.24, 0.30, hue));
      w = max(w, smoothstep(0.2, 0.26, hue) * (1.0 - smoothstep(0.42, 0.5, hue)) * (1.0 - smoothstep(0.35, 0.6, sat)) * 0.8);
      w *= smoothstep(0.03, 0.12, sat) * sand * land;
      if (w > 0.0) {
        // (a pale, dusty beige: less saturated than the game's olive (no lighter: noon burned out); never
        // past a sand's saturation: the yellower patches went bright orange, in blotches and bands)
        // (in the late afternoon: amber, and as rich as the low sun makes it)
        float h2 = mix(hue, mix(0.095, 0.075, golden), w);
        float s2 = mix(sat, mix(min(sat, 0.3), clamp(sat * 1.25, 0.4, 0.5), golden), w);
        vec3 k = clamp(abs(mod(h2 * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
        c = mx * mix(vec3(1.0), k, s2);
      }
      // and its pale slopes: the game lights some near white, a little blue (any hue, barely any
      // color, bright): to a sand beige, a touch darker
      float wp = (1.0 - smoothstep(0.08, 0.2, sat)) * smoothstep(0.45, 0.7, mx) * sand * land * (1.0 - w);
      if (wp > 0.0) {
        vec3 beige = clamp(abs(mod(0.095 * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
        c = mix(c, mx * 0.92 * mix(vec3(1.0), beige, 0.27), wp);
      }
    }
    // on the sand (its normals written 0.9 long: tfrag3.frag, hfrag.frag): the wind's ripples,
    // made up in the world (the game's sand is a blur up close), shaded by the sun across them.
    // By the mark only, not the color: the warm haze turns the far sand orange
    float nl = pix_tag;
    float dune = step(0.85, nl) * step(nl, 0.95) * (1.0 - flame_keep);
    float level = dot(surf_n, up_view);
    float foot = length(p) / 4096.0 * pixel_rad / max(abs(dot(ray, up_view)), 0.05);  // m per pixel
    float rip = dune * smoothstep(0.4, 0.75, level);
    if (rip > 0.0) {
      vec2 xz = cam_pos.xz / 4096.0 + world_dir(p).xz / 4096.0;
      vec2 across = rip_across;
      vec2 sun_xz = world_dir(sky_sun.xyz).xz;
      float facing = dot(across, sun_xz) / max(length(sun_xz), 1e-3);
      // in patches: sharp in some places, smoothed out in others
      float patches = 0.35 + 0.65 * smoothstep(-0.25, 0.35, gnoise(xz * 0.11 + 3.7));
      // the dune's sides: the one the wind climbs is combed into ripples, the sheltered one past the
      // crest (the wind blows along +across) is smoothed by the sand falling over it
      vec3 wn = world_dir(surf_n);
      float lee = smoothstep(0.03, 0.2, dot(wn.xz, across));
      float windward = smoothstep(0.03, 0.2, -dot(wn.xz, across));
      patches *= mix(1.0, 0.3, lee) * mix(1.0, 1.25, windward);
      // the sand's own grain, near: three pictures, by place. Coarse gravelly sand on the steeper
      // parts and in patches (the sand thinner there over the stones), a crust in flat hollows of
      // its own, fine rippled sand the rest. Only their detail: the game's colors stay
      float sclose = 1.0 - smoothstep(25.0, 70.0, length(p) / 4096.0);
      float w_coarse = 0.0, w_crust = 0.0;
      if (sand_set > 0.0 && sclose > 0.0) {
        float n1 = gnoise(xz / 23.0 + 4.2) * 0.7 + gnoise(xz / 7.0 + 1.1) * 0.3;
        float n2 = gnoise(xz / 31.0 + 9.3) * 0.7 + gnoise(xz / 9.0 + 5.5) * 0.3;
        w_coarse = clamp(smoothstep(0.12, 0.3, 1.0 - level) * 0.8 + smoothstep(0.15, 0.45, n1), 0.0, 1.0);
        w_crust = smoothstep(0.2, 0.45, n2) * (1.0 - w_coarse) * smoothstep(0.88, 0.96, level);
        float w_fine = max(1.0 - w_coarse - w_crust, 0.0);
        // (each at two sizes turned against each other for the fine, so no repeat shows; mip levels
        // from the pixel's footprint: 2048 texels a tile)
        vec2 q1 = xz / 3.0, q2 = mat2(0.8, -0.6, 0.6, 0.8) * xz / 1.1 + 0.37;
        vec3 fine_c = notile(tex_T17, q1, max(log2(foot * 2048.0 / 3.0), 0.0)).rgb * 0.6 +
                      notile(tex_T17, q2, max(log2(foot * 2048.0 / 1.1), 0.0)).rgb * 0.4;
        vec3 coarse_c = notile(tex_T18, mat2(0.6, 0.8, -0.8, 0.6) * xz / 2.0, max(log2(foot * 2048.0 / 2.0), 0.0)).rgb;
        vec3 crust_c = notile(tex_T19, xz / 6.0 + 0.61, max(log2(foot * 2048.0 / 6.0), 0.0)).rgb;
        vec3 ratio = w_fine * fine_c / max(sand_set_avg[0], vec3(0.02)) +
                     w_coarse * coarse_c / max(sand_set_avg[1], vec3(0.02)) +
                     w_crust * crust_c / max(sand_set_avg[2], vec3(0.02));
        c *= mix(vec3(1.0), clamp(ratio, vec3(0.45), vec3(1.6)), sclose * rip * 0.85);
        // (the ripples belong to the loose fine sand: little of them over gravel and crust)
        patches *= 1.0 - (0.6 * w_coarse + 0.8 * w_crust) * sclose;
      }
      // the light's height and its run across the crests: low, each crest shades the trough behind it
      vec3 lw = normalize(world_dir(sun_dir.xyz));
      float l_run = dot(across, lw.xz);
      float l_tan = lw.y / max(abs(l_run), 1e-3);  // the light's rise per meter across the crests
      float self_sh = 0.0;
      // the same ripples at four sizes, each a copy of the smallest scaled up (its wiggles too),
      // each kept while a wave spans 2+ pixels, and their weights renormalized: where the small
      // ones fade the bigger take over at the same strength, so far off looks like up close
      // (the waves' spacing on screen stays about the same, as perspective shrinks them)
      float shade = 0.0, wsum = 0.0;
      for (int k = 0; k < 4; k++) {
        float wl = k == 0 ? 0.24 : k == 1 ? 0.9 : k == 2 ? 3.4 : 12.0;  // wavelength, m
        // (while a wave spans ~2 pixels across the crests; the depth's edges give a huge
        // derivative: there the pixel's own size)
        float m = rip_m < foot * 3.0 ? rip_m : foot;
        float vis = 1.0 - smoothstep(wl / 3.2, wl / 1.25, m);
        if (vis > 0.0) {
          float sc = 0.24 / wl;
          vec2 o = vec2(5.1, 3.3) * float(k);
          // wiggles, and a slow bend of many waves over tens of meters: the wind's direction drifts,
          // the crests turn with it (a rotation would slip by the distance from the world's origin)
          float wk = gnoise(xz * 0.35 * sc + o) * 2.2 + gnoise(xz * 1.3 * sc + o.yx) * 0.6 +
                     gnoise(xz * 0.03 * sc + o * 0.3) * 9.0;
          float phase = dot(xz, across) * 6.2832 / wl + wk * 6.2832;
          float sp = sin(phase), cp = cos(phase);
          float sp2 = 2.0 * sp * cp, cp2 = 1.0 - 2.0 * sp * sp;  // (of the double angle)
          float crest = sp + 0.35 * sp2;
          float slope = cp + 0.7 * cp2;
          // crests broken off here and there (noise long along them, short across): ends and gaps
          // instead of endless parallel lines
          vec2 cr = vec2(dot(xz, vec2(-across.y, across.x)) / (wl * 7.0), dot(xz, across) / (wl * 1.8));
          float whole = 0.25 + 0.75 * smoothstep(-0.3, 0.3, gnoise(cr + o * 1.7));
          // (the smallest fully up close, the next ones mixed under it a little)
          float wgt = vis * (k == 0 ? 1.0 : 0.6);
          shade += wgt * whole * (0.08 * slope * facing + 0.025 * crest);
          // (relief: marched toward the light over one wave, the crests about a fifteenth of their
          // spacing high; where one rises over the line to the sun this spot is in its shadow.
          // The wiggles change slowly over a wave: the phase alone)
          if (lw.y > 0.0 && l_tan < 1.2) {
            const float amp = 0.065;  // of the wavelength
            float h0 = crest;
            float dir = l_run > 0.0 ? 1.0 : -1.0;
            float occ = 0.0;
            // (each step turns the sine and cosine on by a sixth of a wave, and those of the double
            // angle by a third: no trigonometry in the loop. No table of those numbers either: const
            // arrays indexed in a loop cost NVIDIA's compiled shader 6 ms here)
            vec2 r1 = vec2(sp, cp), r2 = vec2(sp2, cp2);
            const vec2 turn1 = vec2(0.5, 0.8660), turn2 = vec2(-0.5, 0.8660);
            for (int s = 1; s <= 6; s++) {
              float dp = float(s) * 1.0472;  // a sixth of a wave
              r1 = vec2(r1.x * turn1.x + dir * r1.y * turn1.y, r1.y * turn1.x - dir * r1.x * turn1.y);
              r2 = vec2(r2.x * turn2.x + dir * r2.y * turn2.y, r2.y * turn2.x - dir * r2.x * turn2.y);
              float hs = r1.x + 0.35 * r2.x;
              // (heights in wavelengths: crest * amp; the line climbs l_tan per wavelength travelled)
              float over = (hs - h0) * amp - l_tan * dp / 6.2832;
              occ = max(occ, smoothstep(0.0, 0.02, over));
            }
            self_sh += wgt * whole * occ;
          }
          wsum += wgt;
        }
      }
      shade *= 2.6 * min(wsum / 0.35, 1.0) / max(wsum, 1e-3);  // (the biggest fades out alone)
      // (the crests' own shadows: only where the sun reaches at all, deepest with it low; bluish,
      // the sky lights them)
      self_sh *= min(wsum / 0.35, 1.0) / max(wsum, 1e-3) * sun_lit * sun_dir.w *
                 (1.0 - smoothstep(0.35, 0.9, lw.y));
      shade -= 0.28 * self_sh;
      // crests across the view (stripes along the horizon) read far too strong in the distance:
      // fainter there, the ones running toward the camera kept
      vec2 look = normalize(world_dir(p).xz + 1e-4);
      float side = abs(dot(look, across));
      float rdist = length(p) / 4096.0;
      shade *= 1.0 - 0.7 * side * side * smoothstep(40.0, 200.0, rdist);
      // and all of them softer far off: the big waves there drew thick dark lines over the dunes
      shade *= 1.0 - 0.65 * smoothstep(50.0, 220.0, rdist);
      float fine = 1.0 - smoothstep(0.006, 0.015, foot);
      float grain = fine > 0.0 ? gnoise(xz * 23.0) * 0.6 + gnoise(xz * 61.0) * 0.4 : 0.0;  // (near only)
      c *= 1.0 + rip * (patches * shade + 0.07 * grain * fine);
      // (and swaths tens of meters across, paler or deeper: wind-sorted sand, not one even tone)
      float swath = gnoise(xz / 38.0 + 1.3) * 0.6 + gnoise(xz / 11.0 + 4.1) * 0.4;
      c *= 1.0 + rip * swath * vec3(0.12, 0.15, 0.2);
      // the sand's light: brighter with the sun at your back (the grains hide their own shadows:
      // the opposition surge), a sheen looking into the low sun at a grazing angle
      vec3 vw = -normalize(world_dir(p));  // toward the eye
      float lit = sun_lit * sun_dir.w * (1.0 - night_dark.x);
      float back = pow(max(dot(vw, lw), 0.0), 8.0);
      float graze = pow(1.0 - max(dot(vw, wn), 0.0), 4.0) * pow(max(dot(-vw.xz, lw.xz), 0.0), 2.0);
      c *= 1.0 + rip * lit * (0.18 * back + 0.22 * graze * (1.0 - 0.5 * lw.y));
      // and glints: here and there a grain (quartz, mica) turned just so mirrors the sun. Each cell
      // of the ground holds one, at a spot of its own and with a tilt of its own; it flashes when
      // its tilt halves the angle between the sun and the eye (they twinkle as the camera moves).
      // Cells never smaller than a couple of pixels; gone where those grow past a few cm
      float cell = max(0.012, foot * 2.5);
      float gv = 1.0 - smoothstep(0.02, 0.05, cell);
      if (gv > 0.0 && lit > 0.0) {
        vec2 gq = xz / cell;
        vec2 gi = floor(gq);
        // (an integer hash: the cells' numbers run to hundreds of thousands)
        vec2 r1 = vec2(ihash3(vec3(gi, 1.0)), ihash3(vec3(gi, 2.0)));
        vec2 r2 = vec2(ihash3(vec3(gi, 3.0)), ihash3(vec3(gi, 4.0)));
        float spot = 1.0 - smoothstep(0.08, 0.3, length(fract(gq) - (0.2 + 0.6 * r1)));
        vec3 gn = normalize(wn + vec3(r2.x * 2.0 - 1.0, 0.0, r2.y * 2.0 - 1.0) * 0.55);
        float mirror = smoothstep(0.985, 0.997, dot(gn, normalize(lw + vw)));
        float glint = spot * mirror * step(0.55, r1.y + r2.x * 0.3) * gv;
        c += rip * glint * lit * vec3(1.6, 1.45, 1.2) * (0.6 + 0.6 * golden);
      }
      // sand on the move even without a storm: thin veils of it skimming the ground downwind, in
      // gusts that come and go, more on the side the wind climbs (near only; the storm has its own)
      float wt = weather.z;
      vec2 perp = vec2(-across.y, across.x);
      float rnear = 1.0 - smoothstep(18.0, 45.0, rdist);
      if (rnear > 0.0 && weather.y < 0.5) {
        float gusts = smoothstep(0.0, 0.45, gnoise(vec2(dot(xz, across) / 30.0 - wt * 0.12, dot(xz, perp) / 22.0)));
        vec2 sq = vec2(dot(xz, across) - wt * 3.5, dot(xz, perp));
        float streak = smoothstep(0.15, 0.55, gnoise(sq * vec2(0.45, 3.2) + 7.0)) *
                       (0.5 + 0.5 * smoothstep(0.0, 0.5, gnoise(sq * vec2(1.6, 9.0) + 2.0)));
        float veil = streak * gusts * rnear * mix(0.6, 1.0, windward) * (1.0 - lee * 0.6) * (1.0 - 2.0 * weather.y);
        float dl = clamp(sky_sun.w * 2.5, 0.0, 1.0);
        c = mix(c, c * vec3(1.18, 1.12, 1.02) + vec3(0.025, 0.02, 0.012) * dl, rip * veil * 0.5);
      }
      // a mirage over the far sand under a high sun: the hot air bends the light, the ground near the
      // horizon shows the sky (and the far hills) upside down below it, in shimmering pools
      float mir = rip * noon * sun_haze * max(1.0 - golden * 2.0, 0.0) * (1.0 - weather.x) * (1.0 - clouds.w) *
                  smoothstep(180.0, 450.0, rdist) * (1.0 - smoothstep(0.0, 0.06, -dot(ray, up_view)));
      if (mir > 0.0) {
        // the horizon's height on screen here, and this pixel mirrored over it
        vec3 hv = ray - up_view * dot(ray, up_view);
        float hy = (hv.y / max(hv.z, 1e-3) * proj.w + 1.0) * 0.5;
        vec2 muv = vec2(tex_coord.x, 2.0 * hy - tex_coord.y);
        muv.x += sin(muv.y * 300.0 + wt * 7.0) * 0.0015;
        vec3 above = texture(tex_T0, clamp(muv, vec2(0.0), vec2(1.0))).rgb;
        float pools = smoothstep(-0.1, 0.35, gnoise(vec2(xz.x / 60.0 + wt * 0.05, xz.y / 25.0)));
        c = mix(c, above * 0.95, clamp(mir * pools, 0.0, 1.0) * 0.55);
      }
    }
  }

  // the ground under a dense grass field (EnhancedGrass): dark, earthy and greenish, the gaps between
  // the blades showing the grass's own shade instead of bright sand; fading out over the field's
  // ragged edge (as shrub_grass.vert thins it)
  if (grass_ground.w > 0.0 && z > 0.0) {
    float gtag = pix_tag;
    if ((gtag > 0.85 && gtag < 0.95) || (gtag > 0.55 && gtag < 0.65)) {
      vec3 gw = view_to_world * p / 4096.0 + cam_pos / 4096.0;
      float lush = texture(tex_T21, (gw.xz - grass_field.xy) / grass_field.z).g - gnoise(gw.xz / 9.0) * 0.15;
      float under = smoothstep(0.08, 0.45, lush) * grass_ground.w *
                    smoothstep(grass_water + 0.05, grass_water + 0.3, gw.y);
      vec3 soil = luma(c) * vec3(0.62, 0.7, 0.34) * 0.75;
      c = mix(c, soil, under * 0.8);
    }
  }

  // grading: the area's tint (shadows lifted toward one color, highlights toward another), then
  // saturation, gentle S-curve, warm highlights / cool shadows
  // rain: colder, the colors kept (a touch richer, like the wet slums at night)
  c *= mix(vec3(1.0), vec3(0.9, 0.98, 1.1), weather.x);
  c = max(mix(vec3(luma(c)), c, 1.0 + 0.1 * weather.x), 0.0);
  // the desert under a high sun: the game lights its ground near white, the eye stops down (the
  // ground only: the sky keeps its own)
  if (sun_haze > 0.0 && z > 0.0 && wsurf == 0.0) {
    float high = sun_haze * noon * (1.0 - clouds.w);
    // (by brightness: the near-white sand fully, the dark rocks little; stopped down alike they went black.
    // Not in the late afternoon's low light: there the shade stays deep)
    c *= 1.0 - high * mix(mix(0.04, 0.3, smoothstep(0.3, 0.8, luma(c))), 0.49, min(golden * 2.0, 1.0));
    // (and the color the glare washed out of it back: sand, not chalk)
    c = max(mix(vec3(luma(c)), c, 1.0 + 0.35 * high), 0.0);
  }
  // the late afternoon: amber and deep, the lit parts richer, the shade darker
  if (golden > 0.0 && z > 0.0) {
    float lg = luma(c);
    float tag = pix_tag;
    // (the characters (merc2.frag: 0.45 long) keep their colors: the deepening is the land's)
    float gk = golden * (tag > 0.3 && tag < 0.55 ? 0.25 : 1.0);
    c = max(mix(vec3(lg), c, 1.0 - 0.15 * gk), 0.0);
    // (the sand stopped down; the mountains lifted, the low sun lights them face on. By the
    // normals' tag lengths, tfrag3.frag / hfrag.frag)
    float rock_expo = mix(1.0, 1.12, smoothstep(15.0, 60.0, length(p) / 4096.0));
    float expo = tag > 0.85 && tag < 0.95 ? 0.82 : tag > 0.7 && tag < 0.8 ? rock_expo : 1.0;
    // (the grey rock slabs in the sand: warm sandstone in this light, not ash)
    if (tag > 0.7 && tag < 0.8) {
      float mxr = max(c.r, max(c.g, c.b));
      float satr = (mxr - min(c.r, min(c.g, c.b))) / max(mxr, 1e-3);
      float grey = (1.0 - smoothstep(0.3, 0.6, satr)) * (1.0 - smoothstep(40.0, 120.0, length(p) / 4096.0));
      // (and lit: the game bakes them near black; the low sun catches their tops)
      float lift = mix(1.55, 1.2, smoothstep(20.0, 80.0, length(p) / 4096.0));
      c = mix(c, min(lg * lift, 0.5) * vec3(1.32, 0.95, 0.62), golden * grey * 0.85);
    }
    c *= mix(vec3(1.0), vec3(1.05, 0.99, 0.92) * expo, golden);
    c *= 1.0 - 0.22 * gk;  // (and all of it stopped down a little: low light, deeper)
    // (the far mountains: their sunlit faces orange, the ones turned away in a deep, cooler shade)
    if (tag > 0.7 && tag < 0.8) {
      float far = smoothstep(100.0, 300.0, length(p) / 4096.0);
      // (by the low sun where its glow is: the faces toward it lit, the ones toward us backlit)
      vec2 brg = normalize(world_dir(sun_dir.xyz).xz + 1e-4);
      float lit = smoothstep(-0.05, 0.35, dot(world_dir(surf_n), vec3(brg.x * 0.95, 0.3, brg.y * 0.95)));
      vec3 lg3 = vec3(luma(c));
      vec3 sunny = max(mix(lg3, c, 1.35), 0.0) * vec3(1.1, 0.95, 0.78);
      vec3 shade = c * vec3(0.66, 0.6, 0.6);
      c = mix(c, mix(shade, sunny, lit), golden * far * (tune_far_dbg == 5.0 ? 0.0 : 1.0));
      // (the rocks near: the faces the low sun reaches catch it, the rest in a cool shade)
      float lit_near = smoothstep(-0.1, 0.55, dot(world_dir(surf_n), normalize(world_dir(sun_dir.xyz) + vec3(0.0, 1e-4, 0.0))));
      c *= mix(vec3(1.0), mix(vec3(0.6, 0.62, 0.72), vec3(1.5, 1.28, 1.0), lit_near), golden * (1.0 - far));
      // (and, looking toward the sun, the warm haze between: backlit mountains glow amber)
      vec2 shz = normalize(world_dir(sky_sun.xyz).xz + 1e-4);
      float toward = max(dot(normalize(world_dir(ray).xz + 1e-4), shz), 0.0);
      c += vec3(0.24, 0.12, 0.035) * pow(toward, 5.0) * golden * far;
    }
    c *= 1.0 - 0.4 * gk * (1.0 - smoothstep(0.2, 0.55, lg));

  }
  // by day the same on rock: the game bakes the mountains evenly dark, so the sun's own direction
  // lights them, the faces turned away in a cooler shade (fading out as the late afternoon's takes
  // over)
  // dust and grime gathered at the foot of the walls: a wall's lowest meter a little darker where it
  // stands on a floor (found a few steps below on screen: the first point lower down that faces up)
  if (z > 0.0 && wsurf == 0.0 && abs(world_dir(surf_n).y) < 0.35 && length(p) < 60.0 * 4096.0 &&
      pix_tag > 0.985) {
    float h = 1e9;
    for (int k = 1; k <= 4; k++) {
      vec2 uq = tex_coord - vec2(0.0, texel.y * float(k * k) * 5.0);
      float dq = texture(tex_T3, uq).r;
      vec4 gq = gbuf(uq);
      if (dq > 1e-7 && gq.a > 0.17 && (gq.rgb * 2.0 - 1.0).y > 0.7) {
        vec3 dw = view_to_world * (view_pos(uq, dq) - p) / 4096.0;
        if (dw.y < 0.0 && length(dw.xz) < 1.5) {
          h = min(h, -dw.y);
        }
      }
    }
    c *= 1.0 - 0.2 * (1.0 - smoothstep(0.05, 1.1, h)) * (1.0 - smoothstep(30.0, 60.0, length(p) / 4096.0));
  }
  // Jak out of the water: dark and shining for some seconds, as he dries (the pixels around him,
  // characters' tag)
  if (jak_fx.w > 0.0 && z > 0.0 && wsurf == 0.0) {
    float ctag = pix_tag;
    float jd = length(p - jak_fx.xyz) / 4096.0;
    if (ctag > 0.4 && ctag < 0.56 && jd < 2.5) {
      float wetk = jak_fx.w * (1.0 - smoothstep(1.8, 2.5, jd));
      vec3 vd = normalize(p);
      float glint = pow(max(dot(reflect(vd, surf_n), sun_dir.xyz), 0.0), 30.0) * sun_dir.w * (1.0 - clouds.w);
      float sheen = pow(1.0 - clamp(dot(surf_n, -vd), 0.0, 1.0), 4.0);
      c *= 1.0 - 0.18 * wetk;
      c += wetk * (sun_color() * glint * 0.6 + vec3(0.6, 0.7, 0.8) * sheen * 0.12);
    }
  }
  // characters lit from behind by the sun: a thin bright rim along their outline, so they stand out
  // from the scenery (by day, outdoors: the areas with a day sky)
  if (z > 0.0 && sky_day > 0.0 && wsurf == 0.0) {
    float ctag = pix_tag;
    if (ctag > 0.4 && ctag < 0.56) {
      float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w) * (1.0 - weather.x);
      vec3 vd = normalize(-p);
      float edge = pow(1.0 - clamp(dot(surf_n, vd), 0.0, 1.0), 3.0);
      float behind = smoothstep(-0.1, 0.6, dot(normalize(p), sun_dir.xyz));
      float side = clamp(dot(surf_n, sun_dir.xyz) + 0.5, 0.0, 1.0);
      c += vec3(1.0, 0.86, 0.66) * edge * behind * side * daylight * sky_day * 0.35 *
           (1.0 - smoothstep(15.0, 40.0, length(p) / 4096.0));
    }
    // leaves against the sun: thin, the light shines through them, a luminous yellow-green (or
    // gold for autumn leaves) instead of a dark cut-out (foliage: green or orange-dominant plain
    // scenery, looking toward the sun)
    if (ctag > 0.95) {
      float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w) * (1.0 - weather.x);
      float toward = smoothstep(0.3, 0.95, dot(normalize(p), sun_dir.xyz));
      float leafy = smoothstep(0.03, 0.12, c.g - c.b) * smoothstep(0.0, 0.08, max(c.g, c.r) - c.b - 0.02);
      float k = toward * leafy * daylight * sky_day * (1.0 - smoothstep(60.0, 200.0, length(p) / 4096.0));
      c += c * vec3(0.9, 1.0, 0.45) * k * 0.6;
    }
  }
  // the far air takes the sky's color: warm toward the sun, cool and bluer away from it, so the
  // distance reads deeper (by day; the desert has its own haze)
  if (z > 0.0 && sky_day > 0.0 && sun_haze == 0.0) {
    float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - weather.x);
    float far_k = smoothstep(120.0, 1200.0, length(p) / 4096.0) * daylight * sky_day;
    float toward = dot(normalize(p), sky_sun.xyz) * 0.5 + 0.5;
    vec3 air = mix(vec3(0.9, 0.97, 1.08), vec3(1.08, 1.0, 0.88), smoothstep(0.35, 0.95, toward));
    c = mix(c, c * air, far_k * 0.6);
  }
  vec3 pre_relight = c;
  if (z > 0.0 && golden < 0.5 && (sun_haze > 0.0 || rock_sun > 0.0 || wall_sun > 0.0)) {
    float tag = pix_tag;
    if (tag > 0.7 && tag < 0.8 && rock_sun > 0.0) {
      // (the morning isn't dawn here: the sun is up and lights the rock)
      float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w * (1.0 - sky_day)) * (1.0 - weather.x);
      float rndl = dot(world_dir(surf_n), normalize(world_dir(sun_dir.xyz) + vec3(0.0, 1e-4, 0.0)));
      // (the low morning sun grazes most of a peak: the light wraps further round, only the faces
      // turned right away in shade)
      float lit = smoothstep(mix(-0.05, -0.4, sky_day), mix(0.45, 0.3, sky_day), rndl);
      // (under the desert's high sun most of a cliff stands nearly edge on to it: there the narrow step
      // above turned the mesh's few normals into black and white patches)
      float high_sun = noon * min(sun_haze * 1.25, 1.0);
      lit = mix(lit, smoothstep(-0.6, 0.9, rndl), high_sun);
      float far = smoothstep(30.0, 150.0, length(p) / 4096.0);
      // (in the morning the faces away from the low sun deep and lilac-brown, the lit ones warm)
      vec3 shade_k = mix(vec3(0.8, 0.83, 0.92), vec3(0.6, 0.54, 0.64), sky_day);
      // (in the desert under the high sun gentler, both ways: the faces burned orange or went near black, their
      // texture lost)
      shade_k = mix(shade_k, vec3(0.95, 0.96, 1.0), noon * min(sun_haze * 1.25, 1.0));
      vec3 lit_k = mix(vec3(1.5, 1.38, 1.2), vec3(1.85, 1.45, 1.05), sky_day);
      lit_k = mix(lit_k, vec3(1.2, 1.14, 1.06), noon * min(sun_haze * 1.25, 1.0));
      vec3 rock_k = mix(shade_k, lit_k, lit);
      // (and what the shadow map has in shade isn't told apart by its faces: one even light from the
      // sky, the texture and the game's own shading kept)
      rock_k = mix(rock_k, mix(vec3(1.6, 1.42, 1.2), rock_k, 1.0 - rock_sh), high_sun);
      vec3 relit = c * rock_k;
      // (at noon the sandstone's faces in the sun brighter and warmer: it read grey-brown)
      relit = mix(relit, max(mix(vec3(luma(relit)), relit, 1.3), 0.0) * mix(vec3(0.95), vec3(1.2, 1.08, 0.92), lit),
                  desert_noon());
      c = mix(c, relit, daylight * (1.0 - golden * 2.0) * mix(mix(0.55, 0.9, far), 1.0, sky_day) * rock_sun);
    }
    if (tag > 0.985 && wsurf == 0.0 && sun_haze > 0.0) {
      // (under the desert's high sun the walls' pale plaster: the patches of sun near white, the rest
      // dark, the shape of the shadow map's edge all over them. Closer together)
      c *= mix(vec3(1.0), mix(vec3(0.72, 0.71, 0.72), vec3(1.6, 1.48, 1.3), rock_sh), noon * min(sun_haze * 1.25, 1.0) * clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - weather.x));
    }
    // the city's walls the same, milder: the game lights a street's both sides alike, so the faces
    // toward the sun a little brighter and warmer, the others a little cooler (not the characters,
    // the shrubs or the sky: plain scenery only)
    if (tag > 0.985 && wall_sun > 0.0 && wsurf == 0.0) {
      float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - clouds.w * (1.0 - sky_day)) * (1.0 - weather.x);
      float wndl = dot(world_dir(surf_n), normalize(world_dir(sun_dir.xyz) + vec3(0.0, 1e-4, 0.0)));
      float lit = smoothstep(-0.15, 0.4, wndl);
      vec3 relit = c * mix(vec3(0.88, 0.9, 0.96), vec3(1.22, 1.15, 1.04), lit);
      // (not far off, where the haze takes over, nor on the ground, which the shadows already shape)
      float wall = 1.0 - smoothstep(0.6, 0.9, abs(world_dir(surf_n).y));
      float near_k = 1.0 - smoothstep(150.0, 500.0, length(p) / 4096.0);
      c = mix(c, relit, daylight * (1.0 - golden * 2.0) * wall * near_k * wall_sun);
    }
    // (and the sand and the rocky ground: the game lights them a greyish white, stopped down above
    // they went a dull grey. Warm golden sand instead, its brightness kept, the sun's side and
    // the shade further apart; less far off, where the haze takes over)
    const vec3 noon_sand = vec3(1.13, 1.0, 0.8);  // its color over its brightness: pale warm cream
    const float noon_mix = 0.3;
    const float noon_sunlit = 1.15;
    if (((tag > 0.85 && tag < 0.95) || (tag > 0.55 && tag < 0.65)) && sun_haze > 0.0) {
      float daylight = clamp(sky_sun.w * 4.0, 0.0, 1.0) * (1.0 - weather.x);
      float k = daylight * (1.0 - golden * 2.0) * sun_haze *
                (1.0 - 0.6 * smoothstep(80.0, 400.0, length(p) / 4096.0));
      // (the game's baked shadows: the light of one hour in its vertex colors, hard triangles on
      // the sand that never move. Where the shadow map has the sun, dark ground is brought up
      // toward the sunlit sand's brightness, its texture kept)
      {
        const float noon_sand_luma = 0.62;
        float lb = luma(c);
        float under = smoothstep(0.88, 0.55, lb / noon_sand_luma);
        float gain = clamp(noon_sand_luma * 0.95 / max(lb, 0.02), 1.0, 1.9);
        c *= mix(1.0, gain, under * sun_lit * k);
      }
      vec3 warm = luma(c) * (tag > 0.85 ? noon_sand : noon_sand * vec3(1.02, 0.97, 0.9));
      c = mix(c, warm, k * noon_mix);
      // (the shade under a high sun: lit by the blue sky and the bright sand around, cooler and not
      // as deep)
      c *= mix(vec3(1.0), mix(vec3(0.8, 0.84, 0.95), vec3(noon_sunlit), sun_lit), k);
    }
  }
  c = mix(c, pre_relight, dust_veil);
  // the desert at noon: the game's dusty fog and the area's warm grade made everything sepia; the
  // light's white back (the sky's blue kept), a touch brighter
  if (z > 0.0 && wsurf == 0.0) {
    float nd = desert_noon() * (1.0 - weather.x);
    // (and the noon's colors as deep as the game's: the glare and the haze above paled them)
    c *= mix(vec3(1.0), vec3(1.03, 1.02, 1.03), nd);
    c = max(mix(vec3(luma(c)), c, 1.0 + 0.18 * nd), 0.0);
  }
  c = c * grade_gain + grade_lift * (1.0 - clamp(c, 0.0, 1.0));
  float l = luma(c);
  c = max(mix(vec3(l), c, saturation * (1.0 + 0.12 * weather2.z)), 0.0);
  // (on 0..1, what's over it kept: past 1 the curve falls back to 0, the flames' bright red cores
  // went green and blue)
  vec3 cc = clamp(c, 0.0, 1.0);
  vec3 curve = cc * cc * (3.0 - 2.0 * cc) + (c - cc);
  c = mix(c, curve, contrast * (1.0 - 0.6 * noon * day_look));
  // (less in the brightest parts: they'd go yellow)
  // (a warm grade (dusk) only a little cooler in the shadows: at 18-19h the whole of a dark street
  // is shadow, and the full split pushed Haven's slums deep navy, red near 0)
  float wl = l - 0.35;
  wl *= (wl < 0.0 && warmth > 0.0) ? tune_warm_shadow : 1.0;
  c += warmth * vec3(1.0, 0.25, -0.75) * wl * (1.0 - 0.6 * smoothstep(0.55, 0.9, l));

  // the desert under a high sun: the game's ground comes in near white (bluish on the slopes), the
  // grade above whiter still: its highlights rolled off softly (a film shoulder) and warmed back
  // to sand, instead of paper
  if (sun_haze > 0.0 && z > 0.0 && wsurf == 0.0) {
    float ls = luma(c);
    // (only the near whites: one squeezing everything over the mid tones flattened the sand,
    // its ripples and soft shading gone; the exposure above brings it down instead)
    if (ls > 0.7) {
      float to = 0.7 + (ls - 0.7) * 0.6 / (1.0 + (ls - 0.7) * 2.0);
      vec3 tone = c * (to / ls);
      tone = max(mix(vec3(to), tone, 1.35), 0.0);  // (some of the saturation back)
      float k = smoothstep(0.75, 0.95, ls);        // (the whiter, the more toward sand)
      tone = mix(tone, to * vec3(1.2, 0.98, 0.7), 0.7 * k);
      c = mix(c, tone, min(sun_haze * 1.25, 1.0) * (1.0 - flame_keep));
    }
  }

  // (the flames as the game drew them: a sprite with no depth of its own, everything above shaded
  // it as what's behind it, the palm's trunk in its shadow: dim, green-blue)
  // the desert's morning on the ground: warm peach-orange sand and rock (the game's grade leaves
  // them a pale yellow-beige), the far rock richer (the haze greyed it to mauve)
  if (day_look > 0.0 && z > 0.0 && wsurf == 0.0) {
    float mtag = pix_tag;
    float ground = (mtag > 0.3 && mtag < 0.55) ? 0.25 : 1.0;  // (characters a little)
    float far_m = smoothstep(80.0, 400.0, length(p) / 4096.0);
    vec3 g = c * mix(vec3(1.02, 0.91, 0.89), vec3(1.05, 0.9, 0.78), far_m);
    g = mix(vec3(luma(g)), g, 1.04 + 0.15 * far_m);
    g *= mix(1.0, 1.08, far_m);
    // (what the day's light burns near white, the grass's pale tips at noon above all, back to
    // straw: a little darker and warm, not paper)
    // (the sand never darker than a dusky lilac-grey, lit by the open sky: the shade, and the whole
    // plain at the sunset (the game's own light there near black))
    if (mtag > 0.85 && mtag < 0.95) {
      float lsh = luma(g);
      vec3 lifted = g * clamp(0.2 / max(lsh, 0.02), 1.0, 2.0) * vec3(0.97, 0.97, 1.05);
      g = mix(g, lifted, 1.0 - far_m);
    }
    float hl = smoothstep(0.72, 0.95, luma(g));
    g = mix(g, luma(g) * vec3(1.08, 0.96, 0.66) * 0.93, hl * 0.75);
    c = mix(c, g, day_look * ground);
    // (under the high sun not as rich as the low sun makes it)
    c = max(mix(vec3(luma(c)), c, 1.0 - 0.2 * noon * day_look * ground), 0.0);
  }
  // the desert's rock by day never past a sandstone's saturation: the relight's warmth, the grade's
  // and the morning look's stacked up and the far mountains went the most saturated orange in the
  // frame (the low sun keeps its own, richer)
  if (sun_haze > 0.0 && z > 0.0 && wsurf == 0.0 && tune_rock_sat > 0.0 && pix_tag > 0.7 && pix_tag < 0.8) {
    float mxr = max(c.r, max(c.g, c.b));
    float satr = (mxr - min(c.r, min(c.g, c.b))) / max(mxr, 1e-3);
    if (satr > tune_rock_sat) {
      float k = mix(1.0, tune_rock_sat / satr, (1.0 - min(golden * 2.0, 1.0)) * (1.0 - flame_keep));
      c = max(mix(vec3(luma(c)), c, k), 0.0);
    }
  }
  // sand outside the desert (Sandover's beach): in the shade never darker than the open sky
  // lights it, a soft grey-blue (the game's own morning light there near black)
  if (sky_day > 0.0 && sun_haze == 0.0 && z > 0.0 && wsurf == 0.0) {
    float stag = pix_tag;
    if (stag > 0.85 && stag < 0.95) {
      float lsh = luma(c);
      vec3 lifted = c * clamp(0.34 / max(lsh, 0.02), 1.0, 2.4) * vec3(1.02, 1.0, 0.97);
      c = mix(c, lifted, sky_day * (1.0 - smoothstep(80.0, 400.0, length(p) / 4096.0)));
    }
  }
  c = mix(c, flame_base, flame_keep);
  // see-through cloth (Jak's tunic and scarf: the 0.52 tag) as the game draws it: the post's light
  // and shade made it look transparent
  {
    float cl = pix_tag;
    c = mix(c, max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0), (cl > 0.505 && cl < 0.535) ? 1.0 : 0.0);
    // characters in a shadow, once more after the grade: the grade spares them the land's evening
    // deepening and the cloth above is the game's own color, so in a wall's shade Jak's hair and
    // Daxter stayed sunlit over near-black ground. Darker and a little cooler, like the sky-lit ground
    if (cl > 0.3 && cl < 0.55 && rock_sh > 0.0) {
      // (the lower the sun, the deeper and duller the shade: at dusk the hair and Daxter stayed
      // bright and saturated over near-black ground)
      float dusk = 1.0 - chr_bounce;
      float dk = min(tune_chr_dark + tune_chr_dusk * dusk, 0.9);
      c = mix(c, mix(vec3(luma(c)), c, 1.0 - tune_chr_dusk_desat * dusk), rock_sh);
      // (toward brown, not grey-blue: dark yellow hair taking the cool tint read olive green)
      vec3 shade_tint = mix(vec3(0.92, 0.96, 1.04), vec3(1.0, 0.93, 0.86), tune_chr_dusk_warm * mix(0.5, 1.0, dusk));
      c *= mix(vec3(1.0), shade_tint * (1.0 - dk), rock_sh);
    }
    // (and at a low sun never darker than this share of the game's own color, in its hue: in a
    // Spargus street at 18h the shade, the grade and the darkening above crushed Jak near black,
    // face and clothes murky, where the game has him readable. Not by night: the night keeps its own)
    if (tune_chr_floor > 0.0 && cl > 0.3 && cl < 0.55 && z > 0.0) {
      vec3 fl = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0) * tune_chr_floor;
      float lf = luma(fl);
      float lc = luma(c);
      // (and at any sun height at half weight: under a roof with the sun high, the Metal Head tower,
      // the shade path left Jak a near-black silhouette in some frames)
      float w = max(1.0 - chr_bounce, tune_chr_floor_any) * (1.0 - night_dark.x);
      if (lf > 1e-3 && lc < lf) {
        c = mix(c, fl, w * smoothstep(0.0, 0.3, (lf - lc) / lf));
      }
    }
  }

  // characters at night: a faint rim of the moon's and sky's light along their edges, so Jak and
  // Daxter read against the dark dock (the scene stays as dark; only the silhouette's last pixels)
  if (tune_chr_rim > 0.0 && night_dark.x > 0.0 && pix_tag > 0.3 && pix_tag < 0.55 && z > 0.0) {
    float ndv = clamp(dot(surf_n, normalize(-p)), 0.0, 1.0);
    float rim = pow(1.0 - ndv, 3.0) * night_dark.x * ao * (1.0 - 0.6 * rock_sh);
    c += vec3(0.42, 0.52, 0.78) * rim * tune_chr_rim * (0.35 + luma(c));
  }

  // characters at golden hour: the low sun catches their edges on its side, most when it is behind
  // them (Jak flat and dark against the warm ground at Spargus 18h). Not in the shadow map's shade
  if (tune_chr_gold_rim > 0.0 && golden > 0.0 && sky_sun.w > 0.0 && pix_tag > 0.3 && pix_tag < 0.55 && z > 0.0) {
    vec3 v = normalize(-p);
    float ndv = clamp(dot(surf_n, v), 0.0, 1.0);
    float side = clamp(dot(surf_n, sky_sun.xyz), 0.0, 1.0);
    float back = smoothstep(-0.4, 0.6, dot(-v, sky_sun.xyz));
    float rim = pow(1.0 - ndv, 3.0) * (0.3 + 0.7 * side) * back * golden * sun_lit * (1.0 - 0.6 * rock_sh);
    c += sun_color() * rim * tune_chr_gold_rim * (0.3 + luma(c));
  }
  // the low sun's sky fill: the ground turned from it is still lit by the whole bright sky and the
  // sunlit dunes around. The rocky ground's low-light darkening (x0.5), the relight, the golden
  // stop-down and the deepening of dark parts stacked up to x0.4 of the game's color, and a near
  // slope went black-brown with a hard edge: 0.35 of the flat sand, where the game has it 1.15.
  // Never darker than this share of the game's own color (sand, rocky ground, rock), warm
  if (tune_gold_fill > 0.0 && golden > 0.0 && z > 0.0 && pix_tag > 0.55 && pix_tag < 0.95) {
    vec3 fill = texture(tex_T0, tex_coord).rgb * vec3(1.0, 0.9, 0.8) * tune_gold_fill;
    c = mix(c, max(c, fill), golden * gold_w);
  }
  // the whole picture leveled a little: the strongest colors pulled back (the pale ones stay as they
  // are) and the near whites rolled off (above the sand, whose grain they flattened), so that nothing burns (not the flames, the game's own)
  {
    float top = max(c.r, max(c.g, c.b));
    float chroma = top > 0.001 ? (top - min(c.r, min(c.g, c.b))) / top : 0.0;
    vec3 level = mix(vec3(luma(c)), c, 1.0 - 0.2 * smoothstep(0.3, 0.85, chroma));
    float over = max(top - 0.82, 0.0);
    level = max(level, 0.0) * (0.82 + over / (1.0 + over / 0.25)) / max(top, 0.82);
    c = mix(c, level, 0.5 * (1.0 - flame_keep));
  }
  // lightning, after the grade (white, whatever the area's tint): the flash lights the sky cold
  // for an instant (the ground below only faintly: it's far off in the clouds), and the bolt over
  // the sky
  if (weather2.w > 0.0 || bolt_light > 0.0 || sheet > 0.0) {
    vec3 cold = vec3(0.85, 0.9, 1.0);
    c += cold * weather2.w * (z == 0.0 ? 0.06 + c * 0.6 : 0.005 + c * 0.08);
    // sheet lightning: the whole sky glows from inside the clouds
    c += cold * sheet * (z == 0.0 ? 0.26 + c * 0.5 : 0.006 + c * 0.07);
    c += cold * bolt_light * 2.2;
  }

  // (the eye's lift of a dark room spares its bright parts: the palace's honeycomb windows at noon,
  // over the sky, went from the game's 74,54,44 to clipped white panes)
  c *= eye_adapt > 1.0 ? mix(eye_adapt, 1.0, tune_adapt_hi * smoothstep(tune_adapt_hi_lo, 0.9, max(c.r, max(c.g, c.b))))
                       : eye_adapt;
  // the player's brightness (menu "Brightness"): neutral (all 0) skips it, the picture as before.
  // Exposure by the hour (milder on what is already bright: the sky and lamps don't clip), then a
  // toe that raises the darks and keeps black black, by night with a faint cool fill
  if (brightness != vec4(0.0)) {
    float bm = max(c.r, max(c.g, c.b));
    float bg = 1.0 + brightness.x;
    c *= bg > 1.0 ? mix(bg, 1.0 + (bg - 1.0) * 0.35, smoothstep(0.5, 1.0, bm)) : bg;
    float bd = 0.09 / (dot(c, vec3(0.299, 0.587, 0.114)) + 0.09);
    bd *= bd;
    c *= 1.0 + (brightness.y * 1.6 + brightness.z * 1.4) * bd;
    c += brightness.z * vec3(0.55, 0.7, 1.0) * 0.018 * bd;
  }

  // the game's translucent light columns (the arena's shaft down the wall): their blended draw left
  // the normal target longer than any tag; the shade, occlusion and dusk grade below then took the
  // wall behind for the surface and the column vanished (vanilla draws it). Never darker than the
  // game's own picture there (its effects taken out: they come back below)
  float shaft_m = 0.0;
  if (tune_shaft_keep > 0.0 && z > 0.0) {
    vec3 gn = texture(tex_T11, tex_coord).rgb * 2.0 - 1.0;
    vec3 raw_scene = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
    // (only where the game's picture is bright: a decal strip blended over a floor marks it too. On
    // walls from a lower luma: the column's dim translucent outer third was crushed to black beside its
    // restored core, a hard vertical edge; floors keep the high gate)
    float shaft_lo = mix(tune_shaft_lo, 0.3, smoothstep(0.6, 0.85, abs(dot(surf_n, up_view))));
    shaft_m = smoothstep(tune_shaft_n0, tune_shaft_n1, dot(gn, gn)) *
              smoothstep(shaft_lo, shaft_lo + 0.2, luma(raw_scene)) * tune_shaft_keep;
    c = mix(c, max(c, raw_scene), shaft_m);
    // the column's light on the wall right beside it: within ~4% of the screen of a bright column
    // pixel, never darker than the game's picture times a falloff (the shaded wall cut off at the
    // column's side, 33 -> 140 luma in one step at dusk where vanilla goes 99 -> 138)
    if (tune_shaft_spill > 0.0 && shaft_m < 0.99) {
      // (a spiral of 8 taps turned per pixel and frame, the TAA averages it: a smooth falloff)
      float near_m = 0.0;
      float sj = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + haze.w * 7.13);
      for (int k = 0; k < 8; k++) {
        float f = (float(k) + sj) / 8.0;
        float a = float(k) * 2.39996 + sj * 6.2832;
        float r = 0.045 * f * tune_shaft_spill_r;
        vec2 tuv = tex_coord + vec2(cos(a) * r * texel.x / texel.y, sin(a) * r);
        vec3 tn = texture(tex_T11, tuv).rgb * 2.0 - 1.0;
        float tm = smoothstep(tune_shaft_n0, tune_shaft_n1, dot(tn, tn));
        if (tm > 0.0) {
          tm *= smoothstep(0.3, 0.5, luma(texture(tex_T0, tuv).rgb));
          near_m = max(near_m, tm * (1.0 - f));
        }
      }
      c = mix(c, max(c, raw_scene * tune_shaft_spill), near_m * (1.0 - shaft_m));
    }
  }

  // the low sun's shade floor, last: never under tune_dusk_floor of the game's own color, and in the
  // game's hue. Before the golden and warm grade it came back saturated orange, the blue near 0
  // (arena wall beside the column at 19h 40,22,8, vanilla 73,53,31): a luma floor whose color is
  // the game's, taken over the more the post went under it
  if (tune_dusk_floor_end > 0.0 && tune_dusk_floor > 0.0 && dusk_floor_w > 0.0) {
    vec3 game_c = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
    vec3 fl = game_c * tune_dusk_floor;
    float lf = luma(fl);
    float lc = luma(c);
    float dd = clamp((lf - lc) / max(lf, 1e-3), 0.0, 1.0);
    c = mix(c, fl * (max(lc, lf) / max(lf, 1e-4)), dusk_floor_w * smoothstep(0.0, tune_dusk_floor_soft, dd));
  }

  // the morning's low sun: the land never much more saturated than the game draws it. The area's
  // grade, the contrast curve (it takes the small channels: blue) and the morning's peach look
  // stacked: Spargus' rock slabs at 7h went lime-orange (147,107,51 vs vanilla 125,101,60, peaks
  // 228,173,93) and the sand orange-red
  if (tune_morning_sat_cap > 0.0 && z > 0.0 && !(pix_tag > 0.3 && pix_tag < 0.55)) {
    float mw = max(morning, sky_morning) * (1.0 - flame_keep);
    if (mw > 0.0) {
      vec3 game_c = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
      float gt = max(game_c.r, max(game_c.g, game_c.b));
      float sg = gt > 0.02 ? (gt - min(game_c.r, min(game_c.g, game_c.b))) / gt : 0.0;
      float ct = max(c.r, max(c.g, c.b));
      float sc = ct > 0.02 ? (ct - min(c.r, min(c.g, c.b))) / ct : 0.0;
      float cap = max(sg * tune_morning_sat_cap, 0.15);
      if (sc > cap) {
        c = mix(c, max(mix(vec3(luma(c)), c, cap / sc), 0.0), mw);
      }
    }
  }
  // night: the dim land in the moon's hue, the game's. Jak's lantern pool (warm, x1.3 by the eye's
  // adaptation) and the shade steps before the night darkening left the moonlit desert grey-lilac
  // (22h box 72,69,93, vanilla 51,69,90; lantern off 50,46,61: mauve). The hue only, the
  // brightness kept; the bright core of a lamp's pool and the flames keep theirs
  float night_hue_k = (pix_tag > 0.3 && pix_tag < 0.55) ? tune_night_hue_chr : 1.0;
  if (tune_night_hue > 0.0 && clouds.w > 0.0 && z > 0.0 && night_hue_k > 0.0) {
    vec3 game_c = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
    float lg = luma(game_c);
    if (lg > 0.01) {
      float lc = luma(c);
      vec3 gh = game_c * (lc / lg);
      float w = tune_night_hue * night_hue_k * clouds.w * (1.0 - smoothstep(tune_night_hue_lo, tune_night_hue_hi, lc)) * (1.0 - flame_keep);
      c = mix(c, max(gh, 0.0), w);
    }
  }
  // (test: tune_night_win >= 10 shows the glossy pixels magenta, the window panes cyan)
  if (tune_night_win > 9.0 && z > 0.0 && pix_normal.a > 0.17 && pix_normal.a < 0.5) {
    c = pix_tag > 0.985 && pix_tag < 0.9925 ? vec3(0.0, 1.0, 1.0) : vec3(1.0, 0.0, 1.0);
  }
  // at night some of the window panes are lit from inside (EnhancedMaterials.h's kWindow: glossy,
  // 0.99 long): a warm light, one in tune_night_win_share of the world's cells (a pane's room), each
  // its own strength; above the bloom's knee so the far city gets points of light, and the water's
  // reflections take them. The dark around them stays as dark
  if (tune_night_win > 0.0 && clouds.w > 0.0 && z > 0.0 && pix_normal.a > 0.17 && pix_normal.a < 0.5 &&
      pix_tag > 0.985 && pix_tag < 0.9925) {
    vec3 wp = (view_to_world * p + cam_pos) / 4096.0;
    vec3 cell = floor(wp / vec3(tune_night_win_cell, tune_night_win_cell * 1.25, tune_night_win_cell));
    float h1 = fract(sin(dot(cell, vec3(127.1, 311.7, 74.7))) * 43758.5453);
    float h2 = fract(sin(dot(cell, vec3(269.5, 183.3, 246.1))) * 43758.5453);
    float lit = step(h1, tune_night_win_share) * smoothstep(0.3, 0.8, clouds.w);
    if (lit > 0.0) {
      // (warm to pale, by the room; the pane's own picture kept as a little shading over it)
      vec3 wc = mix(vec3(1.0, 0.62, 0.3), vec3(1.0, 0.85, 0.6), h2) * mix(0.6, 1.0, fract(h2 * 7.3));
      float pic = luma(texture(tex_T0, tex_coord).rgb);
      wc *= tune_night_win * (0.75 + 0.5 * smoothstep(0.0, 0.3, pic));
      c = mix(c, max(c, wc), lit);
    }
  }
  // under a roof the sky through the windows as the game drew it, and what it drew over it with no
  // depth (the palace's honeycomb lattice): the bloom of the bright panes on themselves (+40%), the
  // eye's lift of the dark room (+20%) and the grade made them flat white panes at noon (box 142,104,79,
  // vanilla 74,54,44), the lattice gone
  // (the palace's panes are mostly not the sky but a blended pane with the sky painted on, ~25 m
  // away: its draw left the normal target longer than any tag. Shaded, relit and bloomed as a wall
  // they went cream with drifting blotches at 18h, the lattice lost: those too as the game drew them.
  // tune_pane_keep 0: as before)
  float pane_m = 0.0;
  if (tune_pane_keep > 0.0 && enclosed > 0.0 && z > 0.0) {
    vec3 gn = texture(tex_T11, tex_coord).rgb * 2.0 - 1.0;
    pane_m = smoothstep(tune_shaft_n0, tune_shaft_n1, dot(gn, gn)) * tune_pane_keep;
  }
  if (tune_sky_enclosed_keep > 0.0 && enclosed > 0.0 && (d <= 1e-7 || pane_m > 0.0)) {
    vec3 game_c = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
    c = mix(c, game_c * tune_sky_enclosed_gain,
            enclosed * tune_sky_enclosed_keep * (d <= 1e-7 ? 1.0 : pane_m));
  }

  // (indoor smoke and dust the game blended over the floor (the palace hall's grey wisps) stay in the
  // scene and were relit and graded as floor: their light went with the floor's (grey-wisp share
  // 0.14% of the lower half vs vanilla 0.21%). What they added, back, never past the game's colour)
  if (tune_smoke_floor > 0.0 && enclosed > 0.0 && z > 0.0) {
    vec3 sm = max(fx_rest, 0.0) * (1.0 - fx_w);
    if (max(sm.r, max(sm.g, sm.b)) > 0.004) {
      vec3 game_c = max(texture(tex_T0, tex_coord).rgb - fx_w * fx_d, 0.0);
      c = mix(c, max(c, min(game_c, c + sm * tune_smoke_floor)), enclosed);
    }
  }

  // the light effects back, over the graded scene (not stopped down with the bright ground, not
  // adapted to the glare): a gain by day, more over bright ground, their color back, and the sum
  // rolled off at the top with its hue kept
  if (fx_w > 0.0) {
    // (over dark ground (a shaded alley at dawn) an effect never vanished: the full gain there grew
    // an enemy's small purple shot into a saturated magenta blob with bloom, 9 -> ~300 px)
    float g_dark = mix(tune_fx_dark, 1.0, smoothstep(tune_fx_dark0, tune_fx_dark1, fx_under));
    float g = 1.0 + (tune_fx_gain - 1.0) * g_dark * (1.0 + tune_fx_bright * smoothstep(0.25, 0.75, fx_under)) * fx_day;
    vec3 fa = fx_d * g;
    fa = max(mix(vec3(luma(fa)), fa, mix(1.0, tune_fx_sat, fx_day)), 0.0);
    vec3 t = c + fa * fx_w;
    float m = max(t.r, max(t.g, t.b));
    float k = tune_fx_knee;
    if (m > k) {
      float s = k + (1.0 - k) * (1.0 - exp(-(m - k) / max(1.0 - k, 1e-3)));
      t = mix(t, t * (s / m), fx_w);
    }
    c = t;
  }
  if (tune_fx_debug > 0.5) {
    c = tune_fx_debug < 1.5 ? fx_d * 4.0 : tune_fx_debug < 2.5 ? vec3(fx_w) :
        tune_fx_debug < 3.5 ? texture(tex_T24, uv).rgb : tune_fx_debug < 4.5 ? vec3(fx_day, 0.5, 0.0) :
        tune_fx_debug < 5.5 ? texture(tex_T0, tex_coord).rgb : tune_fx_debug < 6.5 ? texture(tex_T23, uv).rgb :
        tune_fx_debug > 8.5 ? vec3(flame_keep, fx_w, fx_day) :
        tune_fx_debug > 7.5 ? vec3(shaft_m, fx_w, 0.0) :
        vec3(z / (40.0 * 4096.0), (pix_tag - 0.9) * 5.0, pix_normal.a);
  }
  if (abs(tune_water_dbg - 4.0) < 0.5) {
    c = vec3(wsurf, fx_w, 0.0);  // (test: the water's mask, the effects' weight)
  }
  if (tune_shadow_dbg > 1.5) {
    c = vec3(rock_sh, sun_lit, pix_tag);  // (test: the shadow, the sun's light, the material tag)
    if (tune_shadow_dbg > 4.5 && z > 0.0) {
      // (test: how far above this point the near map's caster is: red 0-2 m, green 2-20 m, blue 20-200 m)
      vec3 sw = (light_mvp * vec4(view_to_world * p + cam_pos, 1.0)).xyz * 0.5 + 0.5;
      float gap = (sw.z - texture(tex_T13, sw.xy).r) * shadow_params.x;
      c = vec3(gap / 2.0, gap / 20.0, gap / 200.0);
    }
  }
  if (tune_fx_ab > 0.0 && tex_coord.x > 1.0 - 8.0 * texel.x && tex_coord.y < 8.0 * texel.y) {
    c = fx_ab_off ? vec3(0.0) : vec3(1.0);
  }

  // the night's pale ground (marked in the night block), pressed down here, on the final values: in
  // the night block even -90% came back as -5% (the eyes' adaptation and the lights after it)
  if (pale_w > 0.0) {
    float lc = luma(c);
    float knee = tune_night_pale_knee;
    float target = lc > knee ? knee + (lc - knee) * tune_night_pale_slope : lc;
    c *= mix(1.0, max(target / max(lc, 1e-4), tune_night_pale_cap), pale_w);
    // (the press flattens what it presses: the port's boards at 20h one even lilac sheet with no
    // plank lines. The game picture's own grain (each pixel against its neighbours 2 px away) put back
    // on top, so the gaps and the grain read again)
    if (tune_night_pale_grain > 0.0) {
      vec2 o = 2.0 * texel;
      float m = 0.25 * (luma(texture(tex_T0, tex_coord + vec2(o.x, 0.0)).rgb) +
                        luma(texture(tex_T0, tex_coord - vec2(o.x, 0.0)).rgb) +
                        luma(texture(tex_T0, tex_coord + vec2(0.0, o.y)).rgb) +
                        luma(texture(tex_T0, tex_coord - vec2(0.0, o.y)).rgb));
      float gr = clamp(luma(texture(tex_T0, tex_coord).rgb) / max(m, 1e-3), 0.6, 1.4);
      c *= mix(1.0, gr, tune_night_pale_grain * pale_w);
    }
  }

  // a shoulder for every pixel: relit lamps, light spill, shafts, bloom and GI added up past 1 and
  // the clamp below cut them to a flat white-yellow with the hue gone. The brightest channel rolls
  // off from the knee (hue kept, below it nothing changes), and what was far past 1 goes toward
  // white, as a hot core does (tune_shoulder_knee >= 1: off)
  if (tune_shoulder_knee < 1.0) {
    float m = max(c.r, max(c.g, c.b));
    float k = tune_shoulder_knee;
    if (m > k) {
      float s = k + (1.0 - k) * (1.0 - exp(-(m - k) / (1.0 - k)));
      vec3 t = c * (s / m);
      c = mix(t, vec3(s), smoothstep(1.0, 1.0 + tune_shoulder_white, m) * 0.6);
    }
  }

  // vignette
  vec2 v = tex_coord - 0.5;
  c *= 1.0 - vignette * smoothstep(0.25, 0.85, dot(v, v) * 2.2);
  // (a NaN or infinity from any term above clamps to a lone white or black pixel, and the TAA keeps
  // it: the game's own color there instead)
  if (any(isnan(c)) || any(isinf(c))) {
    c = texture(tex_T0, tex_coord).rgb;
  }

  // (test: lights.albedo's target: 1 its color, 2 the baked light (a), the right half only)
  if (tune_albedo_dbg > 0.5 && tex_coord.x > 0.5) {
    vec4 ad = texture(tex_T27, tex_coord);
    c = tune_albedo_dbg < 1.5 ? ad.rgb : vec3(ad.a);
  }
  color = vec4(clamp(c, 0.0, 1.0) * color_mult.rgb * color_mult.a, 1.0) + color_add;
  // dither to the 8-bit target: a triangular +-1 step of interleaved gradient noise, turned every
  // frame (the TAA then averages it into finer steps): no bands in the sky and the dark shade
  if (tune_dither > 0.0) {
    vec2 fp = gl_FragCoord.xy + 5.588238 * mod(floor(haze.w * 60.0), 64.0);
    float n1 = fract(52.9829189 * fract(dot(fp, vec2(0.06711056, 0.00583715))));
    float n2 = fract(52.9829189 * fract(dot(fp + vec2(47.0, 17.0), vec2(0.06711056, 0.00583715))));
    color.rgb += (n1 + n2 - 1.0) * tune_dither / 255.0;
  }
}
