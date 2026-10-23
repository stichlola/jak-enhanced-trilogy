#include "EnhancedPost.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/enhanced_state.h"

#include "game/graphics/pipelines/opengl.h"

#include "third-party/imgui/imgui.h"

EnhancedCamera g_enhanced_camera;
EnhancedSun g_enhanced_sun;

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
  glDeleteTextures(1, &m_black_tex);
  glDeleteTextures(1, &m_white_tex);
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
  if (m_settings.ssao) {
    do_ssao(scene_fbo, w, h, screen_vao, render_state, prof);
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
  glActiveTexture(GL_TEXTURE0);

  auto id = shader.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform1i(glGetUniformLocation(id, "ao_debug"), m_settings.ssao_debug);
  glUniform1i(glGetUniformLocation(id, "tex_T3"), 3);
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
