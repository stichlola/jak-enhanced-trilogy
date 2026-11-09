// The passes' buffers and the frame's setup: SSAO, resize, prepare, the ray traced cover and the
// composite's uniforms (split from EnhancedPost.cpp; shared helpers in EnhancedPostInternal.h)

#include "game/graphics/opengl_renderer/EnhancedPostInternal.h"

using namespace enh_post;

static GLuint load_tiling_picture(const char* name, float avg[3]) {
  const auto path = file_util::get_jak_project_dir() / "custom_assets" /
                    game_version_names[g_game_version] / "scenery" / name;
  int w = 0, h = 0, n = 0;
  unsigned char* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
  if (!px) {
    return 0;
  }
  double sum[3] = {0.0, 0.0, 0.0};
  for (int i = 0; i < w * h; i++) {
    for (int k = 0; k < 3; k++) {
      sum[k] += px[i * 4 + k];
    }
  }
  for (int k = 0; k < 3; k++) {
    avg[k] = (float)(sum[k] / (255.0 * w * h));
  }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  stbi_image_free(px);
  return tex;
}

void EnhancedPost::free_ssao() {
  if (m_normal_fbo) {
    glDeleteFramebuffers(1, &m_normal_fbo);
    glDeleteTextures(1, &m_normal_tex);
    m_normal_fbo = m_normal_tex = 0;
  }
  if (m_albedo_fbo) {
    glDeleteFramebuffers(1, &m_albedo_fbo);
    glDeleteTextures(1, &m_albedo_tex);
    m_albedo_fbo = m_albedo_tex = 0;
  }
  if (m_depth_fbo) {
    glDeleteFramebuffers(1, &m_depth_fbo);
    glDeleteTextures(1, &m_depth_tex);
    glDeleteFramebuffers(2, m_ao_fbo);
    glDeleteTextures(2, m_ao_tex);
    glDeleteFramebuffers(2, m_rt_hist_fbo);
    glDeleteTextures(2, m_rt_hist_tex);
  }
  if (m_ao_hist_fbo[0]) {
    glDeleteFramebuffers(2, m_ao_hist_fbo);
    glDeleteTextures(2, m_ao_hist_tex);
    m_ao_hist_fbo[0] = m_ao_hist_fbo[1] = m_ao_hist_tex[0] = m_ao_hist_tex[1] = 0;
  }
  m_ao_hist_valid = false;
  m_rt_hist_fbo[0] = m_rt_hist_fbo[1] = m_rt_hist_tex[0] = m_rt_hist_tex[1] = 0;
  m_rt_hist_valid = false;
  m_depth_fbo = m_depth_tex = 0;
  m_ao_fbo[0] = m_ao_fbo[1] = m_ao_tex[0] = m_ao_tex[1] = 0;
  m_ssao_w = m_ssao_h = 0;
}

