#include "EnhancedPost.h"

#include <cstdlib>
#include <cstring>

#include "game/graphics/pipelines/opengl.h"

#include "third-party/imgui/imgui.h"

EnhancedPost::EnhancedPost() {
  const char* env = std::getenv("JAK_ENHANCED");
  if (env && std::strcmp(env, "0") == 0) {
    m_settings.enabled = false;
  }

  // bound in place of the bloom when it's off, so the composite shader always samples something
  glGenTextures(1, &m_black_tex);
  glBindTexture(GL_TEXTURE_2D, m_black_tex);
  u32 black = 0;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &black);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
}

EnhancedPost::~EnhancedPost() {
  free_levels();
  glDeleteTextures(1, &m_black_tex);
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
                           int w,
                           int h,
                           GLuint screen_vao,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  if (!m_settings.enabled || !m_settings.bloom) {
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
  glActiveTexture(GL_TEXTURE0);

  auto id = shader.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
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
