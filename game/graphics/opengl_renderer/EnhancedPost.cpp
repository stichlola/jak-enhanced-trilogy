#include "EnhancedPost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/loader/EnhancedVegetation.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "game/graphics/pipelines/opengl.h"
#include "game/runtime.h"

#include "third-party/imgui/imgui.h"

EnhancedCamera g_enhanced_camera;
EnhancedSun g_enhanced_sun;
EnhancedPlayer g_enhanced_player;
EnhancedPushers g_enhanced_pushers;
EnhancedSmokeLights g_enhanced_smoke_lights;
EnhancedMerc g_enhanced_merc;
EnhancedWind g_enhanced_wind;
EnhancedCasters g_enhanced_casters;
EnhancedLights g_enhanced_lights;
EnhancedGameLights g_enhanced_game_lights;
EnhancedExplosions g_enhanced_explosions;
EnhancedMenu g_enhanced_menu;
EnhancedNormals g_enhanced_normals;

EnhancedNormalTarget::EnhancedNormalTarget() {
  if (!g_enhanced_normals.fbo || !g_enhanced_normals.enabled) {
    return;
  }
  GLint bound = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
  if ((GLuint)bound != g_enhanced_normals.fbo) {
    return;  // drawing somewhere else (texture animation, offscreen)
  }
  const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(2, both);
  m_on = true;
}

EnhancedNormalTarget::~EnhancedNormalTarget() {
  if (m_on) {
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }
}

namespace {
GLuint make_1x1(u32 rgba) {
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  return tex;
}

// the game's vertex shaders scale y by SCISSOR_ADJUST * HEIGHT_SCALE (jak 2/3 values, Shader.cpp)
constexpr float kScissorHeightScale = (512.f / 416.f) * 0.5f;
// GOAL distance units per meter
constexpr float kMeter = 4096.f;

// the key light in world space; test switch: JAK_ENHANCED_SUN_YAW=<degrees> turns it around the
// world's up axis
void sun_world(float out[3]) {
  static const float yaw = [] {
    const char* env = std::getenv("JAK_ENHANCED_SUN_YAW");
    return env ? (float)std::atof(env) * 3.14159265f / 180.f : 0.f;
  }();
  const float* d = g_enhanced_sun.dir;
  out[0] = std::cos(yaw) * d[0] + std::sin(yaw) * d[2];
  out[1] = d[1];
  out[2] = -std::sin(yaw) * d[0] + std::cos(yaw) * d[2];
}
}  // namespace

EnhancedPost::EnhancedPost() {
  // the menu's choices from the last session first, the test switches below win over them
  try {
    auto path = file_util::get_user_settings_dir(g_game_version) / "enhanced-settings.json";
    if (file_util::file_exists(path.string())) {
      m_saved_settings = file_util::read_text_file(path);
      apply_menu_settings(m_saved_settings);
      lg::info("[enhanced] settings loaded from {}", path.string());
    }
  } catch (std::exception& e) {
    lg::error("[enhanced] could not load the saved settings: {}", e.what());
  }
  if (const char* pop = std::getenv("JAK_ENHANCED_POPULATION")) {
    // the game reads the same variable at boot
    m_population = std::clamp(std::atoi(pop), 0, 3);
  } else if (m_population != 2) {
    g_enhanced_menu.ask(6, std::to_string(m_population));
  }
  const char* env = std::getenv("JAK_ENHANCED");
  if (env && std::strcmp(env, "0") == 0) {
    m_settings.enabled = false;
  }
  // test switch: JAK_ENHANCED_RIM=<strength> overrides the character rim light
  if (const char* rim = std::getenv("JAK_ENHANCED_RIM")) {
    g_enhanced_merc.rim = (float)std::atof(rim);
  }
  if (const char* relight = std::getenv("JAK_ENHANCED_RELIGHT")) {
    m_settings.relight = (float)std::atof(relight);
  }
  if (const char* gi = std::getenv("JAK_ENHANCED_GI")) {
    m_settings.gi_strength = (float)std::atof(gi);
  }
  if (const char* wind = std::getenv("JAK_ENHANCED_WIND")) {
    g_enhanced_wind.strength = (float)std::atof(wind);
  }
  if (const char* vol = std::getenv("JAK_ENHANCED_MIST")) {
    // 0 = off, otherwise the density at the ground (/m)
    m_settings.volume = std::atof(vol) > 0.0;
    if (m_settings.volume) {
      m_settings.mist_density = (float)std::atof(vol);
    }
  }
  if (const char* push = std::getenv("JAK_ENHANCED_GRASS_PUSH")) {
    // the radius (m) the grass bends away from Jak in, 0 = off
    g_enhanced_wind.push_radius = (float)std::atof(push);
    g_enhanced_wind.push = g_enhanced_wind.push_radius > 0.f;
  }
  if (const char* normals = std::getenv("JAK_ENHANCED_NORMALS")) {
    // 0 = normals rebuilt from depth only, 2 = show them
    m_settings.geometry_normals = std::atoi(normals) != 0;
    m_settings.normal_debug = std::atoi(normals) == 2;
  }
  if (const char* menu = std::getenv("JAK_ENHANCED_MENU")) {
    // test runs: the in-game menu open from the start; "<kind>:<value>" also queues one request
    // (e.g. 2:ctysluma-alley, 1:23)
    g_enhanced_menu.open = true;
    const char* colon = std::strchr(menu, ':');
    if (colon) {
      g_enhanced_menu.ask(std::atoi(menu), colon + 1);
    }
  }
  if (const char* ls = std::getenv("JAK_ENHANCED_LIGHT_SHADOWS")) {
    // how dark the dynamic lights' shadows are, 0 = none
    m_settings.light_shadow = (float)std::atof(ls);
  }
  if (const char* far_view = std::getenv("JAK_ENHANCED_FAR_VIEW")) {
    // 0 = the game's occlusion culling (precomputed visibility) as it was
    m_settings.far_view = std::atoi(far_view) != 0;
  }
  if (const char* smoke = std::getenv("JAK_ENHANCED_LIT_SMOKE")) {
    // how much the dynamic lights brighten and tint the smoke and dust near them, 0 = off
    m_settings.lit_smoke = (float)std::atof(smoke);
  }
  if (const char* prints = std::getenv("JAK_ENHANCED_PRINTS")) {
    // footprints and ruts in the sand, 0 = off
    m_settings.prints = (float)std::atof(prints);
  }
  if (const char* wake = std::getenv("JAK_ENHANCED_WAKE")) {
    // rings on the water where Jak swims or rides, 0 = off
    m_settings.wake = (float)std::atof(wake);
  }
  if (const char* blast = std::getenv("JAK_ENHANCED_BLAST")) {
    // explosions' flash and shockwave strength, 0 = off
    m_settings.blast = (float)std::atof(blast);
  }
  if (const char* fire = std::getenv("JAK_ENHANCED_FIRE")) {
    // 0 = the game's fires as they are; else a scale for the hot air and the embers
    float k = (float)std::atof(fire);
    m_settings.fire = k > 0.f;
    m_settings.fire_haze *= k;
    m_settings.fire_embers *= k;
  }
  if (const char* ws = std::getenv("JAK_ENHANCED_WATER_SIM")) {
    // 0 = the water without swell, refraction and caustics
    if (std::atof(ws) <= 0.0) {
      m_settings.water_swell = m_settings.water_refraction = m_settings.water_caustics = 0.f;
    }
  }
  if (const char* lights = std::getenv("JAK_ENHANCED_LIGHTS")) {
    // 0 = off, otherwise the strength
    m_settings.lights = std::atof(lights) > 0.0;
    if (m_settings.lights) {
      m_settings.light_strength = (float)std::atof(lights);
    }
  }
  if (const char* sm = std::getenv("JAK_ENHANCED_SHADOWS")) {
    // 0 = off (screen-space shadows only), otherwise the strength
    m_settings.shadow_map = std::atof(sm) > 0.0;
    if (m_settings.shadow_map) {
      m_settings.shadow_strength = (float)std::atof(sm);
    }
  }
  if (const char* casters = std::getenv("JAK_ENHANCED_SHADOW_CASTERS")) {
    // 0 = only the static level geometry casts sun shadows
    m_settings.shadow_casters = std::atoi(casters) != 0;
  }
  if (const char* mist_sun = std::getenv("JAK_ENHANCED_MIST_SUN")) {
    m_settings.mist_sun = (float)std::atof(mist_sun);
  }
  if (const char* taa = std::getenv("JAK_ENHANCED_TAA")) {
    m_settings.taa = std::strcmp(taa, "0") != 0;
  }
  const char* ao_debug = std::getenv("JAK_ENHANCED_AO_DEBUG");
  if (ao_debug && std::strcmp(ao_debug, "1") == 0) {
    m_settings.ssao_debug = true;
  }
  // what's set now (saved choices plus test switches) counts as saved: the file is written only
  // when the player changes something in the menu, so a test run's switches don't stick
  m_saved_settings = menu_settings_json();

  // bound in place of the bloom / occlusion when they're off, so the composite always samples
  m_black_tex = make_1x1(0);
  m_white_tex = make_1x1(0xffffffff);
  // stands in for the sun shadow map until it's drawn: a shadow sampler must always see a depth
  // texture with compare mode on (depth 1 = nothing in the way)
  glGenTextures(1, &m_no_shadow_tex);
  glBindTexture(GL_TEXTURE_2D, m_no_shadow_tex);
  const float one = 1.f;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glBindTexture(GL_TEXTURE_2D, 0);
}

EnhancedPost::~EnhancedPost() {
  free_levels();
  free_ssao();
  if (m_shaft_fbo) {
    glDeleteFramebuffers(1, &m_shaft_fbo);
    glDeleteTextures(1, &m_shaft_tex);
  }
  if (m_scene_fbo) {
    glDeleteFramebuffers(1, &m_scene_fbo);
    glDeleteTextures(1, &m_scene_tex);
  }
  if (m_ocean_fbo[0]) {
    glDeleteFramebuffers(3, m_ocean_fbo);
    glDeleteTextures(3, m_ocean_depth);
  }
  if (m_gi_fbo[0]) {
    glDeleteFramebuffers(2, m_gi_fbo);
    glDeleteTextures(2, m_gi_tex);
  }
  if (m_taa_fbo[0]) {
    glDeleteFramebuffers(2, m_taa_fbo);
    glDeleteTextures(2, m_taa_tex);
  }
  if (m_vol_fbo) {
    glDeleteFramebuffers(1, &m_vol_fbo);
    glDeleteTextures(1, &m_vol_tex);
  }
  if (m_shadow_fbo) {
    glDeleteFramebuffers(1, &m_shadow_fbo);
    glDeleteTextures(1, &m_shadow_tex);
    glDeleteVertexArrays(1, &m_shadow_vao);
  }
  for (auto& [id, draws] : m_shadow_draws) {
    for (auto& d : draws) {
      if (d.own_indices) {
        glDeleteBuffers(1, &d.indices);
      }
    }
  }
  glDeleteTextures(1, &m_black_tex);
  glDeleteTextures(1, &m_no_shadow_tex);
  glDeleteTextures(1, &m_white_tex);
}

namespace {
float halton(u32 i, u32 base) {
  float f = 1.f;
  float r = 0.f;
  while (i > 0) {
    f /= (float)base;
    r += f * (float)(i % base);
    i /= base;
  }
  return r;
}
}  // namespace