// the traced occlusion: the setting, or tuning.json "post.rt_ao" (0/1) over it in a running game
bool EnhancedPost::rt_ao_on() const {
  static const EnhancedTune t_rt_ao("post.rt_ao", -1.f);
  return t_rt_ao >= 0.f ? t_rt_ao > 0.5f : m_settings.rt_ao;
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

    glGenFramebuffers(1, &m_albedo_fbo);
    glGenTextures(1, &m_albedo_tex);
    glBindTexture(GL_TEXTURE_2D, m_albedo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_albedo_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_albedo_tex, 0);

    // (the occlusion and the characters' sun shadows at half the resolution a side: blurred anyway,
    // a quarter of the pixels to march)
    glGenFramebuffers(2, m_ao_fbo);
    glGenTextures(2, m_ao_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_ao_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (w + 1) / 2, (h + 1) / 2, 0, GL_RED, GL_UNSIGNED_BYTE,
                   nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ao_tex[i], 0);
    }
    glGenFramebuffers(2, m_rt_hist_fbo);
    glGenTextures(2, m_rt_hist_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_rt_hist_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, (w + 1) / 2, (h + 1) / 2, 0, GL_RG, GL_HALF_FLOAT,
                   nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_rt_hist_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ao_tex[0], 0);
      // (location 2: location 1 is ssao.temporal's history, attached here per frame when it's on)
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_rt_hist_tex[i],
                             0);
      const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2};
      glDrawBuffers(3, bufs);
    }
    m_rt_hist_valid = false;
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  enh_cpu_split("ssao-alloc");
  // copy (and for msaa, resolve) the depth buffer into a texture we can sample
  static const EnhancedTune t_probe_depth("debug.ssao_probe", 0.f);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
  if ((int)(float)t_probe_depth != 5) {  // (test 5: last frame's depth, for the CPU probe only)
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                      GL_NEAREST);
  }
  // and the level normals (msaa: averaged, renormalized in the shaders)
  enh_cpu_split("ssao-depth-blit");
  m_normals_ready = false;
  static const EnhancedTune t_probe_blits("debug.ssao_probe", 0.f);
  const bool probe_no_blits = (int)(float)t_probe_blits == 2;
  if (g_enhanced_normals.enabled && src_fbo == g_enhanced_normals.fbo && !probe_no_blits) {
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_normal_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    m_normals_ready = true;
  }
  // and their albedo (lights.albedo)
  m_albedo_ready = false;
  if (g_enhanced_normals.albedo && src_fbo == g_enhanced_normals.fbo && !probe_no_blits) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_albedo_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    m_albedo_ready = true;
  }

  enh_cpu_split("ssao-blits");
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  const int aw = (w + 1) / 2, ah = (h + 1) / 2;
  glViewport(0, 0, aw, ah);

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
    // (and the sun as seen in the sky: the glow, the clouds, the god rays)
    sun_world(world_sun, true);
    for (int col = 0; col < 3; col++) {
      m_sky_view[col] = 0.f;
      for (int row = 0; row < 3; row++) {
        m_sky_view[col] += world_sun[row] * cam.rot[row][col];
      }
      if (flip) {
        m_sky_view[col] = -m_sky_view[col];
      }
    }
    // kept with the shadow map when the characters aren't in it: these give them their shadows
    // (with them in it, a second shadow cast the game's own way: darker, doubled)
    const bool on = m_settings.sun_shadows && g_enhanced_sun.valid && !g_enhanced_casters.cast;
    glUniform4f(glGetUniformLocation(id, "sun"), sun[0], sun[1], sun[2],
                on ? m_settings.sun_shadow_strength : 0.f);
    glUniform1f(glGetUniformLocation(id, "sun_length"), m_settings.sun_shadow_length * kMeter);
  }
  glUniform1f(glGetUniformLocation(id, "close_up"), m_movie * kCloseUp);
  // traced occlusion
  // (test switch, what the traced occlusion costs, in one run: JAK_ENHANCED_AO_CYCLE=1 times the
  // pass for 3 s each with the ordered traversal, the unordered one and no tracing, twice, and logs
  // the averages)
  struct AoProbe {
    double start = -1.0, sum[3] = {};
    int count[3] = {};
    bool done = false;
    GLuint query = 0;
  };
  static AoProbe ao_probe;
  // (JAK_ENHANCED_AO_CYCLE=2: the traced occlusion's checkerboard on, off (all pixels a frame), none)
  static const int ao_cycle_kind = [] {
    const char* env = std::getenv("JAK_ENHANCED_AO_CYCLE");
    return env ? std::max(1, std::atoi(env)) : 0;
  }();
  static const bool ao_cycle = ao_cycle_kind > 0;
  int ao_phase = -1;
  if (ao_cycle && !ao_probe.done) {
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (ao_probe.start < 0.0) {
      ao_probe.start = now + 10.0;  // (the level settles first)
      glGenQueries(1, &ao_probe.query);
    }
    const double t = now - ao_probe.start;
    if (t >= 0.0 && t < 18.0) {
      ao_phase = (int)(t / 3.0) % 3;
      if (std::fmod(t, 3.0) < 0.5) {
        ao_phase += 3;  // (switched just now: not timed yet)
      }
    } else if (t >= 18.0) {
      ao_probe.done = true;
      const char* names[3] = {"ordered", ao_cycle_kind == 2 ? "every-pixel" : "unordered", "off"};
      for (int i = 0; i < 3; i++) {
        lg::info("[enhanced] ao cost: {:6.2f} ms rt {} ({} levels)",
                 ao_probe.count[i] ? ao_probe.sum[i] / ao_probe.count[i] : 0.0, names[i],
                 m_rt_count);
      }
    }
  }
  const int ao_mode = ao_phase >= 3 ? ao_phase - 3 : ao_phase;
  const bool rt_ao = rt_ao_on() && m_rt_ready && ao_mode != 2;
  // (tuning.json "post.rt_ao_temporal": 1 ray a pixel a frame, turned each frame, accumulated in a
  // reprojected history; 0: the old 6 rays a pixel every frame, no history)
  static const EnhancedTune t_rt_temporal("post.rt_ao_temporal", 1.f);
  static const EnhancedTune t_rt_rays("post.rt_ao_rays", 1.f);
  const bool rt_temporal = rt_ao && t_rt_temporal > 0.5f;
  glUniform1i(glGetUniformLocation(id, "rt_levels"), rt_ao ? m_rt_count : 0);
  glUniform1i(glGetUniformLocation(id, "rt_unordered"), ao_mode == 1 && ao_cycle_kind != 2);
  glUniform1i(glGetUniformLocation(id, "rt_every_pixel"), ao_mode == 1 && ao_cycle_kind == 2);
  glUniform1i(glGetUniformLocation(id, "rt_rays"),
              rt_temporal ? std::max(1, (int)t_rt_rays) : 6);
  glUniform1i(glGetUniformLocation(id, "rt_temporal"), rt_temporal ? 1 : 0);
  glUniform1f(glGetUniformLocation(id, "rt_frame"), (float)(m_frame % 4096));
  if (rt_temporal) {
    bool cut = !m_prev_camera.valid;
    float moved = 0.f;
    for (int c = 0; c < 3; c++) {
      const float d = cam.trans[c] - m_prev_camera.trans[c];
      moved += d * d;
    }
    cut = cut || std::sqrt(moved) > 8.f * kMeter;  // a cut or a teleport isn't motion
    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_2D, m_rt_hist_tex[1 - m_rt_hist_cur]);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T13"), 13);
    glUniform1i(glGetUniformLocation(id, "rt_history"), m_rt_hist_valid && !cut ? 1 : 0);
    glUniformMatrix3fv(glGetUniformLocation(id, "prev_view_to_world"), 1, GL_TRUE,
                       &m_prev_camera.rot[0][0]);
    glUniform3f(glGetUniformLocation(id, "prev_cam_pos"), m_prev_camera.trans[0],
                m_prev_camera.trans[1], m_prev_camera.trans[2]);
    glUniform2f(glGetUniformLocation(id, "prev_f"), m_prev_camera.fx, m_prev_fy);
  }
  if (rt_ao) {
    for (int i = 0; i < 2; i++) {
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_rt_buf[i]);
    }
    glUniform4iv(glGetUniformLocation(id, "rt_base"), m_rt_count, m_rt_base.data());
    glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
    glUniform3f(glGetUniformLocation(id, "rt_ao"), m_settings.rt_ao_length * kMeter,
                m_settings.rt_ao_strength, m_settings.rt_ao_distance * kMeter);
  }
  if (m_settings.ssao_debug && (debug_counter % 300) == 1) {
    lg::info("[enhanced] rt ao {} ready {} levels {}", m_settings.rt_ao, m_rt_ready, m_rt_count);
  }
  // temporal (ssao.temporal): half the taps a frame, averaged with the other half found last frame
  // at the same world point (the pass's own RG16F history, written as a second target)
  // (0.77 -> 0.55 ms at 2560x1600, Spargus 18h; the occlusion equal within 0.9/255)
  static const EnhancedTune kAoTemporal("ssao.temporal", 1.f);
  const bool ao_tmp = (float)kAoTemporal > 0.f && m_settings.ssao_intensity > 0.f;
  if (ao_tmp && !m_ao_hist_fbo[0]) {
    glGenFramebuffers(2, m_ao_hist_fbo);
    glGenTextures(2, m_ao_hist_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_ao_hist_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, aw, ah, 0, GL_RG, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao_hist_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ao_tex[0], 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_ao_hist_tex[i], 0);
      const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
      glDrawBuffers(2, bufs);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_ao_hist_valid = false;
  }
  if (!ao_tmp) {
    m_ao_hist_valid = false;
  }
  glUniform1i(glGetUniformLocation(id, "ao_temporal"), !ao_tmp ? 0 : m_ao_hist_valid ? 1 : 2);
  if (ao_tmp) {
    m_ao_frame++;
    glUniform1i(glGetUniformLocation(id, "ao_frame"), (int)(m_ao_frame & 1u));
    float to_prev[3][3];
    float offset[3];
    // (a cut or a teleport: no history)
    if (!enh_post::view_to_prev(cam, m_prev_camera, to_prev, offset)) {
      glUniform1i(glGetUniformLocation(id, "ao_temporal"), 2);
    }
    glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
    glUniform3f(glGetUniformLocation(id, "to_prev_offset"), offset[0], offset[1], offset[2]);
    glUniform2f(glGetUniformLocation(id, "prev_f"), m_prev_camera.fx, m_prev_fy);
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, m_ao_hist_tex[1 - m_ao_hist_cur]);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T12"), 12);
  }
  if (rt_temporal) {
    // (the traced occlusion's history at location 2; with ssao.temporal its history at 1 as well)
    glBindFramebuffer(GL_FRAMEBUFFER, m_rt_hist_fbo[m_rt_hist_cur]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                           ao_tmp ? m_ao_hist_tex[m_ao_hist_cur] : 0, 0);
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, ao_tmp ? (GLenum)GL_COLOR_ATTACHMENT1 : GL_NONE,
                            GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, bufs);
  } else if (ao_tmp) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao_hist_fbo[m_ao_hist_cur]);
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[0]);
  }
  if (ao_phase >= 0 && ao_phase < 3) {
    glBeginQuery(GL_TIME_ELAPSED, ao_probe.query);
  }
  enh_cpu_split("ssao-uniforms");
  // (test, tuning debug.ssao_probe: where the main draw's CPU goes. 1 skips it, 3 flushes before
  // it, 4 draws it twice; 2 skips the normal/albedo blits above)
  static const EnhancedTune t_probe("debug.ssao_probe", 0.f);
  const int probe = (int)(float)t_probe;
  if (probe == 3) {
    glFlush();
    enh_cpu_split("ssao-flush");
  }
  if (probe != 1) {
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  if (probe == 4) {
    enh_cpu_split("ssao-main1");
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  enh_cpu_split("ssao-main");
  if (ao_phase >= 0 && ao_phase < 3) {
    glEndQuery(GL_TIME_ELAPSED);
    GLuint64 ns = 0;
    glGetQueryObjectui64v(ao_probe.query, GL_QUERY_RESULT, &ns);
    ao_probe.sum[ao_phase] += ns / 1e6;
    ao_probe.count[ao_phase]++;
  }
  prof.add_draw_call();
  if (ao_tmp) {
    m_ao_hist_cur = 1 - m_ao_hist_cur;
    m_ao_hist_valid = true;
  }
  if (rt_temporal) {
    m_rt_hist_cur = 1 - m_rt_hist_cur;
  }
  m_rt_hist_valid = rt_temporal;

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
    glUniform2f(glGetUniformLocation(id, "step"), pass == 0 ? 1.f / aw : 0.f,
                pass == 0 ? 0.f : 1.f / ah);
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
  m_haze_shadow_ready = false;
  m_gi_ready = false;
  m_vol_ready = false;
  m_shadow_ready = false;
  g_enhanced_casters.cast = false;
  g_enhanced_casters.keep_game = false;
  if (m_settings.shadow_map && g_enhanced_camera.valid) {
    ENH_TIMED("enh-shadow-map", do_shadow_map(render_state, prof));
  }
  enh_cpu_split("shadow-map");
  m_chr_occ_ready = false;
  if (m_settings.shadow_map && m_settings.shadow_casters && g_enhanced_camera.valid) {
    ENH_TIMED("enh-chr-occ", do_chr_occlusion(render_state, prof));
  }
  enh_cpu_split("chr-occ");
  // the levels' ray tracing hierarchies (occlusion, reflections)
  ENH_TIMED("enh-rt-update", update_rt(render_state));
  enh_cpu_split("rt-update");
  // the open sky straight up around the camera (post.sky_map: the rain stays off what's roofed)
  ENH_TIMED("enh-sky-map", build_sky_map(screen_vao, render_state, prof));
  enh_cpu_split("sky-map");
  // the light probes around the camera (post.probes: the characters' sky and bounce light)
  ENH_TIMED("enh-probes", build_probes(screen_vao, render_state, prof));
  enh_cpu_split("probes");
  if (m_settings.ssao) {
    ENH_TIMED("enh-ssao", do_ssao(scene_fbo, w, h, screen_vao, render_state, prof));
    enh_cpu_split("ssao");
    // (the auto quality sets aside the volume first, then the indirect light, then the god rays)
    // (also for the lamps' and fires' light in the air, with no mist: volume.local_lights)
    // (the lights alone cost far less than the mist: kept to the auto quality's second level)
    if (m_settings.volume && m_ssao_ready &&
        ((m_fade_volume > 0.f && m_fog > 0.01f) ||
         (m_fade_gi > 0.f && m_fog <= 0.01f && volume_lights_on()))) {
      ENH_TIMED("enh-volume", do_volume(render_state, screen_vao, prof));
    }
    enh_cpu_split("volume");
    if (m_settings.gi && m_fade_gi > 0.f && m_ssao_ready) {
      ENH_TIMED("enh-gi", do_gi(scene_tex, screen_vao, render_state, prof));
    }
    enh_cpu_split("gi");
    if (((m_settings.shafts && m_fade_rays > 0.f) || m_grade.sun_haze > 0.f) && m_ssao_ready &&
        g_enhanced_sun.valid) {
      ENH_TIMED("enh-shafts", do_shafts(scene_tex, screen_vao, render_state, prof));
    }
  }
  enh_cpu_split("shafts");
  // set again by the next frame's background draws
  g_enhanced_camera.valid = false;
  if (!m_settings.bloom) {
    return;
  }
  enh_cpu_split("prep-misc");
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
  {
    // the light effects' own glow by day (enh_bloom_down: what the sprite buckets added)
    const bool fx = fx_ready();
    glUniform1i(glGetUniformLocation(down.id(), "tex_T1"), 1);
    glUniform1i(glGetUniformLocation(down.id(), "tex_T2"), 2);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, fx ? m_fx_tex[0] : m_black_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fx ? m_fx_tex[1] : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1f(glGetUniformLocation(down.id(), "fx_bloom"),
                fx ? std::clamp(1.f - m_night, 0.f, 1.f) : 0.f);
    glUniform1f(glGetUniformLocation(down.id(), "fx_mark"), fx && m_fx_marked ? 1.f : 0.f);
  }

  GLuint src = scene_tex;
  int src_w = w;
  int src_h = h;
  auto bloom_down = [&]() {
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
  };
  enh_cpu_split("bloom-setup");
  ENH_TIMED("enh-bloom-down", bloom_down());
  enh_cpu_split("bloom-down");

  // up: tent-filter each level and add it onto the next bigger one
  auto& up = render_state->shaders[ShaderId::ENH_BLOOM_UP];
  up.activate();
  glUniform1i(glGetUniformLocation(up.id(), "tex_T0"), 0);
  GLint u_up_texel = glGetUniformLocation(up.id(), "texel");
  glUniform1f(glGetUniformLocation(up.id(), "radius"), m_settings.bloom_radius);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);
  auto bloom_up = [&]() {
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
  };
  ENH_TIMED("enh-bloom-up", bloom_up());
  glDisable(GL_BLEND);
  glBindVertexArray(0);
}

