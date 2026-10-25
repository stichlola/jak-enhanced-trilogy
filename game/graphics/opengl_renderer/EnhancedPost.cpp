#include "EnhancedPost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "game/graphics/pipelines/opengl.h"

#include "third-party/imgui/imgui.h"

EnhancedCamera g_enhanced_camera;
EnhancedSun g_enhanced_sun;
EnhancedMerc g_enhanced_merc;
EnhancedWind g_enhanced_wind;
EnhancedLights g_enhanced_lights;
EnhancedGameLights g_enhanced_game_lights;

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
    const bool on = m_settings.lights && have_scene && cam.valid;
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
  }
  for (auto it = m_shadow_draws.begin(); it != m_shadow_draws.end();) {
    if (std::find(in_use.begin(), in_use.end(), it->first) == in_use.end()) {
      for (auto& d : it->second) {
        if (d.own_indices) {
          glDeleteBuffers(1, &d.indices);
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
  glBindVertexArray(m_shadow_vao);
  glEnableVertexAttribArray(0);
  for (auto& [lid, draws] : m_shadow_draws) {
    for (auto& d : draws) {
      glUniform1i(u_cut, d.cut);
      glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex), (void*)0);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      glDrawElements(d.mode, d.count, GL_UNSIGNED_INT, nullptr);
      prof.add_draw_call();
    }
  }
  glBindVertexArray(0);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_PRIMITIVE_RESTART);
  glClearDepth(old_clear_depth);
  glDepthFunc(old_depth_func);
  m_shadow_ready = true;
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
  glActiveTexture(GL_TEXTURE0);

  auto id = shader.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform1i(glGetUniformLocation(id, "ao_debug"), m_settings.ssao_debug);
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

void EnhancedPost::draw_debug_window() {
  if (ImGui::TreeNode("Enhanced Post")) {
    ImGui::Checkbox("Enabled", &m_settings.enabled);
    ImGui::Checkbox("SSAO", &m_settings.ssao);
    ImGui::SliderFloat("SSAO radius (m)", &m_settings.ssao_radius, 0.2f, 5.f);
    ImGui::SliderFloat("SSAO intensity", &m_settings.ssao_intensity, 0.f, 3.f);
    ImGui::SliderFloat("SSAO max distance (m)", &m_settings.ssao_max_distance, 10.f, 500.f);
    ImGui::Checkbox("SSAO debug view", &m_settings.ssao_debug);
    ImGui::Checkbox("Sun shadow map", &m_settings.shadow_map);
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