void EnhancedPost::begin_frame(ShaderLibrary& shaders, int w, int h) {
  g_enhanced_lights.count = 0;  // the glows of this frame add themselves
  // the characters of this frame add themselves (Merc2), for the sun shadow map
  g_enhanced_casters.bones.clear();
  g_enhanced_casters.draws.clear();
  g_enhanced_casters.next_mod_copy = 0;
  g_enhanced_casters.capture =
      m_settings.enabled && m_settings.shadow_map && m_settings.shadow_casters;
  g_enhanced_normals.enabled = m_settings.enabled && m_settings.geometry_normals;
  if (g_enhanced_normals.enabled && g_enhanced_normals.fbo) {
    // a = 0: no normal (the post pass rebuilds one from depth there)
    GLint prev = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_enhanced_normals.fbo);
    const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, both);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    glClearBufferfv(GL_COLOR, 1, zero);
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  }
  const bool on = m_settings.enabled && m_settings.taa && w > 0 && h > 0;
  if (!on) {
    if (m_jittered) {
      shaders.set_jitter(0.f, 0.f);
      m_jittered = false;
    }
    m_taa_history = false;
    return;
  }
  m_frame++;
  const u32 i = (m_frame % 8) + 1;
  // pixels in [-0.5, 0.5] -> NDC offset
  shaders.set_jitter((halton(i, 2) - 0.5f) * 2.f / w, (halton(i, 3) - 0.5f) * 2.f / h);
  m_jittered = true;
}

