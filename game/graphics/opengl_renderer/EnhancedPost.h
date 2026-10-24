#pragma once

#include <array>

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
    float fog_density = 0.0012f;   // per meter
    float fog_height = 0.02f;      // per meter above the camera
    float fog_amount = 0.35f;      // at most this much of the fog color
    float haze_strength = 0.0009f;  // heat shimmer over the far ground (uv)
    float motion_blur = 0.5f;      // camera motion blur, as a fraction of the frame's motion
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
   * Run every effect on the 3D scene in render_fbo (w x h) and draw the result back into it.
   * Called between the 3D buckets and the HUD, so the HUD stays untouched.
   */
  void apply_to_scene(GLuint render_fbo,
                      int w,
                      int h,
                      GLuint screen_vao,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof);
  void draw_debug_window();

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
  GLuint m_white_tex = 0;

  // full-res depth copy and occlusion (raw + blurred)
  GLuint m_depth_fbo = 0;
  GLuint m_depth_tex = 0;
  GLuint m_ao_fbo[2] = {0, 0};
  GLuint m_ao_tex[2] = {0, 0};
  int m_ssao_w = 0;
  int m_ssao_h = 0;
  bool m_ssao_ready = false;
  std::array<float, 4> m_last_proj = {0, 0, 1, 1};

  // copy of the scene being composited
  GLuint m_scene_fbo = 0;
  GLuint m_scene_tex = 0;
  int m_scene_w = 0;
  int m_scene_h = 0;

  // the camera of the frame before, for motion blur
  EnhancedCamera m_prev_camera;
  float m_prev_fy = 1.f;
  double m_time = 0;

  // god rays, half resolution
  void do_shafts(GLuint scene_tex, GLuint screen_vao, SharedRenderState* render_state,
                 ScopedProfilerNode& prof);
  GLuint m_shaft_fbo = 0;
  GLuint m_shaft_tex = 0;
  int m_shaft_w = 0;
  int m_shaft_h = 0;
  bool m_shafts_ready = false;
  float m_sun_view[3] = {0, 0, 0};
};