// the sky hidden above Jak: five rays from his head, straight up and tilted 25 degrees four ways,
// through the levels' hierarchies (tfrag and tie: roofs, awnings, arches, trees), up to 40 m
float EnhancedPost::trace_cover(const float pos[3]) const {
  constexpr float kTilt = 0.4226f, kUp = 0.9063f;  // sin, cos of 25 degrees
  const float dirs[5][3] = {{0, 1, 0}, {kTilt, kUp, 0}, {-kTilt, kUp, 0}, {0, kUp, kTilt}, {0, kUp, -kTilt}};
  const float o[3] = {pos[0], pos[1] + 1.8f * 4096.f, pos[2]};
  const float tmax = 40.f * 4096.f;
  int hits = 0;
  for (const auto& d : dirs) {
    float inv[3];
    for (int k = 0; k < 3; k++) {
      inv[k] = 1.f / (std::abs(d[k]) < 1e-7f ? 1e-7f : d[k]);
    }
    bool hit = false;
    for (const auto& [id, level] : m_rt_levels) {
      const EnhancedRtLevel* rt = level.cpu.get();
      if (!rt || rt->nodes.empty() || hit) {
        continue;
      }
      u32 stack[64];
      int sp = 0;
      stack[sp++] = 0;
      while (sp > 0 && !hit) {
        const auto& n = rt->nodes[stack[--sp]];
        const u32 ni = (u32)(&n - rt->nodes.data());
        float tn = 0.f, tf = tmax;
        for (int k = 0; k < 3; k++) {
          float t0 = (n.min[k] - o[k]) * inv[k], t1 = (n.max[k] - o[k]) * inv[k];
          tn = std::max(tn, std::min(t0, t1));
          tf = std::min(tf, std::max(t0, t1));
        }
        if (tn > tf) {
          continue;
        }
        if (n.b > 0) {
          for (u32 i = n.a; i < n.a + n.b && !hit; i++) {
            const auto& tri = rt->tris[i];
            const float* e1 = tri.e1;
            const float* e2 = tri.e2;
            const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                                 d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::abs(det) < 1e-4f) {
              continue;
            }
            const float id = 1.f / det;
            const float tv[3] = {o[0] - tri.v0[0], o[1] - tri.v0[1], o[2] - tri.v0[2]};
            const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * id;
            if (u < 0.f || u > 1.f) {
              continue;
            }
            const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                                 tv[0] * e1[1] - tv[1] * e1[0]};
            const float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * id;
            if (v < 0.f || u + v > 1.f) {
              continue;
            }
            const float hit_t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * id;
            hit = hit_t > 0.f && hit_t < tmax;
          }
        } else if (sp < 62) {
          stack[sp++] = n.a;     // the right child
          stack[sp++] = ni + 1;  // the left child, next
        }
      }
    }
    hits += hit;
  }
  return hits / 5.f;
}

