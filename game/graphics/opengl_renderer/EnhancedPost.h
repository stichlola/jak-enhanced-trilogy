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
 * Jak 3 Enhanced post-processing, run on the finished game frame before it goes to the window:
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
    float motion_blur = 0.5f;      // camera motion blur, as a fraction of the frame's motion
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
  // the in-game menu (\ key): the main switches, time of day, quick travel
  void draw_menu();

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

  // volumetric mist, half resolution
  void do_volume(SharedRenderState* render_state, GLuint screen_vao, ScopedProfilerNode& prof);
  GLuint m_vol_fbo = 0;
  GLuint m_vol_tex = 0;
  int m_vol_w = 0;
  int m_vol_h = 0;
  bool m_vol_ready = false;

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
