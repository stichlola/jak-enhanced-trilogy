#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * The game's projection, saved by the background renderers each frame (make_new_cam_mat), so post
 * effects can rebuild view-space positions from the depth buffer:
 * ndc_z = depth_a + depth_b / view_z, ndc_x = fx * view_x / view_z, ndc_y = fy * view_y / view_z
 * (fy before the shaders' SCISSOR_ADJUST * HEIGHT_SCALE).
 */
struct EnhancedCamera {
  float depth_a = 0.f;
  float depth_b = 0.f;
  float fx = 1.f;
  float fy = 1.f;
  float rot[3][3] = {};  // world -> view: view[col] = sum over row of world[row] * rot[row][col]
  float trans[3] = {};   // camera position, world
  bool valid = false;
};
extern EnhancedCamera g_enhanced_camera;

// Enhanced: the game's framebuffer has a second color target for the level geometry's world
// normals (computed at load, see compute_vertex_normals). fbo = that framebuffer (0 = none).
struct EnhancedNormals {
  GLuint fbo = 0;
  bool enabled = true;
};
extern EnhancedNormals g_enhanced_normals;

// While alive, the background renderers' draws into the game's framebuffer also write their
// normals (the draw mode decides per draw: opaque, depth-writing draws only).
class EnhancedNormalTarget {
 public:
  EnhancedNormalTarget();
  ~EnhancedNormalTarget();

 private:
  bool m_on = false;
};

// Enhanced character shading (merc2): rim light and highlights from the key light.
struct EnhancedMerc {
  bool enabled = true;
  float rim = 0.45f;
  float spec = 0.35f;
  float wet = 0.f;  // rain on the characters: darker, glossier (set by the post pass's weather)
};
extern EnhancedMerc g_enhanced_merc;

// Enhanced wind on grass and leaves (shrub.vert).
struct EnhancedWind {
  bool enabled = true;
  float strength = 1.0f;
  bool push = true;          // grass bends away from Jak
  float push_radius = 1.3f;  // meters
};
extern EnhancedWind g_enhanced_wind;

// Enhanced: the characters (merc) drawn this frame, kept for the sun shadow map. Merc2 copies its
// bones (camera space) and opaque draws here as it flushes; the shadow pass replays them.
struct EnhancedCasters {
  struct Draw {
    GLuint vertices = 0;
    GLuint indices = 0;
    u32 first_index = 0;
    u32 index_count = 0;
    u32 first_bone = 0;  // in bone vectors (vec4)
    GLuint tex = 0;      // 0: no alpha test
    bool strip = true;
  };
  bool capture = false;  // set at the start of a frame when the shadow map wants them
  u32 alignment = 1;     // bone vectors per uniform buffer offset alignment
  std::vector<float> bones;  // 4 floats per bone vector
  std::vector<Draw> draws;
  // copies of the vertex buffers the game modifies per model (blerc faces, mod vertices): Merc2
  // reuses its own ones within the frame, before the shadow pass replays the draws
  struct ModCopy {
    GLuint buffer = 0;
    GLint size = 0;
  };
  std::vector<ModCopy> mod_copies;
  size_t next_mod_copy = 0;
};
extern EnhancedCasters g_enhanced_casters;

/*!
 * Jak Enhanced Trilogy post-processing, run on the finished game frame before it goes to the window:
 * screen-space ambient occlusion from the depth buffer, a light depth of field on the far
 * distance, god rays toward the sun, a soft bloom (bright pass + dual-filter blur chain), color grading, vignette and a light
 * contrast-adaptive sharpen. Set JAK_ENHANCED=0 in the environment to start with it off (vanilla
 * look); the settings can be changed live in the "Renderer Debug" window.
 */
class EnhancedPost {
 public:
  struct Settings {
    bool enabled = true;
    bool ssao = true;
    float ssao_radius = 1.4f;      // meters
    float ssao_intensity = 1.4f;
    float ssao_max_distance = 250.f;  // meters, faded out beyond
    bool sun_shadows = true;          // screen-space shadows toward the sun, in the same pass
    bool shadow_map = true;           // real sun shadows: the levels drawn from the sun
    bool shadow_casters = true;       // characters and wind-blown trees in it too
    float shadow_strength = 0.35f;    // how much light a shadow takes off sunlit surfaces
    float shadow_range = 80.f;        // meters: half the side of the square around the camera
    float sun_shadow_strength = 0.45f;
    float sun_shadow_length = 3.0f;   // meters marched toward the sun
    bool ssao_debug = false;       // show the occlusion alone
    bool dof = true;
    float dof_start = 70.f;  // meters: sharp until here
    float dof_end = 350.f;   // meters: full blur from here
    float dof_radius = 2.5f;  // pixels at 1080p, scaled with the resolution
    bool shafts = true;  // god rays: the bright sky smeared toward the sun on screen
    float shafts_strength = 0.6f;
    float shafts_threshold = 0.4f;
    bool fxaa = true;
    bool taa = true;          // temporal anti-aliasing (FXAA is skipped while it runs)
    float taa_history = 0.9f;  // history weight when still (fast motion: taa_history_fast)
    float taa_history_fast = 0.75f;
    float fog_density = 0.0012f;   // per meter
    float fog_height = 0.02f;      // per meter above the camera
    float fog_amount = 0.35f;      // at most this much of the fog color
    float haze_strength = 0.0009f;  // heat shimmer over the far ground (uv)
    bool volume = true;            // volumetric mist lit by the sun, with light shafts
    float mist_density = 0.006f;   // per meter, at the ground
    float mist_height = 4.0f;      // meters: density falls by e every this much higher
    float mist_distance = 120.f;   // meters marched
    float mist_ground = 1.5f;      // meters: where the ground is assumed below the camera
    float mist_noise = 0.7f;       // drifting patches
    float mist_sun = 1.5f;         // sunlight in the mist
    float mist_aniso = 0.5f;       // forward scattering (brighter looking toward the sun)
    float motion_blur = 0.3f;      // camera motion blur, as a fraction of the frame's motion
    bool water_reflections = true;
    float water_reflection_strength = 0.6f;
    float water_deep[3] = {0.035f, 0.17f, 0.2f};
    float water_shallow[3] = {0.16f, 0.42f, 0.4f};
    float water_unify = 1.0f;
    float water_swell = 1.0f;       // open-sea swell
    float water_refraction = 0.012f;  // shallow water bends what is under it
    float water_caustics = 0.8f;    // caustics on the shallow sea bed
    float water_glint = 1.0f;
    float relight = 1.0f;  // sun shading from depth normals
    bool lights = true;     // the glows (lamps, torches, shots) light their surroundings
    float light_shadow = 0.85f;  // dynamic lights' screen-space shadows (0 = none)
    float blast = 1.0f;     // explosions: flash and shockwave (0 = the game's alone)
    float wake = 1.0f;      // rings on the water where Jak swims or rides
    float prints = 1.0f;    // footprints and ruts in the sand
    float dark_nights = 1.0f;  // nights dark where no light reaches (0 = the game's own)
    bool cloud_shadows = true;  // the clouds shade the ground in drifting patches
    float lens_flare = 0.6f;    // the sun in the lens: ghosts and a halo (0 = off)
    // Jak's lantern: on foot in the desert at night a Wastelander's oil lantern Daxter holds up on
    // his shoulder (Damas's people cross the sands by night with them); off in vehicles
    bool desert_lamp = true;
    bool lantern_intro_done = false;  // the first desert night with the lantern has been shown
    float light_spill = 1.0f;   // at night what shines lights the surfaces around it
    float lit_smoke = 0.8f; // the dynamic lights brighten and tint the smoke and dust near them
    bool far_view = true;   // no occlusion culling from the game's precomputed visibility
    float relief = 1.0f;    // the textures' shading read as small bumps under the sun
    float materials = 1.0f; // metal and glossy surfaces (by texture name): reflections, highlights
    // reflections on water, metal and glass traced through the levels' geometry where the
    // screen-space ones miss (0 off, 1 on, 2 traced only, 3 the traced color alone on the water)
    int ray_traced = 0;  // off by default for now: still has artifacts
    // ambient occlusion traced through the levels' geometry (short rays from every pixel)
    bool rt_ao = false;  // off for now: too slow
    float rt_ao_length = 3.0f;     // meters
    float rt_ao_strength = 1.0f;
    float rt_ao_distance = 80.f;   // meters from the camera
    bool fire = true;       // fires: their light flickers, hot air wobbles over them, embers rise
    float fire_haze = 0.012f;    // how far the hot air bends what's behind it (flame sizes)
    float fire_embers = 1.0f;
    bool geometry_normals = true;  // the levels' own normals (else rebuilt from depth)
    bool normal_debug = false;     // show the normals the post pass uses
    float light_strength = 1.0f;
    float light_reach = 6.0f;  // light radius as a multiple of the glow's size
    float lamp_strength = 0.75f;  // the levels' lamps (light spheres)
    float lamp_reach = 1.0f;     // their radius as a multiple of the game's
    bool gi = true;         // one-bounce screen-space indirect light
    float gi_strength = 1.5f;
    float gi_radius = 3.0f;  // meters
    bool bloom = true;
    float bloom_threshold = 0.82f;  // luminance where the bright pass starts
    float bloom_knee = 0.12f;       // soft ramp above the threshold
    float bloom_strength = 0.22f;
    float bloom_radius = 1.0f;      // tent filter scale on the way up
    float saturation = 1.06f;
    float contrast = 0.10f;  // 0 = linear, 1 = full S-curve
    float warmth = 0.0f;     // shifts toward warm highlights / cool shadows
    float vignette = 0.18f;
    float sharpen = 0.25f;
    bool area_grading = true;      // a color grade per area (Haven, desert, forest...), day and night
    bool soft_shadows = true;      // sun shadows sharp at the contact, softer further from the caster
    float shadow_softness = 0.02f;  // penumbra per meter between caster and receiver
    bool clouds = true;            // a drifting cloud layer in the sky, lit by the sun
    float cloud_amount = 1.0f;
    int weather = 1;               // a preset (kWeatherPresets in EnhancedPost.cpp): 1 changing (by
                                   // area, comes and goes), the last one custom (weather_mix)
    // the custom weather, mixed by hand: rain, thunderstorm, fog, ground mist, snow, dust, clouds,
    // the sun out after the rain, wet ground (< 0: as wet as it rains)
    std::array<float, 9> weather_mix = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.3f, 0.f, -1.f};
    float vivid = 0.f;             // vivid colors: saturated greens and blues, warm sun, deep sky
    float rain_strength = 1.0f;
    float dust_strength = 1.0f;
    bool cine_dof = true;          // depth of field in cutscenes, focused on what's in the middle
    float cine_aperture = 1.0f;
    float blast_sparks = 1.0f;     // explosions throw sparks and leave hot air (0 = off)
  };

  EnhancedPost();
  ~EnhancedPost();
  EnhancedPost(const EnhancedPost&) = delete;
  EnhancedPost& operator=(const EnhancedPost&) = delete;

  /*!
   * Build the occlusion (from scene_fbo's depth) and bloom (from scene_tex) textures for a w x h
   * frame. Call before activate_composite().
   */
  void prepare(GLuint scene_tex,
               GLuint scene_fbo,
               int w,
               int h,
               GLuint screen_vao,
               SharedRenderState* render_state,
               ScopedProfilerNode& prof);

  /*!
   * Activate the composite shader and bind its textures, in place of the plain post_processing
   * shader. The caller sets color_mult / color_add and draws the screen quad.
   */
  Shader& activate_composite(SharedRenderState* render_state, int w, int h);

  bool active() const { return m_settings.enabled; }

  /*!
   * At the start of a frame: the sub-pixel jitter of the 3D world's shaders for this frame (a
   * Halton 2,3 sequence of 8), zero when the temporal anti-aliasing is off.
   */
  void begin_frame(ShaderLibrary& shaders, int w, int h);

  /*!
   * Run every effect on the 3D scene in render_fbo (w x h) and draw the result back into it.
   * Called between the 3D buckets and the HUD, so the HUD stays untouched.
   */
  /*!
   * Around the near ocean bucket: save the depth buffer and let the ocean write its surface,
   * then keep that surface for reflections and put the depth buffer back as it was.
   */
  void begin_ocean(GLuint render_fbo, int w, int h, SharedRenderState* render_state);
  // right after the mid/far ocean (drawn over the sky, with depth): where the depth is still the
  // same at the end of the frame, the far sea shows
  void capture_far_ocean(GLuint render_fbo, int w, int h);
  void end_ocean(GLuint render_fbo, int w, int h, SharedRenderState* render_state);

  void apply_to_scene(GLuint render_fbo,
                      int w,
                      int h,
                      GLuint screen_vao,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof);
  void draw_debug_window();
  // whether the background draws everything in view (not only what the game's coarse
  // precomputed visibility lists for the camera's spot)
  bool far_view() const { return m_settings.enabled && m_settings.far_view; }
  // the in-game menu (\ key): the main switches, time of day, quick travel
  void draw_menu();
  void draw_nap();

 private:
  static constexpr int kLevels = 6;
  struct Level {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0;
    int h = 0;
  };

  void resize(int w, int h);
  void free_levels();
  void free_ssao();
  void do_ssao(GLuint src_fbo,
               int w,
               int h,
               GLuint screen_vao,
               SharedRenderState* render_state,
               ScopedProfilerNode& prof);

  Settings m_settings;
  // the menu's choices, kept in enhanced-settings.json in the game's settings folder
  int m_population = 2;  // 0..3, see enhanced-population-count in the game
  std::string m_saved_settings;
  std::string menu_settings_json() const;
  void apply_menu_settings(const std::string& text);
  void apply_preset(int preset);
  void save_menu_settings();
  std::array<Level, kLevels> m_levels;
  int m_w = 0;
  int m_h = 0;
  GLuint m_black_tex = 0;
  GLuint m_no_shadow_tex = 0;
  GLuint m_white_tex = 0;

  // full-res depth copy and occlusion (raw + blurred)
  GLuint m_depth_fbo = 0;
  GLuint m_depth_tex = 0;
  GLuint m_normal_fbo = 0;  // the level normals, resolved (no msaa)
  GLuint m_normal_tex = 0;
  bool m_normals_ready = false;
  GLuint m_ao_fbo[2] = {0, 0};
  GLuint m_ao_tex[2] = {0, 0};
  int m_ssao_w = 0;
  int m_ssao_h = 0;
  bool m_ssao_ready = false;
  std::array<float, 4> m_last_proj = {0, 0, 1, 1};

  // the near ocean's surface depth, and the depth buffer from before it was drawn
  GLuint m_ocean_fbo[3] = {0, 0, 0};  // 0: before the near ocean, 1: with it, 2: far ocean
  GLuint m_ocean_depth[3] = {0, 0, 0};
  void ensure_ocean_buffers(int w, int h);
  bool m_far_ocean_ready = false;
  int m_ocean_w = 0;
  int m_ocean_h = 0;
  bool m_ocean_ready = false;

  // copy of the scene being composited
  GLuint m_scene_fbo = 0;
  GLuint m_scene_tex = 0;
  int m_scene_w = 0;
  int m_scene_h = 0;

  // the camera of the frame before, for motion blur
  EnhancedCamera m_prev_camera;
  float m_prev_fy = 1.f;
  double m_time = 0;
  // warm glows seen at the same spot (world, game units): the ones that stay are fires, the shots
  // and muzzle flashes move on or die out
  struct FireMemory {
    float pos[3];
    float size;
    double first, last;
    int hits;
  };
  std::vector<FireMemory> m_fires;
  // explosions of the last moment (world, game units): a flash of light and a shockwave
  struct Blast {
    float pos[3];
    float radius;
    double start;
    float dir[3] = {0.f, 0.f, 0.f};  // gun blasts: where it's fired (world)
    int kind = 0;  // 0: an explosion, 1: the red gun's blast, 2: the red gun charging, 3: charged
  };
  std::vector<Blast> m_blasts;
  // where Jak was lately (world, game units): rings spread on the water from these
  struct WakePoint {
    float pos[3];
    double time;
    bool ride = false;  // behind a vehicle or the board: foam churned along it
  };
  std::vector<WakePoint> m_wake;
  // Jak's footprints (a print every stride, left and right in turn) and the ruts of what he drives
  struct Print {
    float pos[3];
    float heading;  // radians, atan2(x, z) of the way he went
    int kind;       // 0 left foot, 1 right foot, 2 ruts
    double time;
    float seg = 0.f;  // ruts: how far back the piece reaches (to the one before), game units
  };
  std::vector<Print> m_prints;
  float m_last_print[3] = {};
  bool m_last_print_valid = false;
  bool m_print_left = true;
  // Jak's steps and landings on any ground, for the splashes in the puddles (kept a second)
  struct Step {
    float pos[3];
    float strength;  // 1 a step, 2 a landing
    double time;
  };
  std::vector<Step> m_steps;
  std::vector<Step> m_bounces;  // raindrops bouncing off the ground around Jak (strength 0)
  int m_last_player_state = 0;
  // dust kicked up behind a vehicle or the board in the desert, spray where they cross water
  struct Puff {
    float pos[3];
    float size;  // meters
    int kind;    // 0 dust, 1 spray
    float seed;
    float life;  // seconds
    double time;
  };
  std::vector<Puff> m_puffs;
  float m_ride_last[3] = {};
  double m_ride_last_time = -1.0;
  float m_ride_speed = 0.f;  // m/s, smoothed
  float m_ride_dir[2] = {0.f, 1.f};
  double m_puff_time = 0.0;
  bool m_puff_left = false;
  int m_last_ride_state = 0;
  const FireMemory* note_fire_glow(const float* pos, float size);
  bool is_fire(const FireMemory& f) const;

  // god rays, half resolution
  void do_shafts(GLuint scene_tex, GLuint screen_vao, SharedRenderState* render_state,
                 ScopedProfilerNode& prof);
  GLuint m_shaft_fbo = 0;
  GLuint m_shaft_tex = 0;
  int m_shaft_w = 0;
  int m_shaft_h = 0;
  bool m_shafts_ready = false;

  // indirect light, half resolution (0: raw, 1: blur temp)
  void do_gi(GLuint scene_tex, GLuint screen_vao, SharedRenderState* render_state,
             ScopedProfilerNode& prof);
  GLuint m_gi_fbo[2] = {0, 0};
  GLuint m_gi_tex[2] = {0, 0};
  int m_gi_w = 0;
  int m_gi_h = 0;
  bool m_gi_ready = false;
  float m_sun_view[3] = {0, 0, 0};

  // sun shadow map: the static geometry of the loaded levels (tfrag, tie) drawn from the sun
  void do_shadow_map(SharedRenderState* render_state, ScopedProfilerNode& prof);
  struct ShadowDraw {
    GLuint vertices = 0;
    GLuint indices = 0;
    bool own_indices = false;  // tfrag: built here
    u32 count = 0;
    GLenum mode = GL_TRIANGLES;
    bool cut = false;
    bool own_vertices = false;  // wind ties: world positions built here
    bool wind = false;          // a caster (drawn with shadow_casters only)
    u32 first = 0;              // first index
    GLuint tex = 0;             // alpha tested with this texture (0 = solid)
  };
  std::unordered_map<u64, std::vector<ShadowDraw>> m_shadow_draws;  // by level load id
  void add_wind_shadow_draws(const LevelData* lev, bool cut, std::vector<ShadowDraw>* draws);
  void draw_shadow_casters(SharedRenderState* render_state, ScopedProfilerNode& prof);
  GLuint m_caster_bones = 0;
  size_t m_caster_bones_size = 0;
  GLuint m_caster_vao = 0;
  GLuint m_shadow_fbo = 0;
  GLuint m_shadow_tex = 0;
  GLuint m_shadow_vao = 0;
  int m_shadow_res = 0;
  bool m_shadow_ready = false;
  float m_light_mvp[16] = {};  // world (game units) -> shadow map clip space, row-major

  // ray-traced reflections: each level's hierarchy (built at load, see EnhancedRayTrace.h) in its
  // own buffers, copied into one set of buffers for the levels in use whenever that set changes
  void update_rt(SharedRenderState* render_state);
  struct RtLevelGpu {
    GLuint buf[3] = {0, 0, 0};  // nodes, triangles, palettes
    u32 size[3] = {0, 0, 0};    // bytes
    GLuint tex = 0;             // its textures (2D array, one mip level)
    int layers = 0;
  };
  std::unordered_map<u64, RtLevelGpu> m_rt_levels;  // by level load id
  std::vector<u64> m_rt_in_use;
  GLuint m_rt_buf[3] = {0, 0, 0};
  GLuint m_rt_tex = 0;  // all their textures (2D array with mipmaps)
  int m_rt_count = 0;                    // levels in the combined buffers
  std::array<s32, 16 * 4> m_rt_base{};   // per level: node, triangle, palette, texture layer base
  bool m_rt_ready = false;

  // volumetric mist, half resolution
  void do_volume(SharedRenderState* render_state, GLuint screen_vao, ScopedProfilerNode& prof);
  GLuint m_vol_fbo = 0;
  GLuint m_vol_tex = 0;
  int m_vol_w = 0;
  int m_vol_h = 0;
  bool m_vol_ready = false;

  // the area's look and weather, eased toward the current area's each frame (update_scene)
  struct Grade {
    float lift[3] = {0.f, 0.f, 0.f};  // added in the shadows
    float gain[3] = {1.f, 1.f, 1.f};  // multiplies the highlights
    float saturation = 1.f;           // times the setting
    float contrast = 0.f;             // plus the setting
    float warmth = 0.f;
    float clouds = 0.45f;             // sky cover, 0..1
  };
  void update_scene();
  Grade m_grade;
  float m_rain = 0.f;  // 0..1
  float m_dust = 0.f;
  float m_wet = 0.f;    // the ground: soaks fast in the rain, dries slowly after
  float m_puddle = 0.f; // the puddles' water: fills slower, outlasts the wet ground by minutes
  float m_storm = 0.f;  // thunderstorm: lightning, wind
  float m_mist = 0.f;   // low fog on the ground
  float m_clear = 0.f;  // the sun out after the rain: shafts, rainbow, steam
  float m_snow = 0.f;   // snowfall (0..1)
  std::string m_visit_zone;     // the zone (town) Jak was last in; m_visit_offset: where its weather cycle starts
  double m_visit_offset = 0.0;
  float m_fog = 0.f;    // how much of the distance fog and the volumetric mist (0..1)
  float m_heat = 0.f;   // hot air shimmering up from lava (0..1)
  float m_sunny = 0.f;  // a bright summer day: saturated, warm sun, deep blue sky
  float m_flash = 0.f;  // lightning flash, 0..1
  float m_bolt[4] = {};  // the bolt: azimuth (rad), seed, age (s), strength
  double m_next_bolt = 0;
  float m_sheet = 0.f;       // sheet lightning: the whole sky lit from inside the clouds
  float m_sheet_age = 10.f;
  float m_sheet_strength = 0.f;
  double m_next_sheet = 0;
  double m_thunder_at = -1;  // when the last bolt's thunder arrives (s)
  int m_thunder_kind = 0;
  float m_wind[2] = {0.f, 1.f};  // the dust's wind direction (world xz)
  float m_wind_speed = 8.f;      // m/s
  double m_drift[2] = {0, 0};    // how far the wind has carried the dust (m, wrapped)
  int m_weather_state = 0;       // what the changing weather is doing now (for the debug text)
  float m_cine = 0.f;  // cutscene depth of field, eased in and out
  float m_movie = 0.f;  // a cutscene plays, eased: its close-ups get the game's own shading
  int m_zone = 0;
  bool m_pond = false;  // the water here is a pond (Jak 3's desert oases)
  float m_night = 0.f;
  float night_darkness() const;
  // the vehicles' headlights (world): position, strength; direction. Kept until the game sends
  // the next frame's
  std::vector<std::array<float, 8>> m_headlights;
  float m_lamp_on = 0.f;  // Jak's lamp in the desert, fading in and out
  float m_lantern[4] = {};  // view space, strength (0: none)
  // the lantern bulb following Jak on a spring: its place next to him (m) and how fast it moves
  // there (m/s), Jak's last position and velocity, and where it is (world, game units)
  float m_bulb_off[3] = {};
  float m_bulb_vel[3] = {};
  float m_bulb_jak[3] = {};
  float m_bulb_jv[3] = {};
  float m_bulb_pos[3] = {};
  bool m_bulb_out = false;
  int m_lamp_mode = 0;           // 0 by itself (desert nights), 1 switched on, 2 switched off
  int m_lamp_toggles_seen = -1;  // the player's switch count last seen
  double m_bulb_time = 0.0;
  std::vector<std::array<float, 4>> m_fire_spawners;  // world xyz, kind (EnhancedFires)
  double m_fire_spawners_time = 0.0;
  double m_headlights_time = -1.0;
  bool m_city = false;  // a lit city (Haven, Spargus): its glow on the night sky
  // cutscene autofocus: a few depth samples around the middle of the screen, read back a frame
  // later (two pixel buffers in turn, no stall)
  void read_focus(int w, int h);
  GLuint m_focus_pbo[2] = {0, 0};
  int m_focus_cur = 0;
  bool m_focus_pending[2] = {false, false};
  float m_focus = 0.f;  // view z (game units), 0 = none yet
  // the shadow map read as plain depths (soft shadows' blocker search), not compared
  GLuint m_shadow_raw_sampler = 0;

  // temporal anti-aliasing: two history buffers, written in turn
  GLuint m_taa_fbo[2] = {0, 0};
  GLuint m_taa_tex[2] = {0, 0};
  int m_taa_w = 0;
  int m_taa_h = 0;
  int m_taa_cur = 0;
  bool m_taa_history = false;
  bool m_jittered = false;
  u32 m_frame = 0;
};