void EnhancedPost::update_rt(SharedRenderState* render_state) {
  m_rt_ready = false;
  if (!render_state->loader) {
    return;
  }
  // the hierarchies are taken from the loader whatever the settings (the roof over Jak is traced on
  // the CPU); on the GPU only for the ray-traced reflections and occlusion
  const bool on_gpu = m_settings.ray_traced > 0 || rt_ao_on() || sky_map_on() || probes_on();
  auto levels = render_state->loader->get_in_use_levels();
  std::vector<u64> in_use;
  for (auto* lev : levels) {
    if (!m_rt_levels.count(lev->load_id)) {
      // the hierarchy the loader built for it (once)
      m_rt_levels[lev->load_id].cpu = enhanced_take_rt(lev->level->level_name);
    }
    auto& gpu = m_rt_levels[lev->load_id];
    if (on_gpu && gpu.cpu && !gpu.buf[0]) {
      const auto& rt = gpu.cpu;
      const void* data[3] = {rt->nodes.data(), rt->tris.data(), rt->palettes.data()};
      gpu.size[0] = rt->nodes.size() * sizeof(EnhancedRtLevel::Node);
      gpu.size[1] = rt->tris.size() * sizeof(EnhancedRtLevel::Tri);
      gpu.size[2] = std::max<size_t>(rt->palettes.size(), 1) * sizeof(u32);
      u32 none = 0;
      if (rt->palettes.empty()) {
        data[2] = &none;
      }
      glGenBuffers(3, gpu.buf);
      for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_COPY_READ_BUFFER, gpu.buf[i]);
        glBufferData(GL_COPY_READ_BUFFER, gpu.size[i], data[i], GL_STATIC_DRAW);
      }
      glBindBuffer(GL_COPY_READ_BUFFER, 0);
      if (rt->layer_count > 0) {
        gpu.layers = rt->layer_count;
        glGenTextures(1, &gpu.tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, gpu.tex);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kRtLayerSize, kRtLayerSize, gpu.layers);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kRtLayerSize, kRtLayerSize, gpu.layers,
                        GL_RGBA, GL_UNSIGNED_BYTE, rt->layers.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
      }
      // the CPU keeps only the geometry
      std::vector<u32>().swap(rt->palettes);
      std::vector<u32>().swap(rt->layers);
    }
    if (gpu.buf[0] && in_use.size() < 16) {
      in_use.push_back(lev->load_id);
    }
  }
  // forget the levels that went away
  for (auto it = m_rt_levels.begin(); it != m_rt_levels.end();) {
    bool used = false;
    for (auto* lev : levels) {
      used |= lev->load_id == it->first;
    }
    if (!used) {
      if (it->second.buf[0]) {
        glDeleteBuffers(3, it->second.buf);
      }
      if (it->second.tex) {
        glDeleteTextures(1, &it->second.tex);
      }
      it = m_rt_levels.erase(it);
    } else {
      ++it;
    }
  }
  // the roof over Jak, a few times a second
  {
    static int frame = 0;
    if (++frame % 6 == 0 && g_enhanced_player.valid) {
      const float cover = trace_cover(g_enhanced_player.pos);
      // test switch: JAK_ENHANCED_COVER_LOG=1 logs each change
      static const bool log = std::getenv("JAK_ENHANCED_COVER_LOG") != nullptr;
      if (log && cover != m_cover_now) {
        lg::info("[enhanced] roof over Jak {:.1f} at {:.0f} {:.0f} {:.0f} m", cover,
                 g_enhanced_player.pos[0] / 4096.f, g_enhanced_player.pos[1] / 4096.f,
                 g_enhanced_player.pos[2] / 4096.f);
      }
      m_cover_now = cover;
    }
  }
  if (!on_gpu || in_use.empty()) {
    m_rt_count = 0;
    m_rt_in_use.clear();  // (switched back on: the combined buffers built again, not count 0)
    return;
  }
  if (in_use != m_rt_in_use) {
    // one buffer of each kind for all of them (copied on the GPU)
    m_rt_in_use = in_use;
    u32 total[3] = {0, 0, 0};
    for (auto id : in_use) {
      for (int i = 0; i < 3; i++) {
        total[i] += m_rt_levels[id].size[i];
      }
    }
    if (!m_rt_buf[0]) {
      glGenBuffers(3, m_rt_buf);
    }
    for (int i = 0; i < 3; i++) {
      glBindBuffer(GL_COPY_WRITE_BUFFER, m_rt_buf[i]);
      glBufferData(GL_COPY_WRITE_BUFFER, total[i], nullptr, GL_STATIC_DRAW);
    }
    // and one texture array (with mipmaps, for the far hits)
    GLint max_layers = 2048;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    int layers = 0;
    for (auto id : in_use) {
      layers += m_rt_levels[id].layers;
    }
    layers = std::clamp(layers, 1, (int)max_layers);
    if (m_rt_tex) {
      glDeleteTextures(1, &m_rt_tex);
    }
    glGenTextures(1, &m_rt_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_rt_tex);
    int mips = 1;
    while ((kRtLayerSize >> mips) > 0) {
      mips++;
    }
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, mips, GL_RGBA8, kRtLayerSize, kRtLayerSize, layers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    int layer_at = 0;
    u32 offset[3] = {0, 0, 0};
    m_rt_count = 0;
    for (auto id : in_use) {
      const auto& gpu = m_rt_levels[id];
      auto* base = &m_rt_base[m_rt_count * 4];
      base[0] = offset[0] / sizeof(EnhancedRtLevel::Node);
      base[1] = offset[1] / sizeof(EnhancedRtLevel::Tri);
      base[2] = offset[2] / sizeof(u32) / 8;
      base[3] = layer_at;
      if (gpu.tex && layer_at + gpu.layers <= layers) {
        glCopyImageSubData(gpu.tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, m_rt_tex, GL_TEXTURE_2D_ARRAY,
                           0, 0, 0, layer_at, kRtLayerSize, kRtLayerSize, gpu.layers);
        layer_at += gpu.layers;
      } else {
        base[3] = -1;  // no textures: mean colors
      }
      for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_COPY_READ_BUFFER, gpu.buf[i]);
        glBindBuffer(GL_COPY_WRITE_BUFFER, m_rt_buf[i]);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, offset[i], gpu.size[i]);
        offset[i] += gpu.size[i];
      }
      m_rt_count++;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    lg::info("[enhanced] ray tracing: {} levels, {:.0f} MB, {} textures", m_rt_count,
             (total[0] + total[1] + total[2]) / 1e6, layer_at);
  }
  m_rt_ready = m_rt_count > 0;
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
  glBindTexture(GL_TEXTURE_2D, m_shafts_ready || m_haze_shadow_ready ? m_shaft_tex : m_black_tex);
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
  // ray-traced reflections: the levels' hierarchies (update_rt), and the time-of-day weights
  const bool rt = m_settings.ray_traced > 0 && m_rt_ready && g_enhanced_tod.valid;
  glUniform1i(glGetUniformLocation(id, "rt_mode"), rt ? m_settings.ray_traced : 0);
  if (rt) {
    for (int i = 0; i < 3; i++) {
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_rt_buf[i]);
    }
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_rt_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T12"), 12);
    glUniform1i(glGetUniformLocation(id, "rt_levels"), m_rt_count);
    glUniform4iv(glGetUniformLocation(id, "rt_base"), m_rt_count, m_rt_base.data());
    // the same decoding as interp_time_of_day: palette p, channel ch, 64 = 1
    float tod[8 * 4];
    for (int p = 0; p < 8; p++) {
      for (int ch = 0; ch < 4; ch++) {
        u32 word = (u32)g_enhanced_tod.itimes[p / 2][(p % 2) * 2 + ch / 2];
        u32 hw = (ch % 2) ? (word >> 16) : word;
        tod[p * 4 + ch] = (hw & 0xff) / 64.f;
      }
    }
    glUniform4fv(glGetUniformLocation(id, "rt_tod"), 8, tod);
  }
  glUniform1i(glGetUniformLocation(id, "tex_T3"), 3);
  glUniform1i(glGetUniformLocation(id, "tex_T4"), 4);
  glUniform1i(glGetUniformLocation(id, "tex_T7"), 7);
  glUniform1i(glGetUniformLocation(id, "tex_T8"), 8);
  glUniform1i(glGetUniformLocation(id, "tex_T9"), 9);
  glUniform1f(glGetUniformLocation(id, "shadow_strength"),
              // (a sharp dark shape from the moon would stand out on the lamps' light: faint at night)
              m_shadow_ready ? m_settings.shadow_strength * g_enhanced_sun.shadow * (1.f - 0.7f * m_night) : 0.f);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  glUniform1i(glGetUniformLocation(id, "volume"), m_vol_ready);
  // (the volume holds only the lights' glow in the air, no mist: not dimmed by the night)
  glUniform1f(glGetUniformLocation(id, "vol_lights_only"), m_vol_ready && m_fog <= 0.01f ? 1.f : 0.f);
  glUniform1f(glGetUniformLocation(id, "volume_fade"),
              m_vol_ready && m_fog <= 0.01f ? m_fade_gi : m_fade_volume);
  glUniform2f(glGetUniformLocation(id, "vol_texel"), m_vol_w ? 1.f / m_vol_w : 0.f,
              m_vol_h ? 1.f / m_vol_h : 0.f);
  glUniform1f(glGetUniformLocation(id, "gi_strength"),
              m_gi_ready ? m_settings.gi_strength * game_gi_scale() * m_fade_gi : 0.f);
  glUniform1f(glGetUniformLocation(id, "shafts_strength"),
              m_shafts_ready ? m_settings.shafts_strength * (1.f + 1.2f * m_clear) * m_fade_rays : 0.f);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  const bool dof = m_settings.dof && m_ssao_ready;
  glUniform3f(glGetUniformLocation(id, "dof"), m_settings.dof_start * kMeter,
              m_settings.dof_end * kMeter, dof ? m_settings.dof_radius * h / 1080.f : 0.f);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / w, 1.f / h);
  glUniform1f(glGetUniformLocation(id, "bloom_strength"),
              m_settings.bloom ? m_settings.bloom_strength : 0.f);
  // the area's grade on top of the settings (update_scene)
  glUniform1f(glGetUniformLocation(id, "saturation"), m_settings.saturation * m_grade.saturation);
  glUniform1f(glGetUniformLocation(id, "contrast"), m_settings.contrast + m_grade.contrast);
  glUniform1f(glGetUniformLocation(id, "warmth"), m_settings.warmth + m_grade.warmth);
  glUniform1f(glGetUniformLocation(id, "sand"), m_grade.sand);
  glUniform1f(glGetUniformLocation(id, "sun_haze"), m_ssao_ready ? m_grade.sun_haze : 0.f);
  {
    // the bright hours (10-14.5, eased in from 7:30 and out by 17): never brighter in the morning
    // or the afternoon than at noon
    const float hr = m_last_hour < 0.f ? 12.f : m_last_hour;
    auto ease = [](float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); };
    // the morning (7-10, eased in from 6 and out by 11:30), where the area has a golden look: a
    // clear sky, pink toward the low sun, cream sand (the bright hours wait for it)
    // (from the dawn: its sky the morning's, pink toward the sun)
    // (in such an area the morning's look lasts through noon, to the late afternoon's amber: its
    // noon, a pale glare, read dull)
    const float golden_area = std::min(m_grade.golden * 2.f, 1.f);
    const float morning_end = 10.f + 4.5f * golden_area;
    const float morning = ease(hr - 5.f) * (1.f - ease((hr - morning_end) / 1.5f)) * golden_area;
    const float noon = ease((hr - 7.5f) / 2.5f) * (1.f - ease((hr - 14.5f) / 2.5f)) * (1.f - morning) *
                       (1.f - golden_area);
    glUniform1f(glGetUniformLocation(id, "noon"), noon);
    glUniform1f(glGetUniformLocation(id, "morning"), morning);
    // the desert's day (from the dawn at 5, half of it still at the sunset at 18): the morning's
    // colors, its blue sky, warm sand and lit rock, all day long; the pink and peach the morning's own
    const float day_look = ease(hr - 5.f) * (1.f - ease((hr - 17.f) / 2.f)) *
                           std::min(m_grade.golden * 2.f, 1.f);
    glUniform1f(glGetUniformLocation(id, "day_look"), day_look);
    // the same hours for the area's day sky (any outdoor area: its clear blue, the morning's pink
    // toward the sun, the clouds lit to match, the shade's open-sky blue)
    const float sky_k = std::max(std::min(m_grade.golden * 2.f, 1.f), m_grade.day_sky);
    glUniform1f(glGetUniformLocation(id, "sky_morning"),
                ease(hr - 5.f) * (1.f - ease((hr - morning_end) / 1.5f)) * sky_k);
    glUniform1f(glGetUniformLocation(id, "sky_day"),
                ease(hr - 5.f) * (1.f - ease((hr - 17.f) / 2.f)) * sky_k);
    glUniform1f(glGetUniformLocation(id, "rock_sun"), m_ssao_ready ? m_grade.rock_sun : 0.f);
  glUniform1f(glGetUniformLocation(id, "wall_sun"), m_ssao_ready ? m_grade.wall_sun : 0.f);
    // the late afternoon (16-19, eased in from 14:30 and out by 20, with the sun down: over by 19 the plain jumped from amber to a noon white): amber light
    const float golden = ease((hr - 14.5f) / 1.5f) * (1.f - ease((hr - 19.f) / 1.f));
    glUniform1f(glGetUniformLocation(id, "golden"), golden * m_grade.golden);
    // the sun going down by the clock (16:30-18:30, out by 20:48), whatever the area's sun height
    glUniform1f(glGetUniformLocation(id, "dusk"), m_dusk);
  }
  glUniform1f(glGetUniformLocation(id, "haze_shadow"), m_haze_shadow_ready ? 1.f : 0.f);
  // under a roof (world.json "enclosed", indoor zones): the sky seen through its windows stays the game's
  glUniform1f(glGetUniformLocation(id, "enclosed"), m_enclosed ? 1.f : 0.f);
  // the rock and ground pictures (custom_assets/<game>/scenery/rock_detail.png, ground_detail.png),
  // loaded on first use, with their average colors
  // (one replaced in the editor: all of them read again)
  if (m_terrain_version != g_enhanced_terrain.version.load()) {
    m_terrain_version = g_enhanced_terrain.version.load();
    for (GLuint* t : {&m_rock_tex, &m_ground_tex, &m_sand_tex[0], &m_sand_tex[1], &m_sand_tex[2]}) {
      if (*t) {
        glDeleteTextures(1, t);
        *t = 0;
      }
    }
    m_rock_loaded = false;
  }
  glUniform1f(glGetUniformLocation(id, "anti_tile"), g_enhanced_terrain.anti_tile.load());
  if (!m_rock_loaded) {
    m_rock_loaded = true;
    float rgb[3];
    m_rock_tex = load_tiling_picture("rock_detail.png", rgb);
    m_rock_avg = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    m_ground_tex = load_tiling_picture("ground_detail.png", m_ground_avg);
    // (the sand's: fine rippled, coarse with gravel, a cracked crust)
    const char* sands[3] = {"sand_fine.png", "sand_coarse.png", "sand_crust.png"};
    for (int i = 0; i < 3; i++) {
      m_sand_tex[i] = load_tiling_picture(sands[i], m_sand_avg[i]);
    }
  }
  glActiveTexture(GL_TEXTURE10);
  glBindTexture(GL_TEXTURE_2D, m_rock_tex ? m_rock_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE16);
  glBindTexture(GL_TEXTURE_2D, m_ground_tex ? m_ground_tex : m_black_tex);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE17 + i);
    glBindTexture(GL_TEXTURE_2D, m_sand_tex[i] ? m_sand_tex[i] : m_black_tex);
  }
  // the composite's gradient noise (gnoise) as a picture: 64 of its cells a side, 16 texels a cell,
  // read bilinearly on unit 20 (noise_lut 1) instead of four hashes a call; the same noise for
  // world points 0-64 cells from the origin, repeating past them. Tuning "post.noise_lut" 0: hashed
  // (desert 16h, 2560x1600: composite 12.37 -> 12.25 ms, pictures equal within TAA noise)
  {
    static const EnhancedTune t_noise_lut("post.noise_lut", 1.f);
    const bool use_lut = (float)t_noise_lut > 0.5f;
    if (use_lut && !m_noise_tex) {
      constexpr int kCells = 64, kPer = 16, kSize = kCells * kPer;
      auto fract = [](float v) { return v - std::floor(v); };
      auto hash2 = [&](int ix, int iy, float& hx, float& hy) {
        const float qx = (float)(((ix % kCells) + kCells) % kCells);
        const float qy = (float)(((iy % kCells) + kCells) % kCells);
        float px = fract(qx * 0.1031f), py = fract(qy * 0.1030f), pz = fract(qx * 0.0973f);
        const float dd = px * (py + 33.33f) + py * (pz + 33.33f) + pz * (px + 33.33f);
        px += dd;
        py += dd;
        pz += dd;
        hx = -1.f + 2.f * fract((px + py) * pz);
        hy = -1.f + 2.f * fract((px + pz) * py);
      };
      std::vector<float> data((size_t)kSize * kSize);
      for (int ty = 0; ty < kSize; ty++) {
        for (int tx = 0; tx < kSize; tx++) {
          const int ix = tx / kPer, iy = ty / kPer;
          const float fx = (float)(tx % kPer) / kPer, fy = (float)(ty % kPer) / kPer;
          const float ux = fx * fx * (3.f - 2.f * fx), uy = fy * fy * (3.f - 2.f * fy);
          float hx, hy;
          hash2(ix, iy, hx, hy);
          const float a = hx * fx + hy * fy;
          hash2(ix + 1, iy, hx, hy);
          const float b = hx * (fx - 1.f) + hy * fy;
          hash2(ix, iy + 1, hx, hy);
          const float c = hx * fx + hy * (fy - 1.f);
          hash2(ix + 1, iy + 1, hx, hy);
          const float d = hx * (fx - 1.f) + hy * (fy - 1.f);
          const float ab = a + (b - a) * ux, cd = c + (d - c) * ux;
          data[(size_t)ty * kSize + tx] = ab + (cd - ab) * uy;
        }
      }
      glGenTextures(1, &m_noise_tex);
      glBindTexture(GL_TEXTURE_2D, m_noise_tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, kSize, kSize, 0, GL_RED, GL_FLOAT, data.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glActiveTexture(GL_TEXTURE20);
    glBindTexture(GL_TEXTURE_2D, m_noise_tex ? m_noise_tex : m_black_tex);
    glUniform1i(glGetUniformLocation(id, "tex_T20"), 20);
    glUniform1f(glGetUniformLocation(id, "noise_lut"), use_lut && m_noise_tex ? 1.f : 0.f);
  }
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T10"), 10);
  glUniform1i(glGetUniformLocation(id, "tex_T16"), 16);
  glUniform1i(glGetUniformLocation(id, "tex_T17"), 17);
  glUniform1i(glGetUniformLocation(id, "tex_T18"), 18);
  glUniform1i(glGetUniformLocation(id, "tex_T19"), 19);
  {
    // the ground under a dense grass field drawn this frame (or the last)
    const auto& gg = g_enhanced_grass_ground;
    const bool on = gg.frame + 2 >= render_state->frame_idx && gg.frame > 0;
    glUniform4f(glGetUniformLocation(id, "grass_ground"), gg.zone[0], gg.zone[1], gg.zone[2],
                on ? gg.zone[3] : 0.f);
    glUniform1f(glGetUniformLocation(id, "grass_water"), gg.water);
    glUniform3f(glGetUniformLocation(id, "grass_field"), gg.field[0], gg.field[1], gg.field[2]);
    glUniform1i(glGetUniformLocation(id, "tex_T21"), 21);
    glActiveTexture(GL_TEXTURE21);
    glBindTexture(GL_TEXTURE_2D, on && gg.tex ? gg.tex : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
  }
  glUniform1f(glGetUniformLocation(id, "sand_set"),
              m_sand_tex[0] && m_sand_tex[1] && m_sand_tex[2] ? 1.f : 0.f);
  glUniform3fv(glGetUniformLocation(id, "sand_set_avg"), 3, &m_sand_avg[0][0]);
  glUniform1f(glGetUniformLocation(id, "rock_detail"),
              m_rock_tex && m_normals_ready ? m_grade.rock_detail : 0.f);
  glUniform1f(glGetUniformLocation(id, "rock_detail_avg"), m_rock_avg);
  glUniform1f(glGetUniformLocation(id, "ground_detail"), m_ground_tex ? 1.f : 0.f);
  glUniform3fv(glGetUniformLocation(id, "ground_detail_avg"), 1, m_ground_avg);
  glUniform3fv(glGetUniformLocation(id, "grade_lift"), 1, m_grade.lift);
  glUniform3fv(glGetUniformLocation(id, "grade_gain"), 1, m_grade.gain);
  // soft shadows: the shadow map again, read as plain depths for the blocker search
  if (!m_shadow_raw_sampler) {
    glGenSamplers(1, &m_shadow_raw_sampler);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  }
  glActiveTexture(GL_TEXTURE13);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready ? m_shadow_tex : m_no_shadow_tex);
  glBindSampler(13, m_shadow_raw_sampler);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T13"), 13);
  // the far map (unit 22): where the near one ends
  glActiveTexture(GL_TEXTURE22);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready && m_shadow_far_ready ? m_shadow_far_tex : m_no_shadow_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T22"), 22);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp_far"), 1, GL_TRUE, m_light_mvp_far);
  glUniform1f(glGetUniformLocation(id, "shadow_far_on"), m_shadow_ready && m_shadow_far_ready ? 1.f : 0.f);
  // the map's depth span (2 x shadow.near_depth_m, do_shadow_map), its side 2 x range; the far
  // map's depth span for its bias in meters (it was a constant: 1.5 m along the light at 6 km)
  glUniform1f(glGetUniformLocation(id, "shadow_far_span"), m_shadow_far_span_m);
  // the middle map (unit 26): between the near and the far one
  const bool mid = m_shadow_ready && m_shadow_mid_ready;
  glActiveTexture(GL_TEXTURE26);
  glBindTexture(GL_TEXTURE_2D, mid ? m_shadow_mid_tex : m_no_shadow_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T26"), 26);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp_mid"), 1, GL_TRUE, m_light_mvp_mid);
  glUniform1f(glGetUniformLocation(id, "shadow_mid_on"), mid ? 1.f : 0.f);
  glUniform1f(glGetUniformLocation(id, "shadow_mid_span"), m_shadow_mid_span_m);
  // the level albedo (unit 27, lights.albedo)
  glActiveTexture(GL_TEXTURE27);
  glBindTexture(GL_TEXTURE_2D, m_albedo_ready ? m_albedo_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T27"), 27);
  glUniform1f(glGetUniformLocation(id, "albedo_on"), m_albedo_ready ? 1.f : 0.f);
  // the depth pyramid (unit 29, post.hiz)
  glActiveTexture(GL_TEXTURE29);
  glBindTexture(GL_TEXTURE_2D, m_hiz_ready ? m_hiz_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T29"), 29);
  glUniform1i(glGetUniformLocation(id, "hiz_levels"), m_hiz_ready ? m_hiz_levels : 0);
  glUniform2f(glGetUniformLocation(id, "hiz_size"), (float)m_hiz_w, (float)m_hiz_h);
  glUniform1i(glGetUniformLocation(id, "hiz_surf"), m_hiz_ready && m_hiz_surf ? 1 : 0);
  // the sky map (unit 30, post.sky_map)
  {
    const bool sky = m_sky_valid && sky_map_on();
    glActiveTexture(GL_TEXTURE30);
    glBindTexture(GL_TEXTURE_2D, sky ? m_skymap_tex[m_sky_front] : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T30"), 30);
    glUniform4f(glGetUniformLocation(id, "sky_map"), m_sky_origin[0], m_sky_origin[1],
                kSkyMapCell * kMeter, sky ? (float)kSkyMapSize : 0.f);
  }
  // the light probes (unit 32, post.probes)
  {
    const bool pr = m_probe_valid && probes_on();
    glActiveTexture(GL_TEXTURE0 + 32);
    glBindTexture(GL_TEXTURE_2D, pr ? m_probe_tex[m_probe_front] : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T32"), 32);
    const float pcell = kProbeCell * kMeter;
    glUniform4f(glGetUniformLocation(id, "probe_grid"), m_probe_cell[0] * pcell,
                m_probe_cell[1] * pcell, m_probe_cell[2] * pcell, pr ? pcell : 0.f);
  }
  glUniform4f(glGetUniformLocation(id, "shadow_params"), m_shadow_near_span_m, 2.f * m_settings.shadow_range,
              m_settings.shadow_softness,
              m_settings.soft_shadows && m_auto_level < 3 ? 1.f : 0.f);
  {
    // the sun for the clouds, day or night (view space), and how high it stands (the camera's up:
    // last frame's, near enough)
    float len = std::sqrt(m_sky_view[0] * m_sky_view[0] + m_sky_view[1] * m_sky_view[1] +
                          m_sky_view[2] * m_sky_view[2]);
    float up = len > 0.f ? (m_sky_view[0] * m_prev_camera.rot[1][0] +
                            m_sky_view[1] * m_prev_camera.rot[1][1] +
                            m_sky_view[2] * m_prev_camera.rot[1][2]) / len
                         : 0.5f;
    float sv[3] = {len > 0.f ? m_sky_view[0] / len : 0.f, len > 0.f ? m_sky_view[1] / len : 0.f,
                   len > 0.f ? m_sky_view[2] / len : 1.f};
    // test: the sun low behind the camera (the rainbow ahead)
    static const bool rainbow_test = std::getenv("JAK_ENHANCED_RAINBOW_TEST") != nullptr;
    if (rainbow_test) {
      for (int i = 0; i < 3; i++) {
        sv[i] = m_prev_camera.rot[1][i] * 0.34f - (i == 2 ? 0.94f : 0.f);
      }
      up = 0.34f;
    }
    // test: the sun (or moon) straight ahead, JAK_ENHANCED_SKY_TEST=<sine of its height> (sunsets,
    // the moon's phases)
    static const char* sky_test = std::getenv("JAK_ENHANCED_SKY_TEST");
    if (sky_test) {
      up = (float)std::atof(sky_test);
      const float flat = std::sqrt(std::max(0.f, 1.f - up * up));
      // (the camera's heading in the world, level, raised by up; then into view space)
      const auto& r = m_prev_camera.rot;
      const float hl = std::max(1e-4f, std::sqrt(r[0][2] * r[0][2] + r[2][2] * r[2][2]));
      const float w[3] = {r[0][2] / hl * flat, up, r[2][2] / hl * flat};
      for (int col = 0; col < 3; col++) {
        sv[col] = w[0] * r[0][col] + w[1] * r[1][col] + w[2] * r[2][col];
      }
    }
    glUniform4f(glGetUniformLocation(id, "sky_sun"), sv[0], sv[1], sv[2], up);
  }
  // (no sky, clouds or weather without a 3D scene: loading screens, menus)
  // (test switch: JAK_ENHANCED_CLOUD_TIME=<s> holds the clouds still, shots compare alike)
  static const char* cloud_time = std::getenv("JAK_ENHANCED_CLOUD_TIME");
  glUniform4f(glGetUniformLocation(id, "clouds"),
              m_settings.clouds && m_ssao_ready ? m_settings.cloud_amount : 0.f, m_grade.clouds,
              cloud_time ? (float)std::atof(cloud_time) : (float)std::fmod(m_time, 10000.0), m_night);
  const float on = m_ssao_ready ? 1.f : 0.f;
  glUniform1f(glGetUniformLocation(id, "city_glow"), m_city ? m_night * on : 0.f);
  glUniform1f(glGetUniformLocation(id, "pale_ground"), m_pale_ground ? 1.f : 0.f);
  glUniform4f(glGetUniformLocation(id, "weather"), m_rain * on, m_dust * on,
              (float)std::fmod(m_time, 1000.0), m_wet * on);
  glUniform1f(glGetUniformLocation(id, "puddle_level"), m_puddle * on);
  glUniform4f(glGetUniformLocation(id, "weather2"), m_storm * on, m_mist * on, m_clear * on,
              m_flash * on);
  {
    const float wl = std::max(1e-3f, std::sqrt(m_wind[0] * m_wind[0] + m_wind[1] * m_wind[1]));
    glUniform4f(glGetUniformLocation(id, "dust_wind"), m_wind[0] / wl, m_wind[1] / wl,
                (float)m_drift[0], (float)m_drift[1]);
  }
  {
    // the evening's sky: how many of the stars show, the moon's lit part, the eclipse, the red sunset
    static const float kStars[kSkyNights] = {1.f, 0.18f, 0.4f, 0.55f, 1.f};
    static const float kLit[kSkyNights] = {1.f, 1.f, 1.f, 0.82f, 1.f};
    glUniform4f(glGetUniformLocation(id, "sky_kind"), kStars[m_night_kind], kLit[m_night_kind],
                m_night_kind == kRedEclipse ? 1.f : 0.f, m_red_dusk * on);
  }
  glUniform1f(glGetUniformLocation(id, "sunny"), m_sunny * on);
  glUniform1f(glGetUniformLocation(id, "sheet"), m_sheet * on);
  glUniform2f(glGetUniformLocation(id, "snow_heat"), m_snow * on, m_heat * on);
  glUniform1f(glGetUniformLocation(id, "snow_cover"), m_snow_cover * m_settings.snow_cover * on);
  glUniform4f(glGetUniformLocation(id, "bolt"), m_bolt[0], m_bolt[1], m_bolt[2],
              m_bolt[3] * m_storm * on);
  {
    // a bolt to the ground: the sines of the elevation of its foot and of the cloud base 600 m
    // above, its distance (game units)
    float st[4] = {0.f, 0.f, 0.f, 0.f};
    if (m_strike_age >= 0.f && m_prev_camera.valid) {
      const float dx = m_strike_pos[0] - m_prev_camera.trans[0];
      const float dy = m_strike_pos[1] - m_prev_camera.trans[1];
      const float dz = m_strike_pos[2] - m_prev_camera.trans[2];
      const float flat = std::max(std::sqrt(dx * dx + dz * dz), kMeter);
      const float top = dy + 600.f * kMeter;
      st[0] = dy / std::sqrt(flat * flat + dy * dy);
      st[1] = top / std::sqrt(flat * flat + top * top);
      st[2] = std::sqrt(flat * flat + dy * dy);
      st[3] = 1.f;
    }
    glUniform4f(glGetUniformLocation(id, "strike"), st[0], st[1], st[2], st[3] * on);
  }
  glUniform1f(glGetUniformLocation(id, "eye_adapt"), m_adapt);
  {
    // the player's brightness (menu "Brightness"), by the hour: night (19-21 in, 5-7 out), dawn and
    // dusk around it, day the rest. A test switch steps the presets:
    // JAK_ENHANCED_BRIGHT_SCRIPT=<s>:<preset>,... (seconds since the first composite)
    static const std::vector<std::pair<double, int>> script = [] {
      std::vector<std::pair<double, int>> steps;
      if (const char* e = std::getenv("JAK_ENHANCED_BRIGHT_SCRIPT")) {
        std::string str(e);
        for (size_t pos = 0; pos < str.size();) {
          size_t end = str.find(',', pos);
          std::string item = str.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
          size_t colon = item.find(':');
          if (colon != std::string::npos) {
            steps.emplace_back(std::atof(item.c_str()), std::atoi(item.c_str() + colon + 1));
          }
          pos = end == std::string::npos ? str.size() : end + 1;
        }
      }
      return steps;
    }();
    static double script_t0 = -1;
    static size_t script_next = 0;
    if (script_t0 < 0) {
      script_t0 = m_time;
    }
    while (script_next < script.size() && m_time - script_t0 >= script[script_next].first) {
      apply_brightness(m_settings, script[script_next].second);
      lg::info("[enhanced] brightness preset {} ({})", script[script_next].second,
               kBrightness[std::clamp(script[script_next].second, 0, kBrightnessPresets - 1)].name);
      script_next++;
    }
    const auto& s = m_settings;
    const float hr = m_last_hour < 0.f ? 12.f : m_last_hour;
    auto ramp = [](float a, float b, float x) { return std::clamp((x - a) / (b - a), 0.f, 1.f); };
    const float night = m_night;
    const float twilight = (1.f - night) * std::max(ramp(16.5f, 18.5f, hr) * (1.f - ramp(20.f, 21.5f, hr)),
                                                    ramp(4.5f, 5.5f, hr) * (1.f - ramp(7.f, 8.5f, hr)));
    const float day = std::max(0.f, 1.f - night - twilight);
    // (as an offset from 1: neutral sliders give exactly 0, and the shader skips the block)
    const float gain = day * (s.bright_day - 1.f) + twilight * (s.bright_dusk - 1.f) +
                       night * (s.bright_night - 1.f);
    glUniform4f(glGetUniformLocation(id, "brightness"), gain, s.shadow_lift, s.night_vis * night, 0.f);
    static size_t logged = 0;
    if (logged != script_next) {
      logged = script_next;
      lg::info("[enhanced] brightness uniform {:.3f} {:.2f} {:.2f} (hour {:.1f}, day {:.2f} twilight {:.2f} night {:.2f})",
               gain, s.shadow_lift, s.night_vis * night, hr, day, twilight, night);
    }
  }
  {
    // the light effects: the scene before and after the sprite buckets (capture_fx, this frame's
    // both), kept out of the day's grade and added back after it (fx_day: 0 at night, as before)
    const bool fx = fx_ready();
    glActiveTexture(GL_TEXTURE23);
    glBindTexture(GL_TEXTURE_2D, fx ? m_fx_tex[0] : m_black_tex);
    glActiveTexture(GL_TEXTURE24);
    glBindTexture(GL_TEXTURE_2D, fx ? m_fx_tex[1] : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T23"), 23);
    glUniform1i(glGetUniformLocation(id, "tex_T24"), 24);
    glUniform1f(glGetUniformLocation(id, "fx_day"), fx ? std::clamp(1.f - m_night, 0.f, 1.f) : 0.f);
    // (and the copy after them carries 1 - the blended dust's coverage in its alpha)
    glUniform1f(glGetUniformLocation(id, "fx_mark"), fx && m_fx_marked ? 1.f : 0.f);
    // the game's sky before the levels drew over it (capture_sky): what they drew there is kept
    // over the Enhanced sky (sky_copy: 2 with the copy before the sprites to compare, 1 without)
    const bool sky = sky_ready();
    glActiveTexture(GL_TEXTURE25);
    glBindTexture(GL_TEXTURE_2D, sky ? m_sky_tex : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T25"), 25);
    glUniform1f(glGetUniformLocation(id, "sky_copy"), sky ? (fx ? 2.f : 1.f) : 0.f);
  }
  glUniform1f(glGetUniformLocation(id, "chroma"), m_chroma);
  glUniform1f(glGetUniformLocation(id, "dawn_fog"), m_dawn_fog);
  glUniform1f(glGetUniformLocation(id, "cloud_rays"), m_settings.cloud_rays);
  {
    // Jak in view space (last frame's camera: close enough for "the pixels around him")
    float jv[3] = {0.f, 0.f, 0.f};
    for (int col = 0; col < 3; col++) {
      for (int row = 0; row < 3; row++) {
        jv[col] += (g_enhanced_player.pos[row] - m_prev_camera.trans[row]) * m_prev_camera.rot[row][col];
      }
    }
    glUniform4f(glGetUniformLocation(id, "jak_fx"), jv[0], jv[1], jv[2], m_jak_wet);
  }
  {
    // the characters' lowest points around Jak (do_chr_occlusion): their ground occlusion
    // (unit 31: 29 is the depth pyramid, 30 the sky map)
    glActiveTexture(GL_TEXTURE31);
    glBindTexture(GL_TEXTURE_2D, m_chr_occ_ready ? m_chr_occ_tex : m_black_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T31"), 31);
    glUniformMatrix4fv(glGetUniformLocation(id, "chr_occ_mvp"), 1, GL_TRUE, m_chr_occ_mvp);
    glUniform1f(glGetUniformLocation(id, "chr_occ_on"), m_chr_occ_ready ? m_chr_occ_depth_m : 0.f);
  }
  glUniform1f(glGetUniformLocation(id, "lens_drops"), m_movie > 0.5f ? 0.f : m_settings.lens_drops);
  glUniform3f(glGetUniformLocation(id, "cine_dof"), m_focus, m_settings.cine_aperture,
              m_focus > 0.f && m_ssao_ready ? m_cine * 9.f * h / 1080.f : 0.f);
  glUniform1f(glGetUniformLocation(id, "blast_sparks"), m_settings.blast_sparks);
  glUniform1f(glGetUniformLocation(id, "fx_detail"), m_settings.fx_detail);
  glUniform1f(glGetUniformLocation(id, "close_up"), m_movie * kCloseUp);
  glUniform1f(glGetUniformLocation(id, "vignette"),
              m_settings.vignette + (g_enhanced_menu.photo_active ? m_settings.photo_vignette : 0.f));
  glUniform1f(glGetUniformLocation(id, "sharpen"), m_settings.sharpen);
  return shader;
}
