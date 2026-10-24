#include "EnhancedPost.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/enhanced_state.h"

#include "game/graphics/pipelines/opengl.h"

#include "third-party/imgui/imgui.h"

EnhancedCamera g_enhanced_camera;
EnhancedSun g_enhanced_sun;
EnhancedMerc g_enhanced_merc;
EnhancedWind g_enhanced_wind;

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
  const char* ao_debug = std::getenv("JAK_ENHANCED_AO_DEBUG");
  if (ao_debug && std::strcmp(ao_debug, "1") == 0) {
    m_settings.ssao_debug = true;
  }

  // bound in place of the bloom / occlusion when they're off, so the composite always samples
  m_black_tex = make_1x1(0);
  m_white_tex = make_1x1(0xffffffff);
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
  glDeleteTextures(1, &m_black_tex);
  glDeleteTextures(1, &m_white_tex);
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
  glBindTexture(GL_TEXTURE_2D, m_scene_tex);
  // the game's brightness/contrast is applied later, on the whole frame
  glUniform4f(glGetUniformLocation(id, "color_mult"), 1.f, 1.f, 1.f, 1.f);
  glUniform4f(glGetUniformLocation(id, "color_add"), 0.f, 0.f, 0.f, 0.f);

  // world up in view space, for the fog and the heat haze
  glUniform3f(glGetUniformLocation(id, "up_view"), cam.rot[1][0], cam.rot[1][1], cam.rot[1][2]);
  const bool have_scene = m_ssao_ready;
  glUniform4f(glGetUniformLocation(id, "fog"), m_settings.fog_density / kMeter,
              m_settings.fog_height / kMeter, have_scene ? m_settings.fog_amount : 0.f, 0.f);
  glUniform3f(glGetUniformLocation(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f);
  m_time += 1.0 / 60.0;
  glUniform4f(glGetUniformLocation(id, "haze"), have_scene ? m_settings.haze_strength : 0.f,
              120.f * kMeter, 400.f * kMeter, (float)m_time);
  glUniform1i(glGetUniformLocation(id, "fxaa"), m_settings.fxaa);

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
  glUniform3fv(glGetUniformLocation(id, "water_deep_color"), 1, m_settings.water_deep);
  glUniform3fv(glGetUniformLocation(id, "water_shallow_color"), 1, m_settings.water_shallow);
  glUniform1f(glGetUniformLocation(id, "water_unify"), m_settings.water_unify);
  glUniform1f(glGetUniformLocation(id, "relight"), m_settings.relight);
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
  if (have_scene && m_prev_camera.valid && cam.valid && m_settings.motion_blur > 0.f) {
    float to_prev[3][3];  // [k][c]: column k of the GLSL mat3
    float offset[3];
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
    if (std::sqrt(moved) < 8.f * kMeter) {
      strength = m_settings.motion_blur;
      glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
      glUniform3f(glGetUniformLocation(id, "to_prev_offset"), offset[0], offset[1], offset[2]);
    }
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
    float sun[3] = {0, 0, 0};
    for (int col = 0; col < 3; col++) {
      for (int row = 0; row < 3; row++) {
        sun[col] += g_enhanced_sun.dir[row] * cam.rot[row][col];
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
  if (m_settings.ssao) {
    do_ssao(scene_fbo, w, h, screen_vao, render_state, prof);
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
  glActiveTexture(GL_TEXTURE0);

  auto id = shader.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform1i(glGetUniformLocation(id, "ao_debug"), m_settings.ssao_debug);
  glUniform1i(glGetUniformLocation(id, "tex_T3"), 3);
  glUniform1i(glGetUniformLocation(id, "tex_T4"), 4);
  glUniform1i(glGetUniformLocation(id, "tex_T7"), 7);
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
    ImGui::SliderFloat("Fog density (/m)", &m_settings.fog_density, 0.f, 0.01f, "%.4f");
    ImGui::SliderFloat("Fog height falloff (/m)", &m_settings.fog_height, 0.f, 0.2f);
    ImGui::SliderFloat("Fog amount", &m_settings.fog_amount, 0.f, 1.f);
    ImGui::SliderFloat("Heat haze", &m_settings.haze_strength, 0.f, 0.005f, "%.4f");
    ImGui::SliderFloat("Motion blur", &m_settings.motion_blur, 0.f, 1.f);
    ImGui::SliderFloat("Sun relight", &m_settings.relight, 0.f, 2.f);
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