void EnhancedPost::begin_ocean(GLuint render_fbo,
                               int w,
                               int h,
                               SharedRenderState* render_state) {
  m_ocean_ready = false;
  if (!m_settings.enabled || !m_settings.water_reflections) {
    return;
  }
  ensure_ocean_buffers(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ocean_fbo[0]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  render_state->enhanced_ocean_depth_write = true;
}

void EnhancedPost::capture_far_ocean(GLuint render_fbo, int w, int h) {
  m_far_ocean_ready = false;
  if (!m_settings.enabled || !m_settings.water_reflections) {
    return;
  }
  ensure_ocean_buffers(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ocean_fbo[2]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  m_far_ocean_ready = true;
}

void EnhancedPost::ensure_ocean_buffers(int w, int h) {
  if (w != m_ocean_w || h != m_ocean_h) {
    if (m_ocean_fbo[0]) {
      glDeleteFramebuffers(3, m_ocean_fbo);
      glDeleteTextures(3, m_ocean_depth);
    }
    m_ocean_w = w;
    m_ocean_h = h;
    glGenFramebuffers(3, m_ocean_fbo);
    glGenTextures(3, m_ocean_depth);
    for (int i = 0; i < 3; i++) {
      glBindTexture(GL_TEXTURE_2D, m_ocean_depth[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                   GL_UNSIGNED_INT_24_8, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ocean_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                             m_ocean_depth[i], 0);
      glDrawBuffer(GL_NONE);
      glReadBuffer(GL_NONE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
}

void EnhancedPost::end_ocean(GLuint render_fbo,
                             int w,
                             int h,
                             SharedRenderState* render_state) {
  if (!render_state->enhanced_ocean_depth_write) {
    return;
  }
  render_state->enhanced_ocean_depth_write = false;
  // keep the depth with the ocean surface, then put the original back
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ocean_fbo[1]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ocean_fbo[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, render_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  m_ocean_ready = true;
}

void EnhancedPost::apply_to_scene(GLuint render_fbo,
                                  int w,
                                  int h,
                                  GLuint screen_vao,
                                  SharedRenderState* render_state,
                                  ScopedProfilerNode& prof) {
  if (!m_settings.enabled) {
    g_enhanced_camera.valid = false;
    return;
  }
  if (w != m_scene_w || h != m_scene_h) {
    if (m_scene_fbo) {
      glDeleteFramebuffers(1, &m_scene_fbo);
      glDeleteTextures(1, &m_scene_tex);
    }
    m_scene_w = w;
    m_scene_h = h;
    glGenFramebuffers(1, &m_scene_fbo);
    glGenTextures(1, &m_scene_tex);
    glBindTexture(GL_TEXTURE_2D, m_scene_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scene_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // copy (resolving msaa) the scene so we can read it while drawing over it
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  const EnhancedCamera cam = g_enhanced_camera;  // prepare() clears the global
  prepare(m_scene_tex, render_fbo, w, h, screen_vao, render_state, prof);
  const bool have_scene = m_ssao_ready;

  // view -> previous view is R^T Rp, with the camera's move as an offset (motion blur, TAA)
  float to_prev[3][3];  // [k][c]: column k of the GLSL mat3
  float offset[3];
  bool reproject = false;
  if (have_scene && m_prev_camera.valid && cam.valid) {
    float moved = 0.f;
    for (int k = 0; k < 3; k++) {
      for (int c = 0; c < 3; c++) {
        to_prev[k][c] = 0.f;
        for (int r = 0; r < 3; r++) {
          to_prev[k][c] += cam.rot[r][k] * m_prev_camera.rot[r][c];
        }
      }
    }
    for (int c = 0; c < 3; c++) {
      offset[c] = 0.f;
      for (int r = 0; r < 3; r++) {
        offset[c] += (cam.trans[r] - m_prev_camera.trans[r]) * m_prev_camera.rot[r][c];
      }
      moved += (cam.trans[c] - m_prev_camera.trans[c]) * (cam.trans[c] - m_prev_camera.trans[c]);
    }
    // a cut or a teleport isn't motion
    reproject = std::sqrt(moved) < 8.f * kMeter;
  }

  // temporal anti-aliasing: this frame blended into the reprojected history
  GLuint scene_tex = m_scene_tex;
  const bool taa = m_settings.taa && m_jittered && have_scene;
  if (taa) {
    if (w != m_taa_w || h != m_taa_h) {
      if (m_taa_fbo[0]) {
        glDeleteFramebuffers(2, m_taa_fbo);
        glDeleteTextures(2, m_taa_tex);
      }
      m_taa_w = w;
      m_taa_h = h;
      glGenFramebuffers(2, m_taa_fbo);
      glGenTextures(2, m_taa_tex);
      for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, m_taa_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_taa_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_taa_tex[i],
                               0);
      }
      glBindTexture(GL_TEXTURE_2D, 0);
      m_taa_history = false;
    }
    const int cur = m_taa_cur;
    glBindFramebuffer(GL_FRAMEBUFFER, m_taa_fbo[cur]);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glBindVertexArray(screen_vao);
    auto& resolve = render_state->shaders[ShaderId::ENH_TAA];
    resolve.activate();
    auto rid = resolve.id();
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_taa_tex[1 - cur]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_scene_tex);
    glUniform1i(glGetUniformLocation(rid, "tex_T0"), 0);
    glUniform1i(glGetUniformLocation(rid, "tex_T1"), 1);
    glUniform1i(glGetUniformLocation(rid, "tex_T3"), 3);
    glUniform4f(glGetUniformLocation(rid, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
                m_last_proj[3]);
    glUniform2f(glGetUniformLocation(rid, "texel"), 1.f / w, 1.f / h);
    if (reproject) {
      glUniformMatrix3fv(glGetUniformLocation(rid, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
      glUniform3f(glGetUniformLocation(rid, "to_prev_offset"), offset[0], offset[1], offset[2]);
    }
    glUniform2f(glGetUniformLocation(rid, "prev_f"), m_prev_camera.fx, m_prev_fy);
    glUniform2f(glGetUniformLocation(rid, "keep"), m_settings.taa_history,
                m_settings.taa_history_fast);
    glUniform1i(glGetUniformLocation(rid, "has_history"), m_taa_history && reproject);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
    scene_tex = m_taa_tex[cur];
    m_taa_cur = 1 - cur;
    m_taa_history = true;
  } else {
    m_taa_history = false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glBindVertexArray(screen_vao);
  auto& shader = activate_composite(render_state, w, h);
  auto id = shader.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  // the game's brightness/contrast is applied later, on the whole frame
  glUniform4f(glGetUniformLocation(id, "color_mult"), 1.f, 1.f, 1.f, 1.f);
  glUniform4f(glGetUniformLocation(id, "color_add"), 0.f, 0.f, 0.f, 0.f);

  // world up in view space, for the fog and the heat haze
  glUniform3f(glGetUniformLocation(id, "up_view"), cam.rot[1][0], cam.rot[1][1], cam.rot[1][2]);
  glUniform4f(glGetUniformLocation(id, "fog"), m_settings.fog_density / kMeter,
              m_settings.fog_height / kMeter, have_scene ? m_settings.fog_amount : 0.f, 0.f);
  glUniform3f(glGetUniformLocation(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f);
  m_time += 1.0 / 60.0;
  glUniform4f(glGetUniformLocation(id, "haze"), have_scene ? m_settings.haze_strength : 0.f,
              120.f * kMeter, 400.f * kMeter, (float)m_time);
  glUniform1i(glGetUniformLocation(id, "fxaa"), m_settings.fxaa && !taa);

  // water reflections: the ocean surface depth from begin/end_ocean this frame
  const bool near_water = have_scene && m_ocean_ready && m_settings.water_reflections;
  const bool far_water = have_scene && m_far_ocean_ready && m_settings.water_reflections;
  const bool water = near_water || far_water;
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, near_water ? m_ocean_depth[1] : m_black_tex);
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D, far_water ? m_ocean_depth[2] : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T5"), 5);
  glUniform1i(glGetUniformLocation(id, "tex_T6"), 6);
  m_far_ocean_ready = false;
  glUniform1f(glGetUniformLocation(id, "water_strength"),
              water ? m_settings.water_reflection_strength : 0.f);
  // the camera's right and forward in view space, and its position, for the ripples
  glUniform3f(glGetUniformLocation(id, "right_view"), cam.rot[0][0], cam.rot[0][1], cam.rot[0][2]);
  glUniform3f(glGetUniformLocation(id, "fwd_view"), cam.rot[2][0], cam.rot[2][1], cam.rot[2][2]);
  glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform3fv(glGetUniformLocation(id, "water_deep_color"), 1, m_settings.water_deep);
  glUniform3fv(glGetUniformLocation(id, "water_shallow_color"), 1, m_settings.water_shallow);
  glUniform1f(glGetUniformLocation(id, "water_unify"), m_settings.water_unify);
  glUniform1f(glGetUniformLocation(id, "water_swell_strength"), m_settings.water_swell);
  glUniform1f(glGetUniformLocation(id, "water_refraction"), m_settings.water_refraction);
  glUniform1f(glGetUniformLocation(id, "water_caustics"), m_settings.water_caustics);
  glUniform1f(glGetUniformLocation(id, "relight"), m_settings.relight);
  {
    // dynamic lights: the glows in view space, the brightest and nearest first, at most kLights
    constexpr int kLights = 32;
    struct Candidate {
      float pos[4];  // view space, radius
      float color[4];
      float score;
    };
    std::vector<Candidate> candidates;
    struct Fire {
      float pos[4];  // view space, flame size
      float seed;
      float dist;
    };
    std::vector<Fire> fires;
    const bool on = m_settings.lights && have_scene && cam.valid;
    g_enhanced_smoke_lights.count = 0;
    for (int i = 0; on && i < g_enhanced_lights.count; i++) {
      const auto& l = g_enhanced_lights.lights[i];
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (l.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      float radius = std::clamp(l.size * m_settings.light_reach, 4.f * kMeter, 30.f * kMeter);
      c.pos[3] = radius;
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // behind the camera beyond its reach, or far away (the moon is a glow too)
      if (c.pos[2] < -radius || dist > 150.f * kMeter) {
        continue;
      }
      // the glow's hue at full value; its brightness (glows are faint, 0.1-0.3) scaled up
      float peak = std::max(l.color[0], std::max(l.color[1], l.color[2]));
      float bright = l.color[0] + l.color[1] + l.color[2];
      if (peak <= 0.f) {
        continue;
      }
      float intensity = std::min(1.5f, bright * 4.f) * m_settings.light_strength;
      // a fire's glow: orange to yellow, no blue, flame sized (shots are pure red or purple, the
      // far beacons and the sky's glows are bigger), and staying put
      const bool warm = m_settings.fire && l.color[2] < 0.25f * l.color[0] &&
                        l.color[1] > 0.1f * l.color[0] && l.color[1] < 0.9f * l.color[0] &&
                        l.size > 0.3f * kMeter && l.size < 6.f * kMeter && dist < 60.f * kMeter;
      if (const auto* f = warm ? note_fire_glow(l.pos, l.size) : nullptr) {
        // its light flickers on top of the game's own (a seed from where it stands)
        float seed = std::fmod(std::abs(f->pos[0] * 0.013f + f->pos[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        float n = 0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                  0.2f * std::sin(t * 37.f + 2.3f * seed);
        intensity *= 0.82f + 0.18f * n;
      }
      for (int k = 0; k < 3; k++) {
        c.color[k] = l.color[k] / peak * intensity;
      }
      c.color[3] = 0.f;
      c.score = bright * radius / (dist + radius);
      // a light is often drawn as two glows at the same spot: keep the one
      bool dup = false;
      for (auto& o : candidates) {
        float dx = o.pos[0] - c.pos[0], dy = o.pos[1] - c.pos[1], dz = o.pos[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz < 0.25f * kMeter * kMeter) {
          dup = true;
          if (c.score > o.score) {
            o = c;
          }
          break;
        }
      }
      if (!dup) {
        candidates.push_back(c);
      }
    }
    // explosions: a bright flash fading in half a second (sorted first: the score is huge)
    {
      std::lock_guard<std::mutex> lock(g_enhanced_explosions.mutex);
      for (int i = 0; i < g_enhanced_explosions.count; i++) {
        const float* e = g_enhanced_explosions.pending[i];
        if (m_blasts.size() < 16) {
          m_blasts.push_back({{e[0], e[1], e[2]}, std::max(e[3], 1.f * kMeter), m_time});
        }
      }
      g_enhanced_explosions.count = 0;
    }
    m_blasts.erase(std::remove_if(m_blasts.begin(), m_blasts.end(),
                                  [&](const Blast& b) { return m_time - b.start > 0.6; }),
                   m_blasts.end());
    for (const auto& b : m_blasts) {
      if (!on || m_settings.blast <= 0.f) {
        break;
      }
      float age = (float)(m_time - b.start);
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (b.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      c.pos[3] = std::clamp(b.radius * 4.f, 8.f * kMeter, 30.f * kMeter);
      float k = 4.f * std::exp(-age * 5.f) * m_settings.blast * m_settings.light_strength;
      c.color[0] = 1.f * k;
      c.color[1] = 0.62f * k;
      c.color[2] = 0.3f * k;
      c.color[3] = 0.f;
      c.score = 1e6f;
      candidates.push_back(c);
    }
    // the levels' own lamps (light spheres), sent by the game with their time-of-day palette
    int lamps = 0;
    if (on) {
      std::lock_guard<std::mutex> lock(g_enhanced_game_lights.mutex);
      lamps = g_enhanced_game_lights.count;
      for (int i = 0; i < lamps; i++) {
        const float* lp = g_enhanced_game_lights.pos[i];
        const float* lc = g_enhanced_game_lights.color[i];
        Candidate c;
        for (int col = 0; col < 3; col++) {
          c.pos[col] = 0.f;
          for (int row = 0; row < 3; row++) {
            c.pos[col] += (lp[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        float radius = std::clamp(lp[3] * m_settings.lamp_reach, 2.f * kMeter, 30.f * kMeter);
        c.pos[3] = radius;
        float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
        if (c.pos[2] < -radius || dist > 150.f * kMeter) {
          continue;
        }
        float bright = lc[0] + lc[1] + lc[2];
        for (int k = 0; k < 3; k++) {
          c.color[k] = lc[k] * m_settings.light_strength * m_settings.lamp_strength;
        }
        c.color[3] = 0.f;
        c.score = bright * radius / (dist + radius);
        candidates.push_back(c);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    const int n = std::min((int)candidates.size(), kLights);
    float pos[kLights * 4] = {};
    float color[kLights * 4] = {};
    for (int i = 0; i < n; i++) {
      std::memcpy(pos + i * 4, candidates[i].pos, sizeof(float) * 4);
      std::memcpy(color + i * 4, candidates[i].color, sizeof(float) * 4);
    }
    {
      // the strongest few back in world space for the smoke particles, drawn next frame
      auto& sl = g_enhanced_smoke_lights;
      sl.count = std::min(n, EnhancedSmokeLights::kMax);
      for (int i = 0; i < sl.count; i++) {
        const auto& c = candidates[i];
        for (int row = 0; row < 3; row++) {
          sl.pos[i][row] = cam.trans[row];
          for (int col = 0; col < 3; col++) {
            sl.pos[i][row] += cam.rot[row][col] * c.pos[col];
          }
        }
        sl.pos[i][3] = c.pos[3];
        std::memcpy(sl.color[i], c.color, sizeof(float) * 4);
      }
      sl.strength = m_settings.lit_smoke;
    }
    // the fires: forget the ones not seen for a second, the nearest few for the hot air and embers
    m_fires.erase(std::remove_if(m_fires.begin(), m_fires.end(),
                                 [&](const FireMemory& f) { return m_time - f.last > 1.0; }),
                  m_fires.end());
    for (const auto& f : m_fires) {
      if (!on || !is_fire(f)) {
        continue;
      }
      Fire v;
      for (int col = 0; col < 3; col++) {
        v.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          v.pos[col] += (f.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      v.pos[3] = f.size;
      v.dist = std::sqrt(v.pos[0] * v.pos[0] + v.pos[1] * v.pos[1] + v.pos[2] * v.pos[2]);
      v.seed = std::fmod(std::abs(f.pos[0] * 0.013f + f.pos[2] * 0.007f), 6.2832f);
      if (v.dist < 45.f * kMeter && v.pos[2] > -f.size) {
        fires.push_back(v);
      }
    }
    std::sort(fires.begin(), fires.end(), [](const Fire& a, const Fire& b) { return a.dist < b.dist; });
    constexpr int kFires = 4;
    float fire_pos[kFires * 4] = {};
    float fire_seed[kFires] = {};
    int fire_count = 0;
    for (const auto& f : fires) {
      if (fire_count == kFires) {
        break;
      }
      std::memcpy(fire_pos + fire_count * 4, f.pos, sizeof(float) * 4);
      fire_seed[fire_count++] = f.seed;
    }
    glUniform1f(glGetUniformLocation(id, "light_shadow"), m_settings.light_shadow);
    {
      // Jak's recent path: a point every 0.1 s while he moves (every 0.5 s standing), kept 2 s;
      // the shader starts a ring on the water from each one that lies at the water's height
      constexpr int kWake = 16;
      const auto& pl = g_enhanced_player;
      if (pl.valid && m_settings.wake > 0.f) {
        double since = m_wake.empty() ? 1e9 : m_time - m_wake.back().time;
        float moved = 1e9f;
        if (!m_wake.empty()) {
          const float* q = m_wake.back().pos;
          float dx = pl.pos[0] - q[0], dz = pl.pos[2] - q[2];
          moved = std::sqrt(dx * dx + dz * dz);
        }
        if ((since >= 0.1 && moved > 0.1f * kMeter) || since >= 0.5) {
          m_wake.push_back({{pl.pos[0], pl.pos[1], pl.pos[2]}, m_time});
        }
      }
      while (!m_wake.empty() && (m_time - m_wake.front().time > 2.0 || m_wake.size() > kWake)) {
        m_wake.erase(m_wake.begin());
      }
      float wake[kWake * 4] = {};
      int wake_count = 0;
      for (const auto& w : m_wake) {
        if (m_settings.wake <= 0.f) {
          break;
        }
        std::memcpy(wake + wake_count * 4, w.pos, sizeof(float) * 3);
        wake[wake_count * 4 + 3] = (float)(m_time - w.time);
        wake_count++;
      }
      glUniform1i(glGetUniformLocation(id, "wake_count"), wake_count);
      glUniform4fv(glGetUniformLocation(id, "wake"), kWake, wake);
      glUniform1f(glGetUniformLocation(id, "wake_strength"), m_settings.wake);
    }
    {
      // footprints where Jak walks on soft ground (the game knows the ground's material), ruts where
      // he drives; kept 30 s
      constexpr int kPrints = 48;
      const auto& pl = g_enhanced_player;
      static bool test_prints = std::getenv("JAK_ENHANCED_PRINTS_TEST") != nullptr;
      static double test_seeded = -1e9;
      if (test_prints && pl.valid && m_time - test_seeded > 10.0) {
        // test switch: footprints walking away from the camera beside Jak, ruts on his other side
        // (again every 10 s, wherever he is by then)
        test_seeded = m_time;
        m_prints.clear();
        float fx = pl.pos[0] - cam.trans[0], fz = pl.pos[2] - cam.trans[2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1.f);
        fx /= len;
        fz /= len;
        for (int i = 0; i < 24; i++) {
          bool rut = i >= 12;
          float along = (-2.f + (i % 12) * (rut ? 0.5f : 0.7f)) * kMeter;
          float side = (rut ? 2.5f : ((i % 2) ? -1.32f : -1.08f)) * kMeter;
          Print p;
          p.pos[0] = pl.pos[0] + fx * along + fz * side;
          p.pos[1] = pl.pos[1] + (rut ? 1.f * kMeter : 0.f);
          p.pos[2] = pl.pos[2] + fz * along - fx * side;
          p.heading = std::atan2(fx, fz);
          p.kind = rut ? 2 : (i % 2);
          p.time = m_time;
          m_prints.push_back(p);
        }
      }
      if (pl.valid && m_settings.prints > 0.f && pl.state > 0) {
        bool rut = pl.state == 2;
        float stride = (rut ? 0.5f : 0.7f) * kMeter;
        float dx = 0.f, dz = 0.f, moved = 1e9f;
        if (m_last_print_valid) {
          dx = pl.pos[0] - m_last_print[0];
          dz = pl.pos[2] - m_last_print[2];
          moved = std::sqrt(dx * dx + dz * dz);
        }
        if (moved > stride) {
          if (moved < 4.f * stride) {  // a step, not a jump or a teleport
            float fx = dx / moved, fz = dz / moved;
            float side = rut ? 0.f : (m_print_left ? 0.12f : -0.12f) * kMeter;
            Print p;
            p.pos[0] = pl.pos[0] + fz * side;
            p.pos[1] = pl.pos[1];
            p.pos[2] = pl.pos[2] - fx * side;
            p.heading = std::atan2(fx, fz);
            p.kind = rut ? 2 : (m_print_left ? 0 : 1);
            p.time = m_time;
            m_prints.push_back(p);
            m_print_left = !m_print_left;
          }
          std::memcpy(m_last_print, pl.pos, sizeof(m_last_print));
          m_last_print_valid = true;
        }
      }
      while (!m_prints.empty() &&
             (m_time - m_prints.front().time > 30.0 || m_prints.size() > (size_t)kPrints)) {
        m_prints.erase(m_prints.begin());
      }
      float prints[kPrints * 4] = {};
      float print_info[kPrints * 2] = {};
      int print_count = 0;
      for (const auto& p : m_prints) {
        if (m_settings.prints <= 0.f) {
          break;
        }
        std::memcpy(prints + print_count * 4, p.pos, sizeof(float) * 3);
        prints[print_count * 4 + 3] = p.heading;
        print_info[print_count * 2] = (float)(m_time - p.time);
        print_info[print_count * 2 + 1] = (float)p.kind;
        print_count++;
      }
      glUniform1i(glGetUniformLocation(id, "print_count"), print_count);
      glUniform4fv(glGetUniformLocation(id, "prints"), kPrints, prints);
      glUniform2fv(glGetUniformLocation(id, "print_info"), kPrints, print_info);
      glUniform1f(glGetUniformLocation(id, "prints_strength"), m_settings.prints);
    }
    // the nearest explosions in view for the shockwave: view space + radius, age
    {
      constexpr int kBlasts = 4;
      float blast_pos[kBlasts * 4] = {};
      float blast_age[kBlasts] = {};
      int blast_count = 0;
      for (const auto& b : m_blasts) {
        if (blast_count == kBlasts || !on || m_settings.blast <= 0.f) {
          break;
        }
        float v[3] = {0.f, 0.f, 0.f};
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            v[col] += (b.pos[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        if (v[2] < 1.f * kMeter) {
          continue;
        }
        std::memcpy(blast_pos + blast_count * 4, v, sizeof(v));
        blast_pos[blast_count * 4 + 3] = b.radius;
        blast_age[blast_count++] = (float)(m_time - b.start);
      }
      glUniform1i(glGetUniformLocation(id, "blast_count"), blast_count);
      glUniform4fv(glGetUniformLocation(id, "blast_pos"), kBlasts, blast_pos);
      glUniform1fv(glGetUniformLocation(id, "blast_age"), kBlasts, blast_age);
      glUniform1f(glGetUniformLocation(id, "blast_strength"), m_settings.blast);
    }
    glUniform1i(glGetUniformLocation(id, "fire_count"), fire_count);
    glUniform4fv(glGetUniformLocation(id, "fire_pos"), kFires, fire_pos);
    glUniform1fv(glGetUniformLocation(id, "fire_seed"), kFires, fire_seed);
    glUniform3f(glGetUniformLocation(id, "fire"), m_settings.fire_haze, m_settings.fire_embers,
                (float)std::fmod(m_time, 1000.0));
    glUniform1i(glGetUniformLocation(id, "light_count"), n);
    glUniform4fv(glGetUniformLocation(id, "light_pos"), kLights, pos);
    glUniform4fv(glGetUniformLocation(id, "light_color"), kLights, color);
    static int debug_counter = 0;
    if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
      lg::info("[enhanced] lights: {} glows, {} lamps, {} used", g_enhanced_lights.count, lamps, n);
      for (int i = 0; i < std::min(n, 6); i++) {
        const auto& c = candidates[i];
        lg::info("[enhanced]   light at {:.1f} {:.1f} {:.1f} m (view), radius {:.1f} m, color {:.2f} {:.2f} {:.2f}",
                 c.pos[0] / kMeter, c.pos[1] / kMeter, c.pos[2] / kMeter, c.pos[3] / kMeter,
                 c.color[0], c.color[1], c.color[2]);
      }
    }
  }
  {
    // the sun for the glints, only when it's above the horizon
    float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                          m_sun_view[2] * m_sun_view[2]);
    float up = len > 0.f ? (m_sun_view[0] * cam.rot[1][0] + m_sun_view[1] * cam.rot[1][1] +
                            m_sun_view[2] * cam.rot[1][2]) / len
                         : 0.f;
    const bool sun = g_enhanced_sun.valid && len > 0.f && up > 0.f;
    glUniform4f(glGetUniformLocation(id, "sun_dir"), sun ? m_sun_view[0] / len : 0.f,
                sun ? m_sun_view[1] / len : 0.f, sun ? m_sun_view[2] / len : 0.f,
                sun ? m_settings.water_glint * std::min(1.f, up * 4.f) : 0.f);
  }
  m_ocean_ready = false;

  // camera motion blur: view -> previous view is R^T Rp, with the camera's move as an offset
  float strength = 0.f;
  if (reproject && m_settings.motion_blur > 0.f) {
    strength = m_settings.motion_blur;
    glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
    glUniform3f(glGetUniformLocation(id, "to_prev_offset"), offset[0], offset[1], offset[2]);
  }
  glUniform4f(glGetUniformLocation(id, "motion"), m_prev_camera.fx, m_prev_fy, strength, 0.02f);
  glUniform2f(glGetUniformLocation(id, "motion_keep"), 10.f * kMeter, 25.f * kMeter);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();

  m_prev_camera = cam;
  m_prev_fy = cam.fy * kScissorHeightScale;

  // leave the state the way the bucket renderers expect it
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
}

void EnhancedPost::do_shafts(GLuint scene_tex,
                             GLuint screen_vao,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  // the sun on screen: in view space the camera looks down +z (see enh_ssao.frag)
  float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                        m_sun_view[2] * m_sun_view[2]);
  if (m_settings.ssao_debug) {
    static int behind_counter = 0;
    if ((behind_counter++ % 300) == 0) {
      lg::info("[enhanced] sun view {} {} {}", m_sun_view[0], m_sun_view[1], m_sun_view[2]);
    }
  }
  if (len < 1e-6f || m_sun_view[2] / len < 0.05f) {
    return;  // behind the camera
  }
  float sx = 0.5f * m_last_proj[2] * m_sun_view[0] / m_sun_view[2] + 0.5f;
  float sy = 0.5f * m_last_proj[3] * m_sun_view[1] / m_sun_view[2] + 0.5f;
  // fade out as the sun leaves the screen
  float off = std::max(std::max(-sx, sx - 1.f), std::max(-sy, sy - 1.f));
  float fade = 1.f - std::clamp(off / 0.6f, 0.f, 1.f);
  static int debug_counter = 0;
  if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
    lg::info("[enhanced] sun on screen {} {} fade {}", sx, sy, fade);
  }
  if (fade <= 0.f) {
    return;
  }

  int w = std::max(1, m_ssao_w / 2);
  int h = std::max(1, m_ssao_h / 2);
  if (w != m_shaft_w || h != m_shaft_h) {
    if (m_shaft_fbo) {
      glDeleteFramebuffers(1, &m_shaft_fbo);
      glDeleteTextures(1, &m_shaft_tex);
    }
    m_shaft_w = w;
    m_shaft_h = h;
    glGenFramebuffers(1, &m_shaft_fbo);
    glGenTextures(1, &m_shaft_tex);
    glBindTexture(GL_TEXTURE_2D, m_shaft_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shaft_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_shaft_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_shaft_fbo);
  glViewport(0, 0, w, h);
  auto& shader = render_state->shaders[ShaderId::ENH_SHAFTS];
  shader.activate();
  auto id = shader.id();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform2f(glGetUniformLocation(id, "sun_uv"), sx, sy);
  glUniform1f(glGetUniformLocation(id, "threshold"), m_settings.shafts_threshold);
  glUniform1f(glGetUniformLocation(id, "fade"), fade);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindVertexArray(0);
  m_shafts_ready = true;
}

void EnhancedPost::do_gi(GLuint scene_tex,
                         GLuint screen_vao,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof) {
  int w = std::max(1, m_ssao_w / 2);
  int h = std::max(1, m_ssao_h / 2);
  if (w != m_gi_w || h != m_gi_h) {
    if (m_gi_fbo[0]) {
      glDeleteFramebuffers(2, m_gi_fbo);
      glDeleteTextures(2, m_gi_tex);
    }
    m_gi_w = w;
    m_gi_h = h;
    glGenFramebuffers(2, m_gi_fbo);
    glGenTextures(2, m_gi_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_gi_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gi_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glViewport(0, 0, w, h);

  auto& gi = render_state->shaders[ShaderId::ENH_GI];
  gi.activate();
  auto id = gi.id();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, m_normals_ready ? m_normal_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T11"), 11);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &g_enhanced_camera.rot[0][0]);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / m_ssao_w, 1.f / m_ssao_h);
  glUniform1f(glGetUniformLocation(id, "radius"), m_settings.gi_radius * kMeter);
  glUniform1f(glGetUniformLocation(id, "max_distance"), 150.f * kMeter);
  glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[0]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();

  auto& blur = render_state->shaders[ShaderId::ENH_GI_BLUR];
  blur.activate();
  id = blur.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  for (int pass = 0; pass < 2; pass++) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[1 - pass]);
    glBindTexture(GL_TEXTURE_2D, m_gi_tex[pass]);
    glUniform2f(glGetUniformLocation(id, "step"), pass == 0 ? 1.f / w : 0.f,
                pass == 0 ? 0.f : 1.f / h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
  }
  glBindVertexArray(0);
  m_gi_ready = true;
}

namespace {
// texture alpha under this doesn't cast a shadow (leaf cards, cutouts)
constexpr float kShadowAlphaRef = 0.25f;
}  // namespace

void EnhancedPost::do_shadow_map(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_shadow_ready = false;
  float sun[3];
  sun_world(sun);
  const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  const auto& cam = g_enhanced_camera;
  if (!g_enhanced_sun.valid || len < 1e-6f || sun[1] / len < 0.05f || !cam.valid) {
    return;  // no sun, or below the horizon
  }

  constexpr int kRes = 4096;
  if (m_shadow_res != kRes) {
    m_shadow_res = kRes;
    glGenFramebuffers(1, &m_shadow_fbo);
    glGenTextures(1, &m_shadow_tex);
    glBindTexture(GL_TEXTURE_2D, m_shadow_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kRes, kRes, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // hardware comparison: sampler2DShadow, 2x2 filtered
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadow_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenVertexArrays(1, &m_shadow_vao);
  }

  // what to draw: geometry 0 (the most detailed) of every tfrag and tie tree of the levels in use;
  // tfrag keeps its indices on the CPU (they're streamed after culling), so they get a buffer here
  auto levels = render_state->loader->get_in_use_levels();
  std::vector<u64> in_use;
  for (auto* lev : levels) {
    in_use.push_back(lev->load_id);
    if (m_shadow_draws.count(lev->load_id)) {
      continue;
    }
    auto& draws = m_shadow_draws[lev->load_id];
    const auto& name = lev->level->level_name;
    const bool cut = name == "mhcitya" || name == "ctyport";
    const auto& tfrags = lev->level->tfrag_trees[0];
    for (size_t i = 0; i < tfrags.size() && i < lev->tfrag_vertex_data[0].size(); i++) {
      const auto& tree = tfrags[i];
      if (tree.kind == tfrag3::TFragmentTreeKind::WATER ||
          tree.kind == tfrag3::TFragmentTreeKind::LOWRES ||
          tree.kind == tfrag3::TFragmentTreeKind::LOWRES_TRANS || tree.unpacked.indices.empty()) {
        continue;
      }
      ShadowDraw d;
      d.vertices = lev->tfrag_vertex_data[0][i];
      glGenBuffers(1, &d.indices);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.unpacked.indices.size() * sizeof(u32),
                   tree.unpacked.indices.data(), GL_STATIC_DRAW);
      d.own_indices = true;
      d.count = tree.unpacked.indices.size();
      d.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      draws.push_back(d);
    }
    const auto& ties = lev->level->tie_trees[0];
    for (size_t i = 0; i < ties.size() && i < lev->tie_data[0].size(); i++) {
      if (ties[i].unpacked.indices.empty()) {
        continue;
      }
      ShadowDraw d;
      d.vertices = lev->tie_data[0][i].vertex_buffer;
      d.indices = lev->tie_data[0][i].index_buffer;
      d.count = ties[i].unpacked.indices.size();
      d.mode = ties[i].use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      draws.push_back(d);
    }
    add_wind_shadow_draws(lev, cut, &draws);
  }
  for (auto it = m_shadow_draws.begin(); it != m_shadow_draws.end();) {
    if (std::find(in_use.begin(), in_use.end(), it->first) == in_use.end()) {
      for (auto& d : it->second) {
        if (d.own_indices) {
          glDeleteBuffers(1, &d.indices);
        }
        if (d.own_vertices) {
          glDeleteBuffers(1, &d.vertices);
        }
      }
      it = m_shadow_draws.erase(it);
    } else {
      ++it;
    }
  }

  // the light's frame: f from the sun down, right and up across; a square of 2 * range around a
  // point ahead of the camera, snapped to whole texels so the edges don't crawl as it moves
  float f[3] = {-sun[0] / len, -sun[1] / len, -sun[2] / len};
  float hint[3] = {0.f, 0.f, 1.f};
  if (std::abs(f[2]) > 0.9f) {
    hint[0] = 1.f;
    hint[2] = 0.f;
  }
  float right[3] = {hint[1] * f[2] - hint[2] * f[1], hint[2] * f[0] - hint[0] * f[2],
                    hint[0] * f[1] - hint[1] * f[0]};
  float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (auto& r : right) {
    r /= rl;
  }
  float up[3] = {f[1] * right[2] - f[2] * right[1], f[2] * right[0] - f[0] * right[2],
                 f[0] * right[1] - f[1] * right[0]};
  const float range = m_settings.shadow_range * kMeter;
  const float depth = 300.f * kMeter;
  float center[3];
  for (int i = 0; i < 3; i++) {
    // the camera looks down view +z: its forward in world is column 2 of the rotation
    center[i] = cam.trans[i] + cam.rot[i][2] * range * 0.5f;
  }
  const float texel = 2.f * range / kRes;
  float cr = right[0] * center[0] + right[1] * center[1] + right[2] * center[2];
  float cu = up[0] * center[0] + up[1] * center[1] + up[2] * center[2];
  float cf = f[0] * center[0] + f[1] * center[1] + f[2] * center[2];
  cr = std::floor(cr / texel) * texel;
  cu = std::floor(cu / texel) * texel;
  const float rows[3][4] = {{right[0] / range, right[1] / range, right[2] / range, -cr / range},
                            {up[0] / range, up[1] / range, up[2] / range, -cu / range},
                            {f[0] / depth, f[1] / depth, f[2] / depth, -cf / depth}};
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 4; c++) {
      m_light_mvp[r * 4 + c] = rows[r][c];
    }
  }
  m_light_mvp[12] = m_light_mvp[13] = m_light_mvp[14] = 0.f;
  m_light_mvp[15] = 1.f;

  GLfloat old_clear_depth = 0.f;
  GLint old_depth_func = GL_GEQUAL;
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
  glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
  glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_fbo);
  glViewport(0, 0, kRes, kRes);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5f, 3.f);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  auto& shader = render_state->shaders[ShaderId::ENH_SHADOW];
  shader.activate();
  auto id = shader.id();
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  GLint u_cut = glGetUniformLocation(id, "enh_cut");
  GLint u_alpha = glGetUniformLocation(id, "alpha_ref");
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(m_shadow_vao);
  glEnableVertexAttribArray(0);
  for (auto& [lid, draws] : m_shadow_draws) {
    for (auto& d : draws) {
      if (d.wind && !m_settings.shadow_casters) {
        continue;
      }
      glUniform1i(u_cut, d.cut);
      glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
      if (d.own_vertices || d.wind) {
        // x, y, z, s, t
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
      } else {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex), (void*)0);
        glDisableVertexAttribArray(1);
      }
      glUniform1f(u_alpha, d.tex ? kShadowAlphaRef : -1.f);
      if (d.tex) {
        glBindTexture(GL_TEXTURE_2D, d.tex);
      }
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      glDrawElements(d.mode, d.count, GL_UNSIGNED_INT, (void*)(d.first * sizeof(u32)));
      prof.add_draw_call();
    }
  }
  glDisableVertexAttribArray(1);
  glBindVertexArray(0);
  // characters only with the sun (or moon) above ~20 degrees: lower, a 2 m character casts a band
  // tens of meters long that follows it around (the game's own short shadow stays)
  if (m_settings.shadow_casters && sun[1] / len > 0.34f) {
    draw_shadow_casters(render_state, prof);
  }
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_PRIMITIVE_RESTART);
  glClearDepth(old_clear_depth);
  glDepthFunc(old_depth_func);
  m_shadow_ready = true;
}

// The wind-blown ties (trees) of a level for the shadow map: the game draws them per instance with
// a matrix it sways each frame; the shadow uses the still matrices, baked into world positions once.
void EnhancedPost::add_wind_shadow_draws(const LevelData* lev,
                                         bool cut,
                                         std::vector<ShadowDraw>* draws) {
  const auto& ties = lev->level->tie_trees[0];
  size_t instances = 0;
  for (const auto& tree : ties) {
    instances += tree.wind_instance_info.size();
  }
  if (instances) {
    lg::info("[enhanced] shadow map {}: {} wind-blown tie instances", lev->level->level_name,
             instances);
  }
  for (const auto& tree : ties) {
    if (tree.instanced_wind_draws.empty()) {
      continue;
    }
    std::vector<float> verts;  // x, y, z, s, t
    std::vector<u32> indices;
    std::vector<ShadowDraw> tree_draws;
    for (const auto& draw : tree.instanced_wind_draws) {
      if (!draw.mode.get_depth_write_enable()) {
        continue;
      }
      ShadowDraw d;
      d.first = indices.size();
      d.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      d.wind = true;
      if (draw.mode.get_at_enable() && draw.tree_tex_id >= 0 &&
          draw.tree_tex_id < (int)lev->textures.size()) {
        d.tex = lev->textures[draw.tree_tex_id];
      }
      u32 off = 0;
      for (const auto& grp : draw.instance_groups) {
        const auto& m = tree.wind_instance_info.at(grp.instance_idx).matrix;
        for (u32 k = off; k < off + grp.num && k < draw.vertex_index_stream.size(); k++) {
          u32 vi = draw.vertex_index_stream[k];
          if (vi == UINT32_MAX || vi >= tree.unpacked.vertices.size()) {
            indices.push_back(UINT32_MAX);
            continue;
          }
          const auto& v = tree.unpacked.vertices[vi];
          for (int c = 0; c < 3; c++) {
            verts.push_back(m[0][c] * v.x + m[1][c] * v.y + m[2][c] * v.z + m[3][c]);
          }
          verts.push_back(v.s);
          verts.push_back(v.t);
          indices.push_back(verts.size() / 5 - 1);
        }
        // strips of different instances must not join
        indices.push_back(UINT32_MAX);
        off += grp.num;
      }
      d.count = indices.size() - d.first;
      if (d.count) {
        tree_draws.push_back(d);
      }
    }
    if (tree_draws.empty()) {
      continue;
    }
    GLuint vb = 0, ib = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ib);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(u32), indices.data(),
                 GL_STATIC_DRAW);
    for (auto& d : tree_draws) {
      d.vertices = vb;
      d.indices = ib;
    }
    // the first one owns the buffers
    tree_draws[0].own_vertices = true;
    tree_draws[0].own_indices = true;
    draws->insert(draws->end(), tree_draws.begin(), tree_draws.end());
  }
}

// The characters drawn this frame (see EnhancedCasters), skinned again with their bones and drawn
// into the shadow map. Their bones put them in camera space: back to the world with the camera.
void EnhancedPost::draw_shadow_casters(SharedRenderState* render_state,
                                       ScopedProfilerNode& prof) {
  auto& cs = g_enhanced_casters;
  cs.capture = false;  // later merc draws (HUD) don't cast
  if (cs.draws.empty()) {
    return;
  }
  // a bound range always spans 128 bones: room past the last one
  constexpr size_t kBoneBlock = 128 * 8 * 4;  // floats
  const size_t size = (cs.bones.size() + kBoneBlock) * sizeof(float);
  if (!m_caster_bones) {
    glGenBuffers(1, &m_caster_bones);
    glGenVertexArrays(1, &m_caster_vao);
  }
  glBindBuffer(GL_UNIFORM_BUFFER, m_caster_bones);
  if (size > m_caster_bones_size) {
    m_caster_bones_size = size * 3 / 2;
    glBufferData(GL_UNIFORM_BUFFER, m_caster_bones_size, nullptr, GL_DYNAMIC_DRAW);
  }
  glBufferSubData(GL_UNIFORM_BUFFER, 0, cs.bones.size() * sizeof(float), cs.bones.data());
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  auto& shader = render_state->shaders[ShaderId::ENH_SHADOW_MERC];
  shader.activate();
  const auto id = shader.id();
  const auto& cam = g_enhanced_camera;
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  // world = cam - rot * merc camera space (the view space negated; rot as stored, see EnhancedCamera)
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform3f(glGetUniformLocation(id, "cam_trans"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  GLint u_alpha = glGetUniformLocation(id, "alpha_ref");
  glBindVertexArray(m_caster_vao);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(5);
  GLuint last_vertices = 0;
  for (const auto& d : cs.draws) {
    if (d.vertices != last_vertices) {
      glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
      constexpr GLsizei kStride = sizeof(tfrag3::MercVertex);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, pos));
      glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, weights[0]));
      glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, st[0]));
      glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, kStride,
                             (void*)offsetof(tfrag3::MercVertex, mats[0]));
      last_vertices = d.vertices;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
    glUniform1f(u_alpha, d.tex ? kShadowAlphaRef : -1.f);
    if (d.tex) {
      glBindTexture(GL_TEXTURE_2D, d.tex);
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_caster_bones, d.first_bone * 4 * sizeof(float),
                      kBoneBlock * sizeof(float));
    glDrawElements(d.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES, d.index_count, GL_UNSIGNED_INT,
                   (void*)(sizeof(u32) * d.first_index));
    prof.add_draw_call();
  }
  glBindVertexArray(0);
}

void EnhancedPost::do_volume(SharedRenderState* render_state,
                             GLuint screen_vao,
                             ScopedProfilerNode& prof) {
  int w = std::max(1, m_ssao_w / 2);
  int h = std::max(1, m_ssao_h / 2);
  if (w != m_vol_w || h != m_vol_h) {
    if (m_vol_fbo) {
      glDeleteFramebuffers(1, &m_vol_fbo);
      glDeleteTextures(1, &m_vol_tex);
    }
    m_vol_w = w;
    m_vol_h = h;
    glGenFramebuffers(1, &m_vol_fbo);
    glGenTextures(1, &m_vol_tex);
    glBindTexture(GL_TEXTURE_2D, m_vol_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_vol_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vol_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  const auto& cam = g_enhanced_camera;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_vol_fbo);
  glViewport(0, 0, w, h);
  auto& shader = render_state->shaders[ShaderId::ENH_VOLUME];
  shader.activate();
  auto id = shader.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  // the near sea isn't in the scene depth: the mist stops at its surface, not on the sea bed
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D,
                m_ocean_ready && m_settings.water_reflections ? m_ocean_depth[1] : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  // view -> world is the transpose of world -> view
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniform3f(glGetUniformLocation(id, "up_view"), cam.rot[1][0], cam.rot[1][1], cam.rot[1][2]);
  {
    float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                          m_sun_view[2] * m_sun_view[2]);
    float up = len > 0.f ? (m_sun_view[0] * cam.rot[1][0] + m_sun_view[1] * cam.rot[1][1] +
                            m_sun_view[2] * cam.rot[1][2]) / len
                         : 0.f;
    const bool sun = g_enhanced_sun.valid && len > 0.f && up > 0.f;
    static int debug_counter = 0;
    if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
      lg::info("[enhanced] mist sun valid {} up {} view {} {} {}", g_enhanced_sun.valid, up,
               m_sun_view[0], m_sun_view[1], m_sun_view[2]);
    }
    glUniform4f(glGetUniformLocation(id, "sun_dir"), sun ? m_sun_view[0] / len : 0.f,
                sun ? m_sun_view[1] / len : 0.f, sun ? m_sun_view[2] / len : 0.f,
                sun ? std::min(1.f, up * 4.f) : 0.f);
  }
  glUniform3f(glGetUniformLocation(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f);
  glUniform4f(glGetUniformLocation(id, "mist"), m_settings.mist_density, m_settings.mist_height,
              m_settings.mist_distance, (float)m_time);
  glUniform4f(glGetUniformLocation(id, "mist_shape"), m_settings.mist_ground,
              m_settings.mist_noise, m_settings.mist_sun, m_settings.mist_aniso);
  glUniform1f(glGetUniformLocation(id, "frame"), (float)(m_frame % 64));
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready ? m_shadow_tex : m_no_shadow_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "shadow_on"), m_shadow_ready);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindVertexArray(0);
  m_vol_ready = true;
}

void EnhancedPost::free_ssao() {
  if (m_normal_fbo) {
    glDeleteFramebuffers(1, &m_normal_fbo);
    glDeleteTextures(1, &m_normal_tex);
    m_normal_fbo = m_normal_tex = 0;
  }
  if (m_depth_fbo) {
    glDeleteFramebuffers(1, &m_depth_fbo);
    glDeleteTextures(1, &m_depth_tex);
    glDeleteFramebuffers(2, m_ao_fbo);
    glDeleteTextures(2, m_ao_tex);
  }
  m_depth_fbo = m_depth_tex = 0;
  m_ao_fbo[0] = m_ao_fbo[1] = m_ao_tex[0] = m_ao_tex[1] = 0;
  m_ssao_w = m_ssao_h = 0;
}

void EnhancedPost::do_ssao(GLuint src_fbo,
                           int w,
                           int h,
                           GLuint screen_vao,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  m_ssao_ready = false;
  static int debug_counter = 0;
  if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
    lg::info("[enhanced] ssao camera valid {} a {} b {} fx {} fy {}", g_enhanced_camera.valid,
             g_enhanced_camera.depth_a, g_enhanced_camera.depth_b, g_enhanced_camera.fx,
             g_enhanced_camera.fy);
  }
  if (!g_enhanced_camera.valid || std::abs(g_enhanced_camera.depth_b) < 1e-12f) {
    return;  // no 3D scene drawn this frame (menus, loading)
  }

  if (w != m_ssao_w || h != m_ssao_h) {
    free_ssao();
    m_ssao_w = w;
    m_ssao_h = h;
    glGenFramebuffers(1, &m_depth_fbo);
    glGenTextures(1, &m_depth_tex);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depth_tex,
                           0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    glGenFramebuffers(1, &m_normal_fbo);
    glGenTextures(1, &m_normal_tex);
    glBindTexture(GL_TEXTURE_2D, m_normal_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_normal_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_normal_tex, 0);

    glGenFramebuffers(2, m_ao_fbo);
    glGenTextures(2, m_ao_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_ao_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ao_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // copy (and for msaa, resolve) the depth buffer into a texture we can sample
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  // and the level normals (msaa: averaged, renormalized in the shaders)
  m_normals_ready = false;
  if (g_enhanced_normals.enabled && src_fbo == g_enhanced_normals.fbo) {
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_normal_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    m_normals_ready = true;
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glViewport(0, 0, w, h);

  const auto& cam = g_enhanced_camera;
  const float fy = cam.fy * kScissorHeightScale;
  m_last_proj = {cam.depth_a, cam.depth_b, cam.fx, fy};

  // occlusion
  auto& ssao = render_state->shaders[ShaderId::ENH_SSAO];
  ssao.activate();
  auto id = ssao.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, m_normals_ready ? m_normal_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T11"), 11);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform4f(glGetUniformLocation(id, "proj"), cam.depth_a, cam.depth_b, cam.fx, fy);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / w, 1.f / h);
  glUniform1f(glGetUniformLocation(id, "radius"), m_settings.ssao_radius * kMeter);
  glUniform1f(glGetUniformLocation(id, "intensity"), m_settings.ssao_intensity);
  glUniform1f(glGetUniformLocation(id, "max_distance"), m_settings.ssao_max_distance * kMeter);
  {
    // sun direction into the view space the shader rebuilds from depth
    float world_sun[3];
    sun_world(world_sun);
    float sun[3] = {0, 0, 0};
    for (int col = 0; col < 3; col++) {
      for (int row = 0; row < 3; row++) {
        sun[col] += world_sun[row] * cam.rot[row][col];
      }
    }
    // test switch: JAK_ENHANCED_SUN_FLIP=1 puts the sun on the other side (god rays, shadows)
    static const bool flip = [] {
      const char* env = std::getenv("JAK_ENHANCED_SUN_FLIP");
      return env && std::strcmp(env, "1") == 0;
    }();
    for (int i = 0; i < 3; i++) {
      if (flip) {
        sun[i] = -sun[i];
      }
      m_sun_view[i] = sun[i];
    }
    // kept with the shadow map: characters aren't in the map, these give them their shadows
    const bool on = m_settings.sun_shadows && g_enhanced_sun.valid;
    glUniform4f(glGetUniformLocation(id, "sun"), sun[0], sun[1], sun[2],
                on ? m_settings.sun_shadow_strength : 0.f);
    glUniform1f(glGetUniformLocation(id, "sun_length"), m_settings.sun_shadow_length * kMeter);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[0]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();

  // depth-aware blur, horizontal then vertical; the result ends up back in m_ao_tex[0]
  auto& blur = render_state->shaders[ShaderId::ENH_SSAO_BLUR];
  blur.activate();
  id = blur.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform4f(glGetUniformLocation(id, "proj"), cam.depth_a, cam.depth_b, cam.fx, fy);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  for (int pass = 0; pass < 2; pass++) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[1 - pass]);
    glBindTexture(GL_TEXTURE_2D, m_ao_tex[pass]);
    glUniform2f(glGetUniformLocation(id, "step"), pass == 0 ? 1.f / w : 0.f,
                pass == 0 ? 0.f : 1.f / h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
  }
  m_ssao_ready = true;
  glBindVertexArray(0);
}

void EnhancedPost::free_levels() {
  for (auto& level : m_levels) {
    if (level.fbo) {
      glDeleteFramebuffers(1, &level.fbo);
      glDeleteTextures(1, &level.tex);
    }
    level = Level();
  }
  m_w = 0;
  m_h = 0;
}

void EnhancedPost::resize(int w, int h) {
  if (w == m_w && h == m_h) {
    return;
  }
  free_levels();
  m_w = w;
  m_h = h;
  int lw = w;
  int lh = h;
  for (auto& level : m_levels) {
    lw = std::max(1, lw / 2);
    lh = std::max(1, lh / 2);
    level.w = lw;
    level.h = lh;
    glGenFramebuffers(1, &level.fbo);
    glGenTextures(1, &level.tex);
    glBindTexture(GL_TEXTURE_2D, level.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, lw, lh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, level.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level.tex, 0);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void EnhancedPost::prepare(GLuint scene_tex,
                           GLuint scene_fbo,
                           int w,
                           int h,
                           GLuint screen_vao,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  if (!m_settings.enabled) {
    return;
  }
  m_ssao_ready = false;
  m_shafts_ready = false;
  m_gi_ready = false;
  m_vol_ready = false;
  m_shadow_ready = false;
  if (m_settings.shadow_map && g_enhanced_camera.valid) {
    do_shadow_map(render_state, prof);
  }
  if (m_settings.ssao) {
    do_ssao(scene_fbo, w, h, screen_vao, render_state, prof);
    if (m_settings.volume && m_ssao_ready) {
      do_volume(render_state, screen_vao, prof);
    }
    if (m_settings.gi && m_ssao_ready) {
      do_gi(scene_tex, screen_vao, render_state, prof);
    }
    if (m_settings.shafts && m_ssao_ready && g_enhanced_sun.valid) {
      do_shafts(scene_tex, screen_vao, render_state, prof);
    }
  }
  // set again by the next frame's background draws
  g_enhanced_camera.valid = false;
  if (!m_settings.bloom) {
    return;
  }
  resize(w, h);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glActiveTexture(GL_TEXTURE0);

  // down: bright pass into level 0, then 13-tap downsample through the chain
  auto& down = render_state->shaders[ShaderId::ENH_BLOOM_DOWN];
  down.activate();
  GLint u_texel = glGetUniformLocation(down.id(), "texel");
  GLint u_threshold = glGetUniformLocation(down.id(), "threshold");
  GLint u_knee = glGetUniformLocation(down.id(), "knee");
  GLint u_first = glGetUniformLocation(down.id(), "first_pass");
  glUniform1i(glGetUniformLocation(down.id(), "tex_T0"), 0);
  glUniform1f(u_threshold, m_settings.bloom_threshold);
  glUniform1f(u_knee, m_settings.bloom_knee);

  GLuint src = scene_tex;
  int src_w = w;
  int src_h = h;
  for (int i = 0; i < kLevels; i++) {
    auto& level = m_levels[i];
    glBindFramebuffer(GL_FRAMEBUFFER, level.fbo);
    glViewport(0, 0, level.w, level.h);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform2f(u_texel, 1.f / src_w, 1.f / src_h);
    glUniform1i(u_first, i == 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
    src = level.tex;
    src_w = level.w;
    src_h = level.h;
  }

  // up: tent-filter each level and add it onto the next bigger one
  auto& up = render_state->shaders[ShaderId::ENH_BLOOM_UP];
  up.activate();
  glUniform1i(glGetUniformLocation(up.id(), "tex_T0"), 0);
  GLint u_up_texel = glGetUniformLocation(up.id(), "texel");
  glUniform1f(glGetUniformLocation(up.id(), "radius"), m_settings.bloom_radius);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);
  for (int i = kLevels - 1; i > 0; i--) {
    auto& from = m_levels[i];
    auto& to = m_levels[i - 1];
    glBindFramebuffer(GL_FRAMEBUFFER, to.fbo);
    glViewport(0, 0, to.w, to.h);
    glBindTexture(GL_TEXTURE_2D, from.tex);
    glUniform2f(u_up_texel, 1.f / from.w, 1.f / from.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
  }
  glDisable(GL_BLEND);
  glBindVertexArray(0);
}

Shader& EnhancedPost::activate_composite(SharedRenderState* render_state, int w, int h) {
  auto& shader = render_state->shaders[ShaderId::ENH_COMPOSITE];
  shader.activate();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_settings.bloom && m_levels[0].tex ? m_levels[0].tex : m_black_tex);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, m_ssao_ready ? m_ao_tex[0] : m_white_tex);
  // depth for the depth of field (only valid when the occlusion pass copied it this frame)
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, m_ssao_ready ? m_depth_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE4);
  glBindTexture(GL_TEXTURE_2D, m_shafts_ready ? m_shaft_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE7);
  glBindTexture(GL_TEXTURE_2D, m_gi_ready ? m_gi_tex[0] : m_black_tex);
  glActiveTexture(GL_TEXTURE8);
  glBindTexture(GL_TEXTURE_2D, m_vol_ready ? m_vol_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready ? m_shadow_tex : m_no_shadow_tex);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, m_normals_ready ? m_normal_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);

  auto id = shader.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform1i(glGetUniformLocation(id, "ao_debug"), m_settings.ssao_debug);
  glUniform1i(glGetUniformLocation(id, "tex_T11"), 11);
  glUniform1i(glGetUniformLocation(id, "normal_debug"), m_settings.normal_debug);
  glUniform1i(glGetUniformLocation(id, "tex_T3"), 3);
  glUniform1i(glGetUniformLocation(id, "tex_T4"), 4);
  glUniform1i(glGetUniformLocation(id, "tex_T7"), 7);
  glUniform1i(glGetUniformLocation(id, "tex_T8"), 8);
  glUniform1i(glGetUniformLocation(id, "tex_T9"), 9);
  glUniform1f(glGetUniformLocation(id, "shadow_strength"),
              m_shadow_ready ? m_settings.shadow_strength : 0.f);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  glUniform1i(glGetUniformLocation(id, "volume"), m_vol_ready);
  glUniform2f(glGetUniformLocation(id, "vol_texel"), m_vol_w ? 1.f / m_vol_w : 0.f,
              m_vol_h ? 1.f / m_vol_h : 0.f);
  glUniform1f(glGetUniformLocation(id, "gi_strength"), m_gi_ready ? m_settings.gi_strength : 0.f);
  glUniform1f(glGetUniformLocation(id, "shafts_strength"),
              m_shafts_ready ? m_settings.shafts_strength : 0.f);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  const bool dof = m_settings.dof && m_ssao_ready;
  glUniform3f(glGetUniformLocation(id, "dof"), m_settings.dof_start * kMeter,
              m_settings.dof_end * kMeter, dof ? m_settings.dof_radius * h / 1080.f : 0.f);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / w, 1.f / h);
  glUniform1f(glGetUniformLocation(id, "bloom_strength"),
              m_settings.bloom ? m_settings.bloom_strength : 0.f);
  glUniform1f(glGetUniformLocation(id, "saturation"), m_settings.saturation);
  glUniform1f(glGetUniformLocation(id, "contrast"), m_settings.contrast);
  glUniform1f(glGetUniformLocation(id, "warmth"), m_settings.warmth);
  glUniform1f(glGetUniformLocation(id, "vignette"), m_settings.vignette);
  glUniform1f(glGetUniformLocation(id, "sharpen"), m_settings.sharpen);
  return shader;
}

void EnhancedPost::draw_menu() {
  ImGui::SetNextWindowPos(ImVec2(30, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  // no close button: \ closes it (and gives the mouse back to the camera)
  if (!ImGui::Begin("Jak 3 Enhanced  ( \\ chiude )", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  auto& s = m_settings;
  if (ImGui::CollapsingHeader("Grafica", ImGuiTreeNodeFlags_DefaultOpen)) {
    static const char* presets[] = {"Classico", "Medio", "Massimo"};
    ImGui::TextUnformatted("Preset:");
    for (int i = 0; i < 3; i++) {
      ImGui::SameLine();
      if (ImGui::Button(presets[i], ImVec2(120, 0))) {
        apply_preset(i);
      }
    }
    ImGui::Checkbox("Grafica Enhanced (tutto)", &s.enabled);
    ImGui::BeginDisabled(!s.enabled);
    // two columns of switches
    int n = 0;
    auto check = [&](const char* label, bool* value) {
      if (n++ % 2) {
        ImGui::SameLine(260);
      }
      return ImGui::Checkbox(label, value);
    };
    check("Antialiasing (TAA)", &s.taa);
    check("Nebbia volumetrica", &s.volume);
    check("Ombre del sole", &s.shadow_map);
    check("Ombre di personaggi e alberi", &s.shadow_casters);
    check("Ombre di contatto", &s.sun_shadows);
    check("Occlusione (SSAO)", &s.ssao);
    check("Luce indiretta (SSGI)", &s.gi);
    check("Luce sui personaggi", &g_enhanced_merc.enabled);
    check("Bloom", &s.bloom);
    check("Raggi di luce", &s.shafts);
    check("Profondita' di campo", &s.dof);
    bool blur = s.motion_blur > 0.f;
    if (check("Motion blur", &blur)) {
      s.motion_blur = blur ? 0.5f : 0.f;
    }
    check("Luci dinamiche", &s.lights);
    check("Fuochi (calore, braci)", &s.fire);
    bool light_shadows = s.light_shadow > 0.f;
    if (check("Ombre delle luci", &light_shadows)) {
      s.light_shadow = light_shadows ? 0.85f : 0.f;
    }
    check("Normali della geometria", &s.geometry_normals);
    bool prints = s.prints > 0.f;
    if (check("Impronte nella sabbia", &prints)) {
      s.prints = prints ? 1.f : 0.f;
    }
    bool smoke = s.lit_smoke > 0.f;
    if (check("Fumo illuminato", &smoke)) {
      s.lit_smoke = smoke ? 0.8f : 0.f;
    }
    check("Distanza visiva estesa", &s.far_view);
    check("Vento sull'erba", &g_enhanced_wind.enabled);
    check("Erba che si piega", &g_enhanced_wind.push);
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("Vegetazione", &g_enhanced_vegetation, "Classica\0Media\0Rigogliosa\0");
    ImGui::BeginDisabled(!s.lights);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Forza lampade", &s.lamp_strength, 0.f, 3.f, "%.2f");
    ImGui::EndDisabled();
    ImGui::SeparatorText("Acqua");
    n = 0;
    bool swell = s.water_swell > 0.f, refr = s.water_refraction > 0.f, caus = s.water_caustics > 0.f;
    check("Riflessi", &s.water_reflections);
    if (check("Onde lunghe", &swell)) {
      s.water_swell = swell ? 1.f : 0.f;
    }
    if (check("Rifrazione", &refr)) {
      s.water_refraction = refr ? 0.012f : 0.f;
    }
    if (check("Caustiche", &caus)) {
      s.water_caustics = caus ? 0.8f : 0.f;
    }
    ImGui::EndDisabled();
  }
  if (ImGui::CollapsingHeader("Citta'", ImGuiTreeNodeFlags_DefaultOpen)) {
    // passers-by, traffic, guards and Metal Heads (the game scales its wanted counts, see
    // enhanced-population-count); the game reads the same variable at boot
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Popolazione", &m_population, "Bassa\0Media\0Alta\0Estrema\0")) {
      g_enhanced_menu.ask(6, std::to_string(m_population));
    }
  }
  save_menu_settings();
  if (ImGui::CollapsingHeader("Ora del giorno", ImGuiTreeNodeFlags_DefaultOpen)) {
    struct Hour {
      const char* name;
      int hour;
    };
    static const Hour hours[] = {{"Alba", 6},     {"Mattina", 9}, {"Mezzogiorno", 12},
                                 {"Tramonto", 18}, {"Sera", 20},  {"Notte", 23}};
    for (int i = 0; i < 6; i++) {
      if (i % 3) {
        ImGui::SameLine();
      }
      if (ImGui::Button(hours[i].name, ImVec2(159, 0))) {
        g_enhanced_menu.ask(1, std::to_string(hours[i].hour));
      }
    }
    static int hour = 12;
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("##ora", &hour, 0, 23, "ore %d");
    ImGui::SameLine();
    if (ImGui::Button("Imposta")) {
      g_enhanced_menu.ask(1, std::to_string(hour));
    }
    static bool frozen = false;
    if (ImGui::Checkbox("Ferma il tempo", &frozen)) {
      g_enhanced_menu.ask(4, frozen ? "0" : "1");
    }
  }
  if (ImGui::CollapsingHeader("Teletrasporto", ImGuiTreeNodeFlags_DefaultOpen)) {
    struct Place {
      const char* name;
      const char* continue_point;
    };
    static const Place places[] = {
        {"Piazza (Jak 2)", "piazza-start"},
        {"Porto", "ctyport-start"},
        {"Hip Hog", "ctyport-hiphog"},
        {"Vicolo degli slum", "ctysluma-alley"},
        {"Zona industriale", "ctyinda-start"},
        {"Settore centrale", "ctygenb-start"},
        {"Teste di Metallo", "mhcitya-near-ctyport"},
        {"Spargus", "wascitya-start"},
        {"Deserto", "desert-facing-wasdoors"},
    };
    for (int i = 0; i < (int)(sizeof(places) / sizeof(places[0])); i++) {
      const auto& p = places[i];
      if (i % 2) {
        ImGui::SameLine();
      }
      if (ImGui::Button(p.name, ImVec2(240, 0))) {
        g_enhanced_menu.ask(2, p.continue_point);
      }
    }
    const auto& cam = g_enhanced_camera;
    if (cam.valid) {
      ImGui::TextDisabled("camera %.0f %.0f %.0f m", cam.trans[0] / kMeter, cam.trans[1] / kMeter,
                          cam.trans[2] / kMeter);
    }
  }
  ImGui::End();
}

std::string EnhancedPost::menu_settings_json() const {
  const auto& s = m_settings;
  nlohmann::json j;
  j["enabled"] = s.enabled;
  j["taa"] = s.taa;
  j["volume"] = s.volume;
  j["shadow_map"] = s.shadow_map;
  j["shadow_casters"] = s.shadow_casters;
  j["sun_shadows"] = s.sun_shadows;
  j["ssao"] = s.ssao;
  j["gi"] = s.gi;
  j["characters"] = g_enhanced_merc.enabled;
  j["bloom"] = s.bloom;
  j["shafts"] = s.shafts;
  j["dof"] = s.dof;
  j["motion_blur"] = s.motion_blur;
  j["lights"] = s.lights;
  j["lamp_strength"] = s.lamp_strength;
  j["fire"] = s.fire;
  j["light_shadow"] = s.light_shadow;
  j["geometry_normals"] = s.geometry_normals;
  j["prints"] = s.prints;
  j["lit_smoke"] = s.lit_smoke;
  j["far_view"] = s.far_view;
  j["wind"] = g_enhanced_wind.enabled;
  j["grass_push"] = g_enhanced_wind.push;
  j["vegetation"] = g_enhanced_vegetation;
  j["water_reflections"] = s.water_reflections;
  j["water_swell"] = s.water_swell;
  j["water_refraction"] = s.water_refraction;
  j["water_caustics"] = s.water_caustics;
  j["population"] = m_population;
  return j.dump(2);
}

void EnhancedPost::apply_menu_settings(const std::string& text) {
  auto j = parse_commented_json(text, "enhanced-settings.json");
  auto get = [&](const char* key, auto& value) {
    if (j.contains(key)) {
      value = j.at(key).get<std::decay_t<decltype(value)>>();
    }
  };
  auto& s = m_settings;
  get("enabled", s.enabled);
  get("taa", s.taa);
  get("volume", s.volume);
  get("shadow_map", s.shadow_map);
  get("shadow_casters", s.shadow_casters);
  get("sun_shadows", s.sun_shadows);
  get("ssao", s.ssao);
  get("gi", s.gi);
  get("characters", g_enhanced_merc.enabled);
  get("bloom", s.bloom);
  get("shafts", s.shafts);
  get("dof", s.dof);
  get("motion_blur", s.motion_blur);
  get("lights", s.lights);
  get("lamp_strength", s.lamp_strength);
  get("fire", s.fire);
  get("light_shadow", s.light_shadow);
  get("geometry_normals", s.geometry_normals);
  get("prints", s.prints);
  get("lit_smoke", s.lit_smoke);
  get("far_view", s.far_view);
  get("wind", g_enhanced_wind.enabled);
  get("grass_push", g_enhanced_wind.push);
  if (!std::getenv("JAK_ENHANCED_VEGETATION")) {
    get("vegetation", g_enhanced_vegetation);
    g_enhanced_vegetation = std::clamp(g_enhanced_vegetation, 0, 2);
  }
  get("water_reflections", s.water_reflections);
  get("water_swell", s.water_swell);
  get("water_refraction", s.water_refraction);
  get("water_caustics", s.water_caustics);
  get("population", m_population);
  m_population = std::clamp(m_population, 0, 3);
}

// Classico: the game as it was (the enhanced pass off), Medio: the costly passes off (mist, indirect
// light, depth of field, the lights' shadows...), Massimo: everything on
void EnhancedPost::apply_preset(int preset) {
  auto& s = m_settings;
  const Settings d;
  s.enabled = preset > 0;
  s.taa = d.taa;
  s.shadow_map = d.shadow_map;
  s.sun_shadows = d.sun_shadows;
  s.ssao = d.ssao;
  s.bloom = d.bloom;
  s.lights = d.lights;
  s.lamp_strength = d.lamp_strength;
  s.fire = d.fire;
  s.geometry_normals = d.geometry_normals;
  s.prints = d.prints;
  s.lit_smoke = d.lit_smoke;
  s.far_view = d.far_view;
  s.water_reflections = d.water_reflections;
  s.water_swell = d.water_swell;
  bool all = preset == 2;
  s.volume = all && d.volume;
  s.shadow_casters = all && d.shadow_casters;
  s.gi = all && d.gi;
  s.shafts = all && d.shafts;
  s.dof = all && d.dof;
  s.motion_blur = all ? d.motion_blur : 0.f;
  s.light_shadow = all ? d.light_shadow : 0.f;
  s.water_refraction = all ? d.water_refraction : 0.f;
  s.water_caustics = all ? d.water_caustics : 0.f;
  g_enhanced_merc.enabled = preset > 0;
  g_enhanced_wind.enabled = preset > 0;
  g_enhanced_wind.push = preset > 0;
  g_enhanced_vegetation = preset;
}

// written only when something changed (the menu calls it every frame it is open)
void EnhancedPost::save_menu_settings() {
  auto text = menu_settings_json();
  if (text == m_saved_settings) {
    return;
  }
  m_saved_settings = text;
  try {
    auto path = file_util::get_user_settings_dir(g_game_version) / "enhanced-settings.json";
    file_util::create_dir_if_needed_for_file(path);
    file_util::write_text_file(path, text);
  } catch (std::exception& e) {
    lg::error("[enhanced] could not save the settings: {}", e.what());
  }
}

// A warm glow this frame: merged with the one seen at the same spot (a flame is often several
// glows, and its glow comes and goes), the memory entry if it has been there long enough to be a fire
// A fire has been glowing at its spot for a second, in most frames (a gun fired from where a
// guard stands glows there too, but in bursts)
bool EnhancedPost::is_fire(const FireMemory& f) const {
  double age = m_time - f.first;
  return age >= 1.0 && f.hits >= 0.6 * age * 60.0;
}

const EnhancedPost::FireMemory* EnhancedPost::note_fire_glow(const float* pos, float size) {
  constexpr float kSame = 1.5f * kMeter;
  for (auto& f : m_fires) {
    float dx = f.pos[0] - pos[0], dy = f.pos[1] - pos[1], dz = f.pos[2] - pos[2];
    if (dx * dx + dy * dy + dz * dz < kSame * kSame) {
      if (f.last != m_time) {
        f.hits++;
        f.last = m_time;
      }
      f.size = std::max(f.size * 0.98f, size);
      return is_fire(f) ? &f : nullptr;
    }
  }
  if (m_fires.size() < 64) {
    m_fires.push_back({{pos[0], pos[1], pos[2]}, size, m_time, m_time, 1});
  }
  return nullptr;
}

void EnhancedPost::draw_debug_window() {
  if (ImGui::TreeNode("Enhanced Post")) {
    ImGui::Checkbox("Enabled", &m_settings.enabled);
    ImGui::Checkbox("SSAO", &m_settings.ssao);
    ImGui::SliderFloat("SSAO radius (m)", &m_settings.ssao_radius, 0.2f, 5.f);
    ImGui::SliderFloat("SSAO intensity", &m_settings.ssao_intensity, 0.f, 3.f);
    ImGui::SliderFloat("SSAO max distance (m)", &m_settings.ssao_max_distance, 10.f, 500.f);
    ImGui::Checkbox("SSAO debug view", &m_settings.ssao_debug);
    ImGui::Checkbox("Sun shadow map", &m_settings.shadow_map);
    ImGui::Checkbox("Characters and wind trees cast", &m_settings.shadow_casters);
    ImGui::SliderFloat("Shadow map strength", &m_settings.shadow_strength, 0.f, 1.f);
    ImGui::SliderFloat("Shadow map range (m)", &m_settings.shadow_range, 20.f, 200.f);
    ImGui::Checkbox("Sun shadows", &m_settings.sun_shadows);
    ImGui::SliderFloat("Sun shadow strength", &m_settings.sun_shadow_strength, 0.f, 1.f);
    ImGui::SliderFloat("Sun shadow length (m)", &m_settings.sun_shadow_length, 0.5f, 10.f);
    ImGui::Checkbox("Depth of field", &m_settings.dof);
    ImGui::SliderFloat("DoF start (m)", &m_settings.dof_start, 10.f, 500.f);
    ImGui::SliderFloat("DoF end (m)", &m_settings.dof_end, 20.f, 1000.f);
    ImGui::SliderFloat("DoF radius (px)", &m_settings.dof_radius, 0.f, 8.f);
    ImGui::Checkbox("God rays", &m_settings.shafts);
    ImGui::SliderFloat("God rays strength", &m_settings.shafts_strength, 0.f, 2.f);
    ImGui::SliderFloat("God rays threshold", &m_settings.shafts_threshold, 0.f, 1.f);
    ImGui::Checkbox("FXAA", &m_settings.fxaa);
    ImGui::Checkbox("TAA", &m_settings.taa);
    ImGui::SliderFloat("TAA history (still)", &m_settings.taa_history, 0.5f, 0.97f);
    ImGui::SliderFloat("TAA history (fast)", &m_settings.taa_history_fast, 0.3f, 0.95f);
    ImGui::SliderFloat("Fog density (/m)", &m_settings.fog_density, 0.f, 0.01f, "%.4f");
    ImGui::SliderFloat("Fog height falloff (/m)", &m_settings.fog_height, 0.f, 0.2f);
    ImGui::SliderFloat("Fog amount", &m_settings.fog_amount, 0.f, 1.f);
    ImGui::SliderFloat("Heat haze", &m_settings.haze_strength, 0.f, 0.005f, "%.4f");
    ImGui::Checkbox("Volumetric mist", &m_settings.volume);
    ImGui::SliderFloat("Mist density (/m)", &m_settings.mist_density, 0.f, 0.1f, "%.4f");
    ImGui::SliderFloat("Mist height (m)", &m_settings.mist_height, 0.5f, 30.f);
    ImGui::SliderFloat("Mist distance (m)", &m_settings.mist_distance, 20.f, 400.f);
    ImGui::SliderFloat("Mist ground below camera (m)", &m_settings.mist_ground, -5.f, 20.f);
    ImGui::SliderFloat("Mist patches", &m_settings.mist_noise, 0.f, 1.f);
    ImGui::SliderFloat("Mist sunlight", &m_settings.mist_sun, 0.f, 4.f);
    ImGui::SliderFloat("Mist forward scattering", &m_settings.mist_aniso, 0.f, 0.95f);
    ImGui::SliderFloat("Motion blur", &m_settings.motion_blur, 0.f, 1.f);
    ImGui::SliderFloat("Sun relight", &m_settings.relight, 0.f, 2.f);
    ImGui::Checkbox("Dynamic lights (glows)", &m_settings.lights);
    ImGui::SliderFloat("Dynamic light strength", &m_settings.light_strength, 0.f, 4.f);
    ImGui::SliderFloat("Dynamic light reach (x glow)", &m_settings.light_reach, 1.f, 12.f);
    ImGui::SliderFloat("Lamp strength", &m_settings.lamp_strength, 0.f, 4.f);
    ImGui::SliderFloat("Lamp reach (x game)", &m_settings.lamp_reach, 0.25f, 3.f);
    ImGui::SliderFloat("Dynamic light shadows", &m_settings.light_shadow, 0.f, 1.f);
    ImGui::SliderFloat("Explosions (flash, shockwave)", &m_settings.blast, 0.f, 3.f);
    ImGui::SliderFloat("Wake rings on the water", &m_settings.wake, 0.f, 3.f);
    ImGui::Checkbox("Fires (flicker, hot air, embers)", &m_settings.fire);
    ImGui::SliderFloat("Fire hot air", &m_settings.fire_haze, 0.f, 0.05f, "%.3f");
    ImGui::SliderFloat("Fire embers", &m_settings.fire_embers, 0.f, 3.f);
    ImGui::Checkbox("Indirect light (SSGI)", &m_settings.gi);
    ImGui::SliderFloat("Indirect strength", &m_settings.gi_strength, 0.f, 3.f);
    ImGui::SliderFloat("Indirect radius (m)", &m_settings.gi_radius, 0.5f, 10.f);
    ImGui::Checkbox("Wind on grass/leaves", &g_enhanced_wind.enabled);
    ImGui::SliderFloat("Wind strength", &g_enhanced_wind.strength, 0.f, 3.f);
    ImGui::Checkbox("Character rim/highlights", &g_enhanced_merc.enabled);
    ImGui::SliderFloat("Character rim light", &g_enhanced_merc.rim, 0.f, 1.5f);
    ImGui::SliderFloat("Character highlights", &g_enhanced_merc.spec, 0.f, 1.5f);
    ImGui::Checkbox("Water reflections", &m_settings.water_reflections);
    ImGui::SliderFloat("Water reflection strength", &m_settings.water_reflection_strength, 0.f, 1.f);
    ImGui::ColorEdit3("Deep water", m_settings.water_deep);
    ImGui::ColorEdit3("Shallow water", m_settings.water_shallow);
    ImGui::SliderFloat("Water color (ours vs game)", &m_settings.water_unify, 0.f, 1.f);
    ImGui::SliderFloat("Water swell", &m_settings.water_swell, 0.f, 3.f);
    ImGui::SliderFloat("Water refraction", &m_settings.water_refraction, 0.f, 0.05f);
    ImGui::SliderFloat("Water caustics", &m_settings.water_caustics, 0.f, 3.f);
    ImGui::SliderFloat("Sun glints", &m_settings.water_glint, 0.f, 3.f);
    ImGui::Checkbox("Bloom", &m_settings.bloom);
    ImGui::SliderFloat("Bloom threshold", &m_settings.bloom_threshold, 0.f, 1.f);
    ImGui::SliderFloat("Bloom knee", &m_settings.bloom_knee, 0.f, 1.f);
    ImGui::SliderFloat("Bloom strength", &m_settings.bloom_strength, 0.f, 2.f);
    ImGui::SliderFloat("Bloom radius", &m_settings.bloom_radius, 0.5f, 3.f);
    ImGui::SliderFloat("Saturation", &m_settings.saturation, 0.f, 2.f);
    ImGui::SliderFloat("Contrast", &m_settings.contrast, 0.f, 1.f);
    ImGui::SliderFloat("Warmth", &m_settings.warmth, -0.2f, 0.2f);
    ImGui::SliderFloat("Vignette", &m_settings.vignette, 0.f, 1.f);
    ImGui::SliderFloat("Sharpen", &m_settings.sharpen, 0.f, 1.f);
    ImGui::TreePop();
  }
}
