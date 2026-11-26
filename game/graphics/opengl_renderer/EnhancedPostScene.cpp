// EnhancedPost::apply_to_scene: the whole chain on the finished 3D scene, pass by pass (split
// from EnhancedPost.cpp; shared helpers in EnhancedPostInternal.h)

#include "game/graphics/opengl_renderer/EnhancedPostInternal.h"

#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/EnhancedFrameProbe.h"
#include "game/graphics/opengl_renderer/Shader.h"

#include <SDL3/SDL_video.h>

#include <chrono>

using namespace enh_post;

namespace {
// How far the GPU runs behind the CPU at a point of the post: a timestamp query (written when the
// GPU gets there) against the GPU's clock read by the CPU as it queues it (glGetInteger64v
// GL_TIMESTAMP, no wait). Near 0 = the GPU was idle, waiting for these commands (starved by the
// CPU or by commands still sitting in the driver); large = it was busy with earlier work. Read 4
// frames later, never waited for; "debug": {"cpu_profile": 1} logs it every 5 s.
struct LagProbe {
  const char* name;
  GLuint q[4] = {};
  GLint64 cpu[4] = {};
  bool used[4] = {};
  double sum = 0.0;
  int n = 0, starved = 0;
  void mark(unsigned frame) {
    if (!q[0]) {
      glGenQueries(4, q);
    }
    const int s = frame & 3;
    if (used[s]) {
      GLint ready = 0;
      glGetQueryObjectiv(q[s], GL_QUERY_RESULT_AVAILABLE, &ready);
      if (ready) {
        GLuint64 g = 0;
        glGetQueryObjectui64v(q[s], GL_QUERY_RESULT, &g);
        const double lag = ((double)(GLint64)g - (double)cpu[s]) * 1e-6;
        if (lag > -5.0 && lag < 100.0) {
          sum += lag;
          n++;
          starved += lag < 0.05 ? 1 : 0;
        }
      }
      used[s] = false;
    }
    glQueryCounter(q[s], GL_TIMESTAMP);
    glGetInteger64v(GL_TIMESTAMP, &cpu[s]);
    used[s] = true;
  }
  std::string report() {
    std::string out = n ? fmt::format("{} {:.2f} ({}% idle)", name, sum / n, 100 * starved / n) : "";
    sum = 0.0;
    n = starved = 0;
    return out;
  }
};
LagProbe g_lag_start{"start"}, g_lag_composite{"composite"}, g_lag_blit{"blit"}, g_lag_taa{"taa"},
    g_lag_end{"end"};

// Variable rate shading for the composite (tuning post.vrs; GL_NV_shading_rate_image, NVIDIA
// Turing and later; elsewhere it stays off). The composite costs the same on every pixel, ~13 ms
// at 2560x1600: where the picture is smooth (sky, shade, flat sand) one invocation for 2x1, 1x2
// or 2x2 pixels looks the same. enh_vrs.comp picks the rate per 16x16 tile from this frame's game
// picture and depth and last frame's finished picture (the composite's own detail). post.vrs 1:
// adaptive; post.vrs_force n (0..6: 1x1, 2x1, 1x2, 2x2, 4x4, ...) the same rate everywhere (test);
// post.vrs_show 1: the coarse tiles drawn 4x4, as blocks; post.vrs_edge / post.vrs_tex /
// post.vrs_depth: the steps that need full rate (one luma step, the tile's mean luma step, a
// relative depth step; 0 = depth not used). post.vrs_log 1: "[enhanced] vrs" in the log every
// 5 s, share of the tiles at each rate (it waits for the GPU: test only).
namespace vrs {
constexpr GLenum kShadingRateImage = 0x9563;  // GL_SHADING_RATE_IMAGE_NV
constexpr GLenum kTexelWidth = 0x955C, kTexelHeight = 0x955D;
constexpr GLenum kRates[7] = {0x9565, 0x9567, 0x9566, 0x9568, 0x956B, 0x956A, 0x9569};
// (1x1, 2 wide x 1 high, 1x2, 2x2, 4x4, 4x2, 2x4)
using BindImage = void(APIENTRY*)(GLuint);
using Palette = void(APIENTRY*)(GLuint, GLuint, GLsizei, const GLenum*);
using Barrier = void(APIENTRY*)(GLboolean);
struct State {
  int supported = -1;  // -1 not checked yet
  BindImage bind = nullptr;
  Palette palette = nullptr;
  Barrier barrier = nullptr;
  GLuint prog = 0;
  GLuint flags_tex = 0, rate_tex = 0, stats = 0;
  int tw = 0, th = 0, w = 0, h = 0;
  int frames = 0;
  std::chrono::steady_clock::time_point last_report;
  bool clear = false;
  fs::file_time_type mtime{};
  bool palette_set = false;
  bool active = false;  // the rate image was made this frame
};
State g;

GLuint compile() {
  const std::string src = file_util::read_text_file(
      file_util::get_file_path({Shader::shader_folder, "enh_vrs.comp"}));
  GLuint s = glCreateShader(GL_COMPUTE_SHADER);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    lg::error("[enhanced] vrs: enh_vrs.comp: {}", log);
    glDeleteShader(s);
    return 0;
  }
  GLuint prog = glCreateProgram();
  glAttachShader(prog, s);
  glLinkProgram(prog);
  glDeleteShader(s);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

bool check() {
  if (g.supported >= 0) {
    return g.supported == 1;
  }
  g.supported = 0;
  if (!SDL_GL_ExtensionSupported("GL_NV_shading_rate_image")) {
    lg::info("[enhanced] vrs: GL_NV_shading_rate_image not supported, post.vrs stays off");
    return false;
  }
  g.bind = (BindImage)SDL_GL_GetProcAddress("glBindShadingRateImageNV");
  g.palette = (Palette)SDL_GL_GetProcAddress("glShadingRateImagePaletteNV");
  g.barrier = (Barrier)SDL_GL_GetProcAddress("glShadingRateImageBarrierNV");
  GLint tw = 0, th = 0;
  glGetIntegerv(kTexelWidth, &tw);
  glGetIntegerv(kTexelHeight, &th);
  g.prog = compile();
  if (!g.bind || !g.palette || !g.barrier || tw != 16 || th != 16 || !g.prog) {
    lg::info("[enhanced] vrs: unusable (texel {}x{}, program {}), post.vrs stays off", tw, th,
             g.prog);
    return false;
  }
  lg::info("[enhanced] vrs: GL_NV_shading_rate_image ready");
  g.supported = 1;
  return true;
}

// the rate image for this frame (prev_tex: last frame's finished picture); false = shade normally
bool make(int w, int h, bool log, GLuint prev_tex, GLuint scene_tex, GLuint depth_tex, const float* proj,
          float edge_thr, float tex_thr, float depth_thr, int force, int show, int dilate,
          const float* to_prev, const float* to_prev_offset, float prev_fx, float prev_fy) {
  g.active = false;
  if (!check()) {
    return false;
  }
  if (w != g.w || h != g.h) {
    if (g.flags_tex) {
      glDeleteTextures(1, &g.flags_tex);
      glDeleteTextures(1, &g.rate_tex);
    }
    g.w = w;
    g.h = h;
    g.tw = (w + 15) / 16;
    g.th = (h + 15) / 16;
    GLuint t[2];
    glGenTextures(2, t);
    g.flags_tex = t[0];
    g.rate_tex = t[1];
    for (GLuint tex : t) {
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8UI, g.tw, g.th);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  if (!g.stats) {
    glGenBuffers(1, &g.stats);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.stats);
    const GLuint zeros[8] = {};
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(zeros), zeros, GL_DYNAMIC_READ);
  }
  // (post.vrs_log 1, every 5 s: the counts since, then cleared; it waits for the frame before, so
  // a test switch only)
  const auto now = std::chrono::steady_clock::now();
  if (g.frames == 0) {
    g.last_report = now;
  }
  ++g.frames;
  if (log && std::chrono::duration<double>(now - g.last_report).count() >= 5.0) {
    g.last_report = now;
    GLuint c[8] = {};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.stats);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(c), c);
    const double n = std::max(1u, c[0] + c[1] + c[2] + c[3] + c[4] + c[5] + c[6]);
    lg::info("[enhanced] vrs: tiles 1x1 {:.0f}%, 2x1 {:.0f}%, 1x2 {:.0f}%, 2x2 {:.0f}%, other {:.0f}% "
             "(shaded {:.0f}% of the pixels)",
             100 * c[0] / n, 100 * c[1] / n, 100 * c[2] / n, 100 * c[3] / n,
             100 * (c[4] + c[5] + c[6]) / n,
             100 * (c[0] + 0.5 * (c[1] + c[2]) + 0.25 * c[3] + 0.0625 * c[4] + 0.125 * (c[5] + c[6])) / n);
    g.clear = true;
  }
  // (enh_vrs.comp again when it changes, with JAK_ENHANCED_SHADER_RELOAD)
  static const bool reload = std::getenv("JAK_ENHANCED_SHADER_RELOAD") != nullptr;
  if (reload && (g.frames % 30) == 0) {
    std::error_code ec;
    const auto mt = fs::last_write_time(
        file_util::get_file_path({Shader::shader_folder, "enh_vrs.comp"}), ec);
    if (!ec && mt != g.mtime) {
      if (g.mtime != fs::file_time_type{}) {
        if (GLuint p = compile()) {
          glDeleteProgram(g.prog);
          g.prog = p;
          lg::info("[enhanced] vrs: enh_vrs.comp reloaded");
        }
      }
      g.mtime = mt;
    }
  }
  GLint old_ssbo = 0;
  glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 3, &old_ssbo);
  GLint old_tex[3] = {};
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex[i]);
  }
  if (g.clear) {
    g.clear = false;
    const GLuint zero = 0;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.stats);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
  }
  glUseProgram(g.prog);
  const GLuint texs[3] = {prev_tex, scene_tex, depth_tex};
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texs[i]);
  }
  glUniform1i(glGetUniformLocation(g.prog, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(g.prog, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(g.prog, "tex_T2"), 2);
  glUniform2i(glGetUniformLocation(g.prog, "size"), w, h);
  glUniform2i(glGetUniformLocation(g.prog, "tiles"), g.tw, g.th);
  glUniform4f(glGetUniformLocation(g.prog, "proj"), proj[0], proj[1], proj[2], proj[3]);
  glUniform3f(glGetUniformLocation(g.prog, "thr"), edge_thr, tex_thr, depth_thr);
  glUniform1i(glGetUniformLocation(g.prog, "force"), force);
  glUniform1i(glGetUniformLocation(g.prog, "show"), show);
  glUniform1i(glGetUniformLocation(g.prog, "stats"), log ? 1 : 0);
  glUniform1i(glGetUniformLocation(g.prog, "dilate"), dilate);
  glUniformMatrix3fv(glGetUniformLocation(g.prog, "to_prev"), 1, GL_FALSE, to_prev);
  glUniform3f(glGetUniformLocation(g.prog, "to_prev_offset"), to_prev_offset[0], to_prev_offset[1],
              to_prev_offset[2]);
  glUniform2f(glGetUniformLocation(g.prog, "prev_f"), prev_fx, prev_fy);
  glBindImageTexture(0, g.flags_tex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R8UI);
  glBindImageTexture(1, g.rate_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8UI);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g.stats);
  glUniform1i(glGetUniformLocation(g.prog, "mode"), 0);
  glDispatchCompute(g.tw, g.th, 1);
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
  glUniform1i(glGetUniformLocation(g.prog, "mode"), 1);
  glDispatchCompute((g.tw + 7) / 8, (g.th + 7) / 8, 1);
  glMemoryBarrier(GL_ALL_BARRIER_BITS);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, old_ssbo);
  for (int i = 2; i >= 0; i--) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, old_tex[i]);
  }
  g.active = true;
  return true;
}

// around the composite's draw only
void begin() {
  if (!g.active) {
    return;
  }
  if (!g.palette_set) {
    g.palette(0, 0, 7, kRates);
    g.palette_set = true;
  }
  g.barrier(GL_TRUE);
  g.bind(g.rate_tex);
  glEnable(kShadingRateImage);
}
void end() {
  if (g.active) {
    glDisable(kShadingRateImage);
    g.bind(0);
  }
}
}  // namespace vrs

// the composite's CPU setup split in steps ("debug": {"cpu_profile": 1}): each ENH_CPU_SPLIT names
// the stretch of code that ends there; "[enhanced] post cpu setup" in the log every 5 s
struct CpuSplits {
  static constexpr int kN = 40;
  double acc[kN] = {};
  const char* names[kN] = {};
  int idx = 0;
  bool on = false;
  std::chrono::steady_clock::time_point last;
  void start(bool enable) {
    on = enable;
    idx = 0;
    if (on) {
      last = std::chrono::steady_clock::now();
    }
  }
  void mark(const char* name) {
    if (!on || idx >= kN) {
      return;
    }
    const auto t = std::chrono::steady_clock::now();
    acc[idx] += std::chrono::duration<double, std::milli>(t - last).count();
    names[idx++] = name;
    last = t;
  }
  std::string report(int frames) {
    std::string out;
    for (int i = 0; i < kN; i++) {
      if (names[i] && frames > 0 && acc[i] / frames >= 0.005) {
        out += fmt::format("{} {:.3f}, ", names[i], acc[i] / frames);
      }
      acc[i] = 0.0;
    }
    return out;
  }
};
CpuSplits g_setup_splits;
#define ENH_CPU_SPLIT(name) g_setup_splits.mark(name)

// lights.stab_log (1: a line a second, 2: also each event): how steady the dynamic lights are, from
// what is uploaded each frame alone (so the same numbers for any way of picking them). Each uploaded
// light is matched to last frame's by its place (1 m, nearest first, each used once).
struct LightStab {
  struct L {
    float world[3];
    float radius;
    float luma;  // r + g + b as uploaded (its fade in it)
    float sw;    // its shadow's weight (0: none)
    int slot;    // its shadow map's slot, -1: none
    bool instant;
    bool moving;  // a moving light (a car's, a shot's): its jumps counted apart (mjumps)
    u32 id;      // its track (0: none): the same light next frame, wherever it moved
    double cast_t0 = 0.0;  // its shadow on (sw > 0) since
  };
  // swaps: shadows switched on (sw 0 -> > 0); castvis: on or off where seen (in view, within
  // 30 m); mapsw2: a shadow's map changed while it showed (not its first map); nearmiss: frames
  // with a lamp within lights.caster_near_m in view 1.5 s and no caster; cpu: the tracks' and
  // casters' CPU time
  int swaps = 0, castvis = 0, mapsw2 = 0, nearmiss = 0;
  // the caster set's own changes: joins, drops (out of view 8 s or light gone), seats given by
  // one out of view, in-view swaps started
  int cjoin = 0, cdrop = 0, cseat = 0, cweak = 0;
  bool events = false;  // lights.stab_log 2
  double cpu_ns = 0.0;
  int cpu_n = 0;
  std::vector<L> prev;
  float prev_trans[3] = {0.f, 0.f, 0.f};
  float prev_rot[3][3] = {};
  double last_print = 0.0;
  int newvis = 0;  // new ones whose reach touched the view last frame too (popped in where seen)
  int cand = 0, inview = 0, used = 0, over32 = 0, nw = 0, newnear = 0, newpop = 0, gone = 0,
      capdrop = 0, jumps = 0, cast = 0, castpop = 0, mapsw = 0, forgot = 0, frames = 0, mjumps = 0;
  float jump = 0.f;
  u32 assigns0 = 0;
  bool first = true;
};
LightStab g_light_stab;
}  // namespace

void enh_cpu_split(const char* name) {
  g_setup_splits.mark(name);
}

// post.hiz: the depth copy's view depths (sky and the sea itself: far) and their mip chain of
// nearest depths, R32F, at most 10 levels (a 1080p frame's tiles up to 512 pixels). ~0.05 ms.
void EnhancedPost::build_hiz(GLuint screen_vao,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  const int w = m_ssao_w, h = m_ssao_h;
  if (w != m_hiz_w || h != m_hiz_h || !m_hiz_tex) {
    if (m_hiz_tex) {
      glDeleteTextures(1, &m_hiz_tex);
      glDeleteFramebuffers(1, &m_hiz_fbo);
    }
    m_hiz_w = w;
    m_hiz_h = h;
    m_hiz_levels = 1;
    while (m_hiz_levels < 10 && (w >> m_hiz_levels) > 0 && (h >> m_hiz_levels) > 0) {
      m_hiz_levels++;
    }
    glGenTextures(1, &m_hiz_tex);
    glBindTexture(GL_TEXTURE_2D, m_hiz_tex);
    // (each level half the one before, rounded down: the shaders take level L as size >> L)
    for (int l = 0; l < m_hiz_levels; l++) {
      glTexImage2D(GL_TEXTURE_2D, l, GL_R32F, std::max(1, w >> l), std::max(1, h >> l), 0, GL_RED,
                   GL_FLOAT, nullptr);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &m_hiz_fbo);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  const bool sea = m_far_ocean_ready && m_ocean_w == w && m_ocean_h == h;
  auto& sh = render_state->shaders[ShaderId::ENH_HIZ];
  sh.activate();
  const auto id = sh.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "has_sea"), sea ? 1 : 0);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_hiz_fbo);
  ENH_TIMED("enh-hiz", {
    for (int l = 0; l < m_hiz_levels; l++) {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_hiz_tex, l);
      glViewport(0, 0, std::max(1, w >> l), std::max(1, h >> l));
      if (l == 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, sea ? m_ocean_depth[2] : m_black_tex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_depth_tex);
        glUniform1i(glGetUniformLocation(id, "mode"), 0);
      } else {
        // (reads only the level before: base = max = l - 1, no feedback with the level drawn)
        glBindTexture(GL_TEXTURE_2D, m_hiz_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, l - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, l - 1);
        glUniform1i(glGetUniformLocation(id, "mode"), 1);
        glUniform2i(glGetUniformLocation(id, "src_size"), std::max(1, w >> (l - 1)),
                    std::max(1, h >> (l - 1)));
      }
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      prof.add_draw_call();
    }
  });
  glBindTexture(GL_TEXTURE_2D, m_hiz_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, m_hiz_levels - 1);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_hiz_ready = true;
}

// post.sky_map (tuning.json, switches in a running game)
bool EnhancedPost::sky_map_on() const {
  static const EnhancedTune t_sky_map("post.sky_map", 1.f);
  return (float)t_sky_map > 0.5f;
}

// the sky map: kSkyMapSize^2 rays straight down from high over the camera, a band of rows a frame
// into the back texture; swapped in whole. Traced again when the camera has gone a quarter of the
// map from its center or the levels change
void EnhancedPost::build_sky_map(GLuint screen_vao,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof) {
  const auto& cam = g_enhanced_camera;
  if (!sky_map_on() || !m_rt_ready || !cam.valid) {
    if (!sky_map_on()) {
      m_sky_valid = false;
      m_sky_row = -1;
    }
    return;
  }
  constexpr int N = kSkyMapSize;
  const float cell = kSkyMapCell * kMeter;
  const float half = N * cell * 0.5f;
  if (!m_skymap_tex[0]) {
    glGenTextures(2, m_skymap_tex);
    glGenFramebuffers(2, m_skymap_fbo);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_skymap_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, N, N, 0, GL_RGBA, GL_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_skymap_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_skymap_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  if (m_sky_row < 0) {
    // start a new one? (camera a quarter of the map off the front's center, other levels, none yet)
    const float cx = m_sky_origin[0] + half, cz = m_sky_origin[1] + half;
    const bool moved = std::abs(cam.trans[0] - cx) > half * 0.5f ||
                       std::abs(cam.trans[2] - cz) > half * 0.5f;
    if (!m_sky_valid || moved || m_sky_levels != m_rt_in_use) {
      // (the corner on a 16-cell grid: the texels fall on the same world spots each time)
      const float snap = 16.f * cell;
      m_sky_back_origin[0] = std::floor((cam.trans[0] - half) / snap) * snap;
      m_sky_back_origin[1] = std::floor((cam.trans[2] - half) / snap) * snap;
      m_sky_top = cam.trans[1] + 300.f * kMeter;
      m_sky_levels = m_rt_in_use;
      m_sky_row = 0;
    }
  }
  if (m_sky_row < 0) {
    return;
  }
  // (tuning.json "post.sky_map_rows": rows traced a frame)
  static const EnhancedTune t_rows("post.sky_map_rows", 32.f);
  const int band = std::clamp((int)(float)t_rows, 1, N);
  const int back = 1 - m_sky_front;
  GLint vp[4], sc[4], fbo = 0;
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_SCISSOR_BOX, sc);
  const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  auto& sh = render_state->shaders[ShaderId::ENH_SKY_MAP];
  sh.activate();
  const auto id = sh.id();
  for (int i = 0; i < 2; i++) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_rt_buf[i]);
  }
  glUniform1i(glGetUniformLocation(id, "rt_levels"), m_rt_count);
  glUniform4iv(glGetUniformLocation(id, "rt_base"), m_rt_count, m_rt_base.data());
  glUniform4f(glGetUniformLocation(id, "grid"), m_sky_back_origin[0], m_sky_back_origin[1], cell,
              m_sky_top);
  glUniform1f(glGetUniformLocation(id, "span"), 600.f * kMeter);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_skymap_fbo[back]);
  const int rows = std::min(band, N - m_sky_row);
  glViewport(0, 0, N, N);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, m_sky_row, N, rows);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glScissor(sc[0], sc[1], sc[2], sc[3]);
  if (!had_scissor) {
    glDisable(GL_SCISSOR_TEST);
  }
  prof.add_draw_call();
  m_sky_row += rows;
  if (m_sky_row >= N) {
    m_sky_front = back;
    m_sky_origin[0] = m_sky_back_origin[0];
    m_sky_origin[1] = m_sky_back_origin[1];
    m_sky_valid = true;
    m_sky_row = -1;
    // (tuning.json "post.sky_map_find" 1: logs the roofed floors nearest the camera, to test at;
    // a floor: the lowest surface, the next one over it 2.2-12 m higher, over all of a 5x5 block)
    static const EnhancedTune t_find("post.sky_map_find", 0.f);
    if ((float)t_find > 0.5f) {
      std::vector<float> px((size_t)N * N * 4);
      glReadPixels(0, 0, N, N, GL_RGBA, GL_FLOAT, px.data());
      std::vector<float> floor_y((size_t)N * N, -1e30f), head((size_t)N * N, 0.f);
      for (int i = 0; i < N * N; i++) {
        const float* h = &px[(size_t)i * 4];
        int lo = -1;
        for (int k = 0; k < 4; k++) {
          if (h[k] > -1e29f) {
            lo = k;
          }
        }
        if (lo >= 1) {
          floor_y[i] = h[lo];
          head[i] = h[lo - 1] - h[lo];
        }
      }
      struct Spot {
        float d2, x, y, z, head;
      };
      std::vector<Spot> spots;
      for (int r = 2; r < N - 2; r++) {
        for (int c = 2; c < N - 2; c++) {
          bool ok = true;
          const float f0 = floor_y[r * N + c];
          for (int dr = -2; dr <= 2 && ok; dr++) {
            for (int dc = -2; dc <= 2 && ok; dc++) {
              const int j = (r + dr) * N + c + dc;
              ok = floor_y[j] > -1e29f && std::abs(floor_y[j] - f0) < 0.3f * kMeter &&
                   head[j] > 2.2f * kMeter && head[j] < 12.f * kMeter;
            }
          }
          if (ok) {
            const float x = m_sky_origin[0] + (c + 0.5f) * cell;
            const float z = m_sky_origin[1] + (r + 0.5f) * cell;
            const float dx = x - cam.trans[0], dz = z - cam.trans[2];
            spots.push_back({dx * dx + dz * dz, x, f0, z, head[r * N + c]});
          }
        }
      }
      std::sort(spots.begin(), spots.end(),
                [](const Spot& a, const Spot& b) { return a.d2 < b.d2; });
      lg::info("[enhanced] sky map: {} roofed floor cells", spots.size());
      int shown = 0;
      for (size_t i = 0; i < spots.size() && shown < 8; i += std::max<size_t>(1, spots.size() / 8)) {
        const auto& s = spots[i];
        lg::info("[enhanced]   roofed floor at {:.1f},{:.1f},{:.1f} m, roof {:.1f} m over it, {:.0f} m off",
                 s.x / kMeter, s.y / kMeter, s.z / kMeter, s.head / kMeter, std::sqrt(s.d2) / kMeter);
        shown++;
      }
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

// post.probes (tuning.json, switches in a running game)
bool EnhancedPost::probes_on() const {
  static const EnhancedTune t_probes("post.probes", 0.f);
  return (float)t_probes > 0.5f;
}

// the light probes: every probe of the grid around the camera traces post.probe_rays rays a frame
// (a band of the grid in every post.probe_every: 8192 probes) and blends them over what it had;
// the grid moves by whole cells
void EnhancedPost::build_probes(GLuint screen_vao,
                                SharedRenderState* render_state,
                                ScopedProfilerNode& prof) {
  const auto& cam = g_enhanced_camera;
  if (!probes_on() || !m_rt_ready || !cam.valid || !g_enhanced_tod.valid) {
    m_probe_valid = false;
    return;
  }
  constexpr int W = kProbeX * kProbeY, H = kProbeZ;
  const float cell = kProbeCell * kMeter;
  if (!m_probe_tex[0]) {
    glGenTextures(2, m_probe_tex);
    glGenFramebuffers(2, m_probe_fbo);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_probe_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_probe_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_probe_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  // the corner: the camera at the grid's middle across, a quarter up (more of it below: the ground
  // and the characters on it), in whole cells
  const int cc[3] = {(int)std::floor(cam.trans[0] / cell) - kProbeX / 2,
                     (int)std::floor(cam.trans[1] / cell) - kProbeY * 3 / 4,
                     (int)std::floor(cam.trans[2] / cell) - kProbeZ / 2};
  const int sh[3] = {cc[0] - m_probe_cell[0], cc[1] - m_probe_cell[1], cc[2] - m_probe_cell[2]};
  const bool keep = m_probe_valid && std::abs(sh[0]) < kProbeX && std::abs(sh[1]) < kProbeY &&
                    std::abs(sh[2]) < kProbeZ;
  static const EnhancedTune t_rays("post.probe_rays", 8.f);
  static const EnhancedTune t_blend("post.probe_blend", 0.25f);
  static const EnhancedTune t_every("post.probe_every", 8.f);
  static const EnhancedTune t_range("post.probe_range_m", 40.f);
  const int back = 1 - m_probe_front;
  GLint vp[4], fbo = 0;
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  auto& shd = render_state->shaders[ShaderId::ENH_PROBES];
  shd.activate();
  const auto id = shd.id();
  for (int i = 0; i < 3; i++) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_rt_buf[i]);
  }
  glActiveTexture(GL_TEXTURE12);
  glBindTexture(GL_TEXTURE_2D_ARRAY, m_rt_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_probe_tex[m_probe_front]);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T12"), 12);
  glUniform1i(glGetUniformLocation(id, "rt_levels"), m_rt_count);
  glUniform4iv(glGetUniformLocation(id, "rt_base"), m_rt_count, m_rt_base.data());
  float tod[8 * 4];
  for (int p = 0; p < 8; p++) {
    for (int ch = 0; ch < 4; ch++) {
      u32 word = (u32)g_enhanced_tod.itimes[p / 2][(p % 2) * 2 + ch / 2];
      u32 hw = (ch % 2) ? (word >> 16) : word;
      tod[p * 4 + ch] = (hw & 0xff) / 64.f;
    }
  }
  glUniform4fv(glGetUniformLocation(id, "rt_tod"), 8, tod);
  glUniform4f(glGetUniformLocation(id, "grid"), cc[0] * cell, cc[1] * cell, cc[2] * cell, cell);
  glUniform4i(glGetUniformLocation(id, "shift"), sh[0], sh[1], sh[2], keep ? 1 : 0);
  glUniform3i(glGetUniformLocation(id, "dims"), kProbeX, kProbeY, kProbeZ);
  m_probe_frame += 1.f;
  if (m_probe_frame > 4096.f) {
    m_probe_frame = 0.f;
  }
  glUniform1f(glGetUniformLocation(id, "frame"), m_probe_frame);
  glUniform1f(glGetUniformLocation(id, "blend"), std::clamp((float)t_blend, 0.01f, 1.f));
  glUniform1f(glGetUniformLocation(id, "tmax"), std::max((float)t_range, 1.f) * kMeter);
  glUniform1i(glGetUniformLocation(id, "rays"), std::clamp((int)(float)t_rays, 1, 64));
  glUniform1i(glGetUniformLocation(id, "every"), std::clamp((int)(float)t_every, 1, kProbeZ / 4));
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_probe_fbo[back]);
  glViewport(0, 0, W, H);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (had_scissor) {
    glEnable(GL_SCISSOR_TEST);
  }
  prof.add_draw_call();
  m_probe_front = back;
  for (int i = 0; i < 3; i++) {
    m_probe_cell[i] = cc[i];
  }
  m_probe_valid = true;
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void EnhancedPost::apply_to_scene(GLuint render_fbo,
                                  int w,
                                  int h,
                                  GLuint screen_vao,
                                  SharedRenderState* render_state,
                                  ScopedProfilerNode& prof) {
  if (m_gpu_query_open) {
    glQueryCounter(m_gpu_query[m_gpu_query_slot][1], GL_TIMESTAMP);
  }
  if (!m_settings.enabled) {
    g_enhanced_camera.valid = false;
    g_enhanced_on = false;  // (the dunes' baked shade back, Hfrag.cpp)
    // the ambient sounds go quiet with the rest of Enhanced
    static const float silent[snd::ambience::kLayers] = {};
    snd::ambience::set_targets(silent);
    // and the game's own rain sound is back
    g_enhanced_play_weather.own_rain = false;
    return;
  }
  // (the scene's copy in half floats too while the game's frame is (post.hdr_scene, OpenGLRenderer's
  // make_fbo): the light past 1 reaches the post)
  static const EnhancedTune t_hdr_scene("post.hdr_scene", 0.f);
  static bool scene_hdr = false;
  const bool hdr_scene = (float)t_hdr_scene > 0.5f;
  if (w != m_scene_w || h != m_scene_h || hdr_scene != scene_hdr) {
    if (m_scene_fbo) {
      glDeleteFramebuffers(1, &m_scene_fbo);
      glDeleteTextures(1, &m_scene_tex);
    }
    m_scene_w = w;
    m_scene_h = h;
    scene_hdr = hdr_scene;
    glGenFramebuffers(1, &m_scene_fbo);
    glGenTextures(1, &m_scene_tex);
    m_adapt_scene_tex = 0;  // (read_exposure asks for its format again)
    glBindTexture(GL_TEXTURE_2D, m_scene_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, hdr_scene ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA,
                 hdr_scene ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scene_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // (the whole post on the GPU, as the passes' timers see it: the log's "post" interval also holds
  // the vsync wait when the frame has time to spare)
  const auto post_cpu0 = std::chrono::steady_clock::now();
  // (test switch "debug": {"post_flush": n}: glFlush at points of the post, so the GPU gets the
  // queued commands before the CPU's next long stretch of setup; bits 1 before the post, 2 after
  // prepare, 4 after the composite)
  static const EnhancedTune t_post_flush("debug.post_flush", 0.f);
  const int post_flush = (int)(float)t_post_flush;
  if (post_flush & 1) {
    glFlush();
  }
  static const EnhancedTune t_cpu_profile("debug.cpu_profile", 0.f);
  const bool lag_probes = (float)t_cpu_profile > 0.5f;
  if (lag_probes) {
    g_lag_start.mark(pass_timer_frame());
  }
  g_setup_splits.start(lag_probes);
  static enh_post::PassTimer t_post_total("enh-total");
  enh_post::PassTimerScope post_total_scope(t_post_total);
  // copy (resolving msaa) the scene so we can read it while drawing over it
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_fbo);
  ENH_TIMED("enh-scene-copy", glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST));
  ENH_CPU_SPLIT("scene-copy");
  enh_probe::sample(enh_probe::kScene, m_scene_fbo, 0, 0, w, h);

  const EnhancedCamera cam = g_enhanced_camera;  // prepare() clears the global
  ENH_TIMED("enh-prepare", prepare(m_scene_tex, render_fbo, w, h, screen_vao, render_state, prof));
  if (post_flush & 2) {
    glFlush();
  }
  const bool have_scene = m_ssao_ready;
  if (enh_probe::on() && (!have_scene || !cam.valid)) {
    enh_probe::state(!cam.valid ? "no-camera" : "no-scene");
  }
  // (where the post's GPU time goes between the named passes: the whole interval from here to the
  // composite's draw on the GPU's clock, which counts its idle time while the CPU sets the composite
  // up, and the CPU's own time for it; "[enhanced] post cpu" in the log every 5 s)
  static enh_post::PassTimer t_setup_gap("enh-setup-span");
  auto setup_span = std::make_unique<enh_post::PassTimerScope>(t_setup_gap);
  const auto setup_cpu0 = std::chrono::steady_clock::now();
  ENH_CPU_SPLIT("prepare-rest");
  update_scene();
  ENH_CPU_SPLIT("update_scene");
  if (have_scene) {
    // (and the common level: the HUD's textures are there, enhanced_character_hud)
    auto levels = render_state->loader->get_in_use_levels();
    if (auto* common = render_state->loader->get_common_level()) {
      levels.push_back(common);
    }
    enhanced_editor_levels(levels);
    ENH_CPU_SPLIT("editor_levels");
    enhanced_editor_frame(w, h, m_depth_fbo, cam, m_last_proj.data());
    ENH_CPU_SPLIT("editor_frame");
  }
  if (have_scene) {
    read_exposure(w, h);
    ENH_CPU_SPLIT("read_exposure");
  }
  if (have_scene && m_cine > 0.f) {
    read_focus(w, h);
  } else if (m_cine <= 0.f) {
    m_focus = 0.f;
  }

  // view -> previous view is R^T Rp, with the camera's move as an offset (motion blur, TAA)
  float to_prev[3][3];  // [k][c]: column k of the GLSL mat3
  float offset[3];
  bool reproject = false;
  ENH_CPU_SPLIT("scene+editor+exposure");
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

  // temporal anti-aliasing: this frame blended into the reprojected history. After the composite
  // (late): its occlusion, shadows, lights and sand are made from this frame's jittered depth, a
  // fraction of a pixel off each frame: resolved before it, their edges (grass blades, pebbles)
  // flickered. (JAK_ENHANCED_TAA_CYCLE: early, late and off in turn, 6 s each, to compare.)
  GLuint scene_tex = m_scene_tex;
  const bool taa_on = m_settings.taa && m_jittered && have_scene;
  static const bool taa_cycle = std::getenv("JAK_ENHANCED_TAA_CYCLE") != nullptr;
  int taa_mode = taa_on ? 1 : -1;  // 0 early, 1 late
  ENH_CPU_SPLIT("to_prev");
  if (taa_on && taa_cycle) {
    static int last_mode = -2;
    taa_mode = (int)(m_time / 6.0) % 3;
    taa_mode = taa_mode == 2 ? -1 : taa_mode;
    if (taa_mode != last_mode) {
      lg::info("[enhanced] taa mode {}", taa_mode == 0 ? "early" : taa_mode == 1 ? "late" : "off");
      last_mode = taa_mode;
      m_taa_history = false;
    }
  }
  const bool taa = taa_mode >= 0;
  auto resolve_taa = [&](GLuint src_tex) -> GLuint {
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
    // the characters' own motion (tuning taa.motion, 0: the camera's reprojection only; taa.dbg 1
    // shows where it's used). Hellcat taking off at 640x360: crisp instead of smeared, 0.09 ms at
    // 2560x1600 (Spargus 18h, 60 models)
    static const EnhancedTune t_motion("taa.motion", 1.f);
    static const EnhancedTune t_taa_dbg("taa.dbg", 0.f);
    bool motion = false;
    static double motion_cpu_ms = 0.0;
    if ((float)t_motion > 0.5f) {
      const auto mv0 = std::chrono::steady_clock::now();
      ENH_TIMED("enh-motion",
                motion = do_motion(render_state, prof, w, h, to_prev, offset, reproject));
      motion_cpu_ms +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mv0).count();
    } else {
      m_mv_prev_ok = false;
    }
    {
      static int frames = 0;
      if ((float)t_motion > 0.5f && ++frames >= 300) {
        lg::info("[enhanced] taa motion: {} of {} models found again, {} still, {} draws, "
                 "{} KB of bones, cpu {:.3f} ms (match {:.3f}, upload {:.3f}, bind {:.3f}, clear {:.3f}, program {:.3f}, draws {:.3f})",
                 m_mv_matched, m_mv_models, m_mv_still, m_mv_draws,
                 g_enhanced_casters.bones.size() * 4 / 1024, motion_cpu_ms / frames,
                 m_mv_cpu[0] / frames, m_mv_cpu[1] / frames, m_mv_cpu[2] / frames, m_mv_cpu[3] / frames, m_mv_cpu[4] / frames, m_mv_cpu[5] / frames);
        frames = 0;
        motion_cpu_ms = 0.0;
        m_mv_cpu[0] = m_mv_cpu[1] = m_mv_cpu[2] = m_mv_cpu[3] = m_mv_cpu[4] = m_mv_cpu[5] = 0.0;
      }
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
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, motion ? m_mv_tex : 0);
    glUniform1i(glGetUniformLocation(rid, "tex_T4"), 4);
    glUniform1i(glGetUniformLocation(rid, "has_motion"), motion ? 1 : 0);
    glUniform1f(glGetUniformLocation(rid, "mv_stamp"), m_mv_stamp);
    glUniform1f(glGetUniformLocation(rid, "tune_taa_dbg"), (float)t_taa_dbg);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_taa_tex[1 - cur]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
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
    glUniform1f(glGetUniformLocation(rid, "time_s"), (float)m_time);
    ENH_TIMED("enh-taa", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    prof.add_draw_call();
    m_taa_cur = 1 - cur;
    m_taa_history = true;
    return m_taa_tex[cur];
  };
  ENH_CPU_SPLIT("taa-cycle");
  if (taa && taa_mode == 0) {
    scene_tex = resolve_taa(m_scene_tex);
  } else if (!taa) {
    m_taa_history = false;
  }

  // (post.hiz 1: the water's reflection walks a depth pyramid, 400 m instead of 80 m; 0: the old
  // per-step march; on by default: same look as the march, +0.06..0.13 ms)
  static const EnhancedTune t_hiz("post.hiz", 1.f);
  // (post.hiz_surf 1: puddles, metal and glass reflect through it too, so it's built on every
  // frame, water or not; on by default: the ctyport docks after rain look the same, the puddles'
  // rays reach 150 m instead of ~25 m, +0.05 ms where there's no water)
  static const EnhancedTune t_hiz_surf("post.hiz_surf", 1.f);
  // (not used by the AO: the shipping one is the Alchemy taps, 0.07-0.18 ms at 640x360, nothing
  // left for a pyramid to save, and it runs in prepare(), before the pyramid exists; contact
  // shadows march 0.5 m, a few pixels)
  ENH_CPU_SPLIT("taa-mode0");
  m_hiz_ready = false;
  m_hiz_surf = (float)t_hiz > 0.5f && (float)t_hiz_surf > 0.5f;
  if ((float)t_hiz > 0.5f && have_scene &&
      (m_hiz_surf || (m_settings.water_reflections && (m_ocean_ready || m_far_ocean_ready)))) {
    build_hiz(screen_vao, render_state, prof);
  }
  ENH_CPU_SPLIT("hiz");

  {
    // (variable rate shading, namespace vrs above: needs last frame's finished picture, the late
    // TAA's history. On by default: Spargus 18h at 2560x1600, composite 13.0 -> 7.5 ms, 42 -> 55
    // fps, full-size frames equal to vrs 0 within the TAA's noise; Hellcat diving at 50 m/s: no
    // blocks, the Hellcat stays at full rate)
    // (off by default for now: wavy bands on smooth dark rock at night, user 2026-11-14; 1 = on)
    static const EnhancedTune t_vrs("post.vrs", 0.f);
    static const EnhancedTune t_vrs_log("post.vrs_log", 0.f);
    static const EnhancedTune t_vrs_force("post.vrs_force", -1.f);
    static const EnhancedTune t_vrs_show("post.vrs_show", 0.f);
    static const EnhancedTune t_vrs_edge("post.vrs_edge", 0.1f);
    static const EnhancedTune t_vrs_tex("post.vrs_tex", 0.02f);
    static const EnhancedTune t_vrs_depth("post.vrs_depth", 0.f);
    // (last frame's edges and texture: 2 from where the tile's pixels were last frame, the camera's
    // move reprojected (still: the tile alone), 1 the 3x3 tiles around as before, 0 the tile alone)
    static const EnhancedTune t_vrs_dilate("post.vrs_dilate", 2.f);
    // (the auto quality's level 3 and up, before it lowers the resolution: the coarse rates reach
    // a little further, edges and texture a step stronger before they keep a tile at full rate)
    static const EnhancedTune t_auto_vrs("auto.vrs", {0.15f, 0.03f});
    float vrs_edge = (float)t_vrs_edge, vrs_tex = (float)t_vrs_tex;
    if (m_auto_level >= 3 && t_auto_vrs.size() >= 2) {
      vrs_edge = std::max(vrs_edge, t_auto_vrs[0]);
      vrs_tex = std::max(vrs_tex, t_auto_vrs[1]);
    }
    vrs::g.active = false;
    if ((float)t_vrs > 0.5f && have_scene && taa && taa_mode == 1 && m_taa_history && reproject &&
        m_taa_w == w && m_taa_h == h) {
      ENH_TIMED("enh-vrs", vrs::make(w, h, (float)t_vrs_log > 0.5f, m_taa_tex[1 - m_taa_cur], m_scene_tex, m_depth_tex,
                                     m_last_proj.data(), vrs_edge, vrs_tex, (float)t_vrs_depth,
                                     (int)std::round((float)t_vrs_force), (int)(float)t_vrs_show,
                                     (int)std::round((float)t_vrs_dilate), &to_prev[0][0], offset,
                                     m_prev_camera.fx, m_prev_fy));
    }
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
  ENH_CPU_SPLIT("activate");
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  // the game's brightness/contrast is applied later, on the whole frame
  glUniform4f(glGetUniformLocation(id, "color_mult"), 1.f, 1.f, 1.f, 1.f);
  glUniform4f(glGetUniformLocation(id, "color_add"), 0.f, 0.f, 0.f, 0.f);

  // world up in view space, for the fog and the heat haze
  glUniform3f(glGetUniformLocation(id, "up_view"), cam.rot[1][0], cam.rot[1][1], cam.rot[1][2]);
  {
    // (fog.w: the look's switches as bits, no new uniform: 1 the richer day sky, 2 explosions by day
    // kept under the highlights. Tuning look.sky_extras / look.blast_day 0: off)
    static const EnhancedTune t_sky_extras("look.sky_extras", 1.f);
    static const EnhancedTune t_blast_day("look.blast_day", 1.f);
    // 4: a punchier fireball, thinner shorter soot and a darker, smaller dust ring (look.blast_punch)
    static const EnhancedTune t_blast_punch("look.blast_punch", 1.f);
    const int bits = ((float)t_sky_extras > 0.5f ? 1 : 0) | ((float)t_blast_day > 0.5f ? 2 : 0) |
                     ((float)t_blast_punch > 0.5f ? 4 : 0);
    glUniform4f(glGetUniformLocation(id, "fog"), m_settings.fog_density / kMeter,
                m_settings.fog_height / kMeter, have_scene ? m_settings.fog_amount * m_fog : 0.f,
                (float)bits);
  }
  glUniform3f(glGetUniformLocation(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f);
  m_time += 1.0 / 60.0;
  // (the heat shimmer only where it's hot, Jak 3's desert by day: elsewhere it only blurred and
  // shimmered the distance; lava has its own)
  const float hot = m_pond ? (1.f - m_night) * (1.f - std::min(1.f, m_rain * 2.f)) : 0.f;
  glUniform4f(glGetUniformLocation(id, "haze"), have_scene ? m_settings.haze_strength * hot : 0.f,
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
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, near_water ? m_ocean_depth[0] : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T5"), 5);
  glUniform1i(glGetUniformLocation(id, "tex_T6"), 6);
  glUniform1i(glGetUniformLocation(id, "tex_T14"), 14);
  m_far_ocean_ready = false;
  glUniform1f(glGetUniformLocation(id, "water_strength"),
              water ? m_settings.water_reflection_strength : 0.f);
  // the camera's right and forward in view space, and its position, for the ripples
  glUniform3f(glGetUniformLocation(id, "right_view"), cam.rot[0][0], cam.rot[0][1], cam.rot[0][2]);
  glUniform3f(glGetUniformLocation(id, "fwd_view"), cam.rot[2][0], cam.rot[2][1], cam.rot[2][2]);
  glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  {
    // sunny: Jak 1's turquoise sea
    const float deep[3] = {0.02f, 0.3f, 0.4f}, shallow[3] = {0.12f, 0.68f, 0.66f};
    float d[3], sh[3];
    // each place its own water (Jak 3): the desert's (its coast, Spargus' sea, the oases) teal
    // near the shore and a clear blue out, fitting the warm sand; Haven's a readable grey teal.
    // The body a little brighter by day (it was an almost black blue). water.place 0 and
    // water.body 1 (or shaders.tune_water_dbg 60/61): the old colors
    static const EnhancedTune t_place("water.place", 1.f);
    static const EnhancedTune t_body("water.body", 1.5f);
    float dbg = 0.f;
    enhanced_tune_lookup("shaders.tune_water_dbg", &dbg, 1);
    const bool old = std::abs(dbg - 60.f) < 0.5f || std::abs(dbg - 61.f) < 0.5f;
    const bool jak3 = g_game_version == GameVersion::Jak3;
    const bool spargus = jak3 && m_city && !m_haven_windows;
    const bool desert = jak3 && (m_pond || spargus);
    const bool haven = jak3 && m_haven_windows;
    const float place = old ? 0.f : std::clamp((float)t_place, 0.f, 1.f);
    // (Haven only half as much brighter: at 1.5 its port went bright cyan at sunset, not grey teal)
    const float body = old ? 1.f : 1.f + ((float)t_body - 1.f) * (1.f - m_night) * (haven ? 0.5f : 1.f);
    const float des_deep[3] = {0.05f, 0.30f, 0.38f}, des_shallow[3] = {0.20f, 0.62f, 0.58f};
    const float hav_deep[3] = {0.06f, 0.20f, 0.24f}, hav_shallow[3] = {0.12f, 0.30f, 0.30f};
    for (int i = 0; i < 3; i++) {
      float pd = m_settings.water_deep[i], ps = m_settings.water_shallow[i];
      if (desert) {
        pd += (des_deep[i] - pd) * place;
        ps += (des_shallow[i] - ps) * place;
      } else if (haven) {
        pd += (hav_deep[i] - pd) * place;
        ps += (hav_shallow[i] - ps) * place;
      }
      d[i] = (pd + (deep[i] - pd) * m_sunny) * body;
      sh[i] = (ps + (shallow[i] - ps) * m_sunny) * body;
    }
    glUniform3fv(glGetUniformLocation(id, "water_deep_color"), 1, d);
    glUniform3fv(glGetUniformLocation(id, "water_shallow_color"), 1, sh);
  }
  glUniform1f(glGetUniformLocation(id, "water_unify"), m_settings.water_unify);
  glUniform1f(glGetUniformLocation(id, "water_swell_strength"), m_settings.water_swell);
  glUniform1f(glGetUniformLocation(id, "water_refraction"), m_settings.water_refraction);
  glUniform1f(glGetUniformLocation(id, "water_caustics"), m_settings.water_caustics);
  // in Jak 3's desert the ocean renderer draws the oases: ponds, murky, their rough bed hidden
  // as in the game
  glUniform1i(glGetUniformLocation(id, "pond"), m_pond ? 1 : 0);
  glUniform1f(glGetUniformLocation(id, "relight"), m_settings.relight);
  ENH_CPU_SPLIT("water-u");
  {
    // dynamic lights: the glows in view space, the brightest and nearest first, at most kLights
    constexpr int kLights = 32;
    struct Candidate {
      float pos[4];  // view space, radius
      float color[4];
      float score;
      bool violet = false;  // an effect's violet glow (shots, dark eco clouds)
      bool instant = false;  // lit at once, not smoothed (an explosion's flash, a fire's flicker)
      bool burst = false;    // an explosion's flash or a gun's charge: short, at full strength at once
      bool mem = false;      // a lamp's light from memory (its glow not drawn this frame)
    };
    std::vector<Candidate> candidates;
    struct Fire {
      float pos[4];  // view space, flame size
      float seed;
      float dist;
    };
    std::vector<Fire> fires;
    // the glows too far to light anything: drawn as points of light that carry across the dark
    // (a camp's fire, a town's lamps over the night desert)
    struct Beacon {
      float pos[4];    // view space, glow size
      float color[4];  // rgb, how far in it is (fades in past 40 m)
      float score;
    };
    std::vector<Beacon> beacons;
    static int debug_beacons = 0;
    const bool on = m_settings.lights && have_scene && cam.valid;
    g_enhanced_smoke_lights.count = 0;
    // in the sun's full daylight the glows' light is lost in it: a garage door's big orange glow
    // lit the sand around for tens of meters, with hard shadows of every slab (the beacons stay)
    // (under a roof the hour doesn't light the room: the temples' torches at noon ran at a quarter of
    // their night strength, no warm pool on the floor in front of them. lights.enclosed_fire 0: off)
    static const EnhancedTune t_enclosed("lights.enclosed_fire", 0.8f);
    const float enclosed = m_enclosed ? std::clamp((float)t_enclosed, 0.f, 1.f) : 0.f;
    const float glow_day = g_enhanced_sun.valid
                               ? (1.f - std::max(m_night, night_darkness())) * (1.f - enclosed)
                               : 0.f;
    for (int i = 0; on && i < g_enhanced_lights.count; i++) {
      const auto& raw = g_enhanced_lights.lights[i];
      auto l = raw;  // as the game draws it, faded with distance: the light it casts
      for (int k = 0; k < 3; k++) {
        l.color[k] *= raw.fade;
      }
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (l.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      // (at most 20 m: a street lamp's 30 m lit a whole square evenly, no pool of light under it)
      // (tuning.json: "lights": {"glow_min_m", "glow_max_m"})
      static const EnhancedTune t_glow_min("lights.glow_min_m", 4.f);
      static const EnhancedTune t_glow_max("lights.glow_max_m", 20.f);
      const float glow_lo = t_glow_min * kMeter;
      float radius = std::clamp(l.size * m_settings.light_reach, glow_lo,
                                std::max(glow_lo, t_glow_max * kMeter));
      c.pos[3] = radius;
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // far ones in front: beacons (not the moon or the sky's glows: huge or kilometers away)
      // (not a fire's sparks and embers either: tiny glows)
      // (by its color before the fade: the game fades its lamps out a few hundred meters away)
      const float raw_peak = std::max(raw.color[0], std::max(raw.color[1], raw.color[2]));
      const bool lamp_sized = raw.size > 0.6f * kMeter && raw.size < 12.f * kMeter;
      if (lamp_sized && raw_peak > 0.f && dist < 2500.f * kMeter) {
        // (its light from the lamp's remembered peak: steady through the glow's pulse, and the
        // same as when its glow is off screen and the light comes from memory)
        if (const auto* mem = remember_lamp(raw.pos, raw.size, raw.color, raw.fade)) {
          l.size = mem->size;
          for (int k = 0; k < 3; k++) {
            l.color[k] = mem->color[k] * mem->hold * raw.fade;
          }
          radius = std::clamp(l.size * m_settings.light_reach, glow_lo,
                              std::max(glow_lo, t_glow_max * kMeter));
          c.pos[3] = radius;
        }
      }
      if (dist > 40.f * kMeter && dist < 2500.f * kMeter && lamp_sized) {
        float peak = raw_peak;
        if (peak > 0.f && c.pos[2] > 0.f) {
          Beacon b;
          std::memcpy(b.pos, c.pos, sizeof(float) * 3);
          b.pos[3] = raw.size;
          for (int k = 0; k < 3; k++) {
            b.color[k] = raw.color[k] / peak;
          }
          b.color[3] = std::clamp((dist / kMeter - 40.f) / 40.f, 0.f, 1.f);
          b.score = (raw.color[0] + raw.color[1] + raw.color[2]) / (dist / kMeter);
          beacons.push_back(b);
        }
      }
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
      // (fading out from 110 to 150 m: at the hard cut a whole street's lamps switched on at once)
      const float far_fade = 1.f - std::clamp((dist / kMeter - 110.f) / 40.f, 0.f, 1.f);
      float intensity = std::min(1.5f, bright * 4.f) * m_settings.light_strength *
                        (1.f - 0.85f * glow_day) * m_grade.glows * far_fade;
      // a fire's glow: orange to yellow, no blue, flame sized (shots are pure red or purple, the
      // far beacons and the sky's glows are bigger), and staying put
      // (Spargus palace's braziers draw only a wide halo, 8-18 m: a fire too, its flame the size
      // of a small one, or the grade took the flame's raw yellow to a dim orange.
      // tuning.json "lights": {"fire_big_m": 0 = off})
      static const EnhancedTune t_fire_big("lights.fire_big_m", 20.f);
      const bool big_halo = l.size >= 6.f * kMeter;
      const bool warm = m_settings.fire && l.color[2] < 0.25f * l.color[0] &&
                        l.color[1] > 0.1f * l.color[0] && l.color[1] < 0.9f * l.color[0] &&
                        l.size > 0.3f * kMeter && l.size < std::max(6.f, (float)t_fire_big) * kMeter &&
                        dist < 60.f * kMeter;
      const float fire_size = big_halo ? std::min(0.2f * l.size, 2.5f * kMeter) : l.size;
      if (const auto* f = warm ? note_fire_glow(l.pos, fire_size) : nullptr) {
        c.instant = true;
        // its light flickers on top of the game's own (a seed from where it stands)
        float seed = std::fmod(std::abs(f->pos[0] * 0.013f + f->pos[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        float n = 0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                  0.2f * std::sin(t * 37.f + 2.3f * seed);
        intensity *= 0.82f + 0.18f * n;
      }
      // a glow of one pure hue (a red warning lamp, an indicator): its light stays small and
      // soft, or a few of them flood a whole district in red (Haven's industrial district)
      {
        float lo = std::min(l.color[0], std::min(l.color[1], l.color[2]));
        float second = bright - peak - lo;
        float pure = std::clamp((0.3f - second / peak) / 0.2f, 0.f, 1.f);
        intensity *= 1.f - 0.65f * pure;
        c.pos[3] = radius * (1.f - 0.45f * pure);
      }
      // (a shot's violet: red and blue with no green, one bolt turned a whole square pink)
      if (l.color[1] < 0.5f * std::min(l.color[0], l.color[2])) {
        intensity *= 0.35f;
        c.pos[3] = std::min(c.pos[3], 8.f * kMeter);
        c.violet = true;
      }
      for (int k = 0; k < 3; k++) {
        c.color[k] = l.color[k] / peak * intensity;
      }
      {  // (tuning.json "lights": {"dump": <m>}: the glows within that many meters, every 7th, in the log)
        static const EnhancedTune t_dump("lights.dump", 0.f);
        static int dump_n = 0;
        if (t_dump > 0.f && dist < t_dump * kMeter && (dump_n++ % 7) == 0) {
          lg::info("[enhanced] glowlight d {:.1f} size {:.2f} raw {:.2f} {:.2f} {:.2f} fade {:.2f} -> {:.2f} {:.2f} {:.2f} r {:.1f}",
                   dist / kMeter, raw.size / kMeter, raw.color[0], raw.color[1], raw.color[2], raw.fade,
                   c.color[0], c.color[1], c.color[2], c.pos[3] / kMeter);
        }
      }
      // (0.25: a fire under a roof, its light reaches further across the floor, enh_composite's
      // tune_fire_floor; still lit as a lamp, with shadows)
      c.color[3] = (warm && m_enclosed) ? 0.25f : 0.f;
      c.score = bright * c.pos[3] / (dist + c.pos[3]);
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
    // (a fight's dark eco cloud is a dozen violet glows side by side: each faint, together they lit
    // Jak saturated magenta in the slums at 21h. Their summed light held to that of a few, and paler)
    // (tuning.json: "lights": {"violet_max", "violet_pale"}; violet_max 0: off)
    {
      static const EnhancedTune t_violet_max("lights.violet_max", 2.5f);
      static const EnhancedTune t_violet_pale("lights.violet_pale", 0.5f);
      int nv = 0;
      for (const auto& c : candidates) {
        nv += c.violet ? 1 : 0;
      }
      if (t_violet_max > 0.f && nv > 0) {
        const float k = std::min(1.f, t_violet_max / (float)nv);
        const float pale = std::clamp((float)t_violet_pale, 0.f, 1.f);
        for (auto& c : candidates) {
          if (!c.violet) {
            continue;
          }
          const float y = 0.3f * c.color[0] + 0.59f * c.color[1] + 0.11f * c.color[2];
          for (int i = 0; i < 3; i++) {
            c.color[i] = (c.color[i] + (y - c.color[i]) * pale) * k;
          }
        }
      }
    }    // the far lamps the game no longer draws: beacons from memory (the ones that stayed put for two
    // seconds; one gone from right in front of the camera was put out or destroyed: forgotten)
    const float mem_ix = 1.f / std::max(std::abs(m_last_proj[2]), 1e-3f);
    const float mem_iy = 1.f / std::max(std::abs(m_last_proj[3]), 1e-3f);
    for (size_t i = 0; on && i < m_lamps.size();) {
      auto& l = m_lamps[i];
      float v[3] = {0.f, 0.f, 0.f};
      for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
          v[col] += (l.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      double unseen = m_time - l.last;
      bool steady = l.last - l.first >= 2.0;
      // (in view the game would draw its glow; off view it never does, so a lamp is forgotten only in
      // view: turning the camera no longer puts out the lamps behind it)
      const bool in_view =
          v[2] > 0.f && std::abs(v[0]) < v[2] * mem_ix && std::abs(v[1]) < v[2] * mem_iy;
      // (a lamp hidden behind a building may not be drawn by the game either: forgotten in view only
      // near, where its glow can't be hidden for long; further out after a minute)
      const bool forget_in_view =
          in_view && ((dist < 30.f * kMeter && unseen > 3.0) || unseen > 60.0);
      if (in_view && unseen > 0.1) {
        l.dark_on_screen = true;  // (back later: a blink)
      }
      if (forget_in_view || (!steady && unseen > 1.0) || unseen > 900.0) {
        if (forget_in_view && steady) {
          g_light_stab.forgot++;
        }
        m_lamps[i] = m_lamps.back();
        m_lamps.pop_back();
        continue;
      }
      i++;
      // a lamp the game doesn't draw this frame (its glow off screen) still lights what's in view
      // around it: its light no longer pops in when its glow comes on screen (the same light as its
      // glow's: its peak, the game's distance fade)
      static const EnhancedTune t_mem_min("lights.glow_min_m", 4.f);
      static const EnhancedTune t_mem_max("lights.glow_max_m", 20.f);
      const float mlo = t_mem_min * kMeter;
      const float mradius =
          std::clamp(l.size * m_settings.light_reach, mlo, std::max(mlo, t_mem_max * kMeter));
      if (steady && unseen > 0.0 && dist < 150.f * kMeter && v[2] > -mradius &&
          !(in_view && l.blinking(m_time))) {
        float lc[3];
        for (int k = 0; k < 3; k++) {
          lc[k] = l.color[k] * l.hold * l.fade;
        }
        const float lpeak = std::max(lc[0], std::max(lc[1], lc[2]));
        const float lbright = lc[0] + lc[1] + lc[2];
        const bool violet = lc[1] < 0.5f * std::min(lc[0], lc[2]);
        if (lpeak > 0.f && !violet) {
          const float radius = mradius;
          const float far_fade = 1.f - std::clamp((dist / kMeter - 110.f) / 40.f, 0.f, 1.f);
          float intensity = std::min(1.5f, lbright * 4.f) * m_settings.light_strength *
                            (1.f - 0.85f * glow_day) * m_grade.glows * far_fade;
          const float lmin = std::min(lc[0], std::min(lc[1], lc[2]));
          const float pure = std::clamp((0.3f - (lbright - lpeak - lmin) / lpeak) / 0.2f, 0.f, 1.f);
          intensity *= 1.f - 0.65f * pure;
          if (intensity > 0.f) {
            Candidate c;
            c.mem = true;
            std::memcpy(c.pos, v, sizeof(float) * 3);
            c.pos[3] = radius * (1.f - 0.45f * pure);
            for (int k = 0; k < 3; k++) {
              c.color[k] = lc[k] / lpeak * intensity;
            }
            c.color[3] = 0.f;
            c.score = lbright * c.pos[3] / (dist + c.pos[3]);
            candidates.push_back(c);
          }
        }
      }
      if (!steady || unseen <= 0.0 || dist < 40.f * kMeter || dist > 2500.f * kMeter || v[2] <= 0.f) {
        continue;
      }
      float peak = std::max(l.color[0], std::max(l.color[1], l.color[2]));
      Beacon b;
      std::memcpy(b.pos, v, sizeof(float) * 3);
      b.pos[3] = l.size;
      for (int k = 0; k < 3; k++) {
        b.color[k] = l.color[k] / peak;
      }
      b.color[3] = std::clamp((dist / kMeter - 40.f) / 40.f, 0.f, 1.f);
      b.score = (l.color[0] + l.color[1] + l.color[2]) / (dist / kMeter);
      beacons.push_back(b);
    }
    // explosions: a bright flash fading in half a second (sorted first: the score is huge)
    {
      std::lock_guard<std::mutex> lock(g_enhanced_explosions.mutex);
      for (int i = 0; i < g_enhanced_explosions.count; i++) {
        const float* e = g_enhanced_explosions.pending[i];
        if (m_blasts.size() < 16) {
          Blast b{{e[0], e[1], e[2]}, std::max(e[3], 1.f * kMeter), m_time};
          b.dir[0] = e[4];
          b.dir[1] = e[5];
          b.dir[2] = e[6];
          b.kind = (int)e[7];
          // (test switch "debug": {"blast_radius": m}: every explosion as one of that radius, to
          // look at a grenade's and a vehicle's size with the test explosion; 0: the game's own)
          static const EnhancedTune t_blast_radius("debug.blast_radius", 0.f);
          if (b.kind == 0 && (float)t_blast_radius > 0.f) {
            b.radius = (float)t_blast_radius * kMeter;
          }
          m_blasts.push_back(b);
        }
      }
      g_enhanced_explosions.count = 0;
    }
    {
      // the smoke of the last few explosions (kind 0) for the particle renderer: darker, thinner
      // soot that clears sooner (look.blast_punch 0: the game's grey puffs as they are)
      static const EnhancedTune t_punch("look.blast_punch", 1.f);
      static const EnhancedTune t_soot_life("look.blast_soot_life", 4.5f);
      // (its own memory: m_blasts forgets an explosion after 2.4 s, its smoke lingers longer)
      static std::vector<Blast> soot;
      for (const auto& b : m_blasts) {
        if (b.kind == 0 && m_time - b.start < 1.0 / 30.0) {
          if (std::none_of(soot.begin(), soot.end(),
                           [&](const Blast& s) { return s.start == b.start && s.pos[0] == b.pos[0]; })) {
            soot.push_back(b);
          }
        }
      }
      const float life = std::max((float)t_soot_life, 0.5f);
      std::erase_if(soot, [&](const Blast& b) { return m_time - b.start > life || m_time < b.start; });
      auto& sl = g_enhanced_smoke_lights;
      sl.soot_count = 0;
      // (not tied to the dynamic lights: the auto quality may turn those off, the smoke stays)
      if ((float)t_punch > 0.5f) {
        for (size_t k = soot.size(); k-- > 0 && sl.soot_count < EnhancedSmokeLights::kSoot;) {
          const auto& b = soot[k];
          const int i = sl.soot_count++;
          sl.soot_pos[i][0] = b.pos[0];
          sl.soot_pos[i][1] = b.pos[1];
          sl.soot_pos[i][2] = b.pos[2];
          sl.soot_pos[i][3] = std::clamp(b.radius * 3.f, 6.f * kMeter, 25.f * kMeter);
          sl.soot_age[i] = (float)(m_time - b.start) / life;
        }
      }
    }
    m_blasts.erase(std::remove_if(m_blasts.begin(), m_blasts.end(),
                                  [&](const Blast& b) {
                                    // gun blasts are short: free their slots for the next ones
                                    // (fx_detail: an explosion's dust and debris last longer)
                                    const double life = b.kind == 2   ? 0.3
                                                        : b.kind != 0 ? 0.7
                                                        : m_settings.fx_detail > 0.f ? 2.4
                                                                                     : 1.6;
                                    return m_time - b.start > life;
                                  }),
                   m_blasts.end());
    update_blast_fx(cam, on, w, h);
    // an explosion's wave through the grass: it spreads for a third of a second, then the grass
    // rises back
    g_enhanced_blast_push.count = 0;
    for (const auto& b : m_blasts) {
      const float age = (float)(m_time - b.start);
      if (b.kind != 0 || !on || age > 1.1f || g_enhanced_blast_push.count >= EnhancedBlastPush::kMax) {
        continue;
      }
      const float r = std::clamp(b.radius * 2.f, 8.f * kMeter, 20.f * kMeter);
      float* out = g_enhanced_blast_push.pos[g_enhanced_blast_push.count++];
      out[0] = b.pos[0];
      out[1] = b.pos[1];
      out[2] = b.pos[2];
      out[3] = std::max(r * std::min(age / 0.3f, 1.f) * std::clamp((1.1f - age) / 0.6f, 0.f, 1.f), 1.f);
    }
    for (const auto& b : m_blasts) {
      if (!on || m_settings.blast <= 0.f) {
        break;
      }
      float age = (float)(m_time - b.start);
      if (age > 0.6f) {
        continue;  // the flash is over (the sparks fly on)
      }
      Candidate c;
      c.instant = true;
      c.burst = true;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (b.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      c.pos[3] = std::clamp(b.radius * 4.f, 8.f * kMeter, 30.f * kMeter);
      // (look.blast_flash_fade: how fast an explosion's light dies, 1/s. At 5 it still stood at 0.9
      // after 0.3 s, past the composite's ceiling all that while: by day a flat orange pool over the
      // ground around the blast for a third of a second. A flash: full for a frame or two, then gone)
      static const EnhancedTune t_flash_fade("look.blast_flash_fade", 11.f);
      float k = 4.f * std::exp(-age * (b.kind == 0 ? std::max((float)t_flash_fade, 1.f) : 5.f)) *
                m_settings.blast * m_settings.light_strength;
      // the red gun: a short red-orange flash carried ahead of the muzzle with its wave
      // (the charged shot: red-fuchsia)
      const float tint[3] = {1.f, b.kind == 1 ? 0.32f : b.kind == 3 ? 0.2f : 0.62f,
                             b.kind == 1 ? 0.1f : b.kind == 3 ? 0.5f : 0.3f};
      if (b.kind == 1 || b.kind == 3) {
        if (age > 0.35f) {
          continue;
        }
        const float ahead = (1.f + 20.f * age) * kMeter;
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            c.pos[col] += b.dir[row] * ahead * cam.rot[row][col];
          }
        }
        c.pos[3] = std::max(11.f * kMeter, b.radius * 7.f);
        k = 5.f * std::exp(-age * 9.f) * m_settings.blast * m_settings.light_strength;
      } else if (b.kind == 2) {
        // charging: a small flicker on the gun, stronger as it charges (radius 0.5-1.5 m)
        if (age > 0.2f) {
          continue;
        }
        c.pos[3] = b.radius * 4.f;
        k = 1.5f * (b.radius / kMeter) * std::exp(-age * 12.f) * m_settings.blast *
            m_settings.light_strength;
      }
      c.color[0] = tint[0] * k;
      c.color[1] = tint[1] * k;
      c.color[2] = tint[2] * k;
      // (2: a flash, lit in the composite past the lamps' soft ceiling; the charging flicker a lamp)
      c.color[3] = b.kind == 2 ? 0.f : 2.f;
      c.score = 1e6f;
      candidates.push_back(c);
    }
    ENH_CPU_SPLIT("glows+blasts");
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
          c.color[k] =
              lc[k] * m_settings.light_strength * m_settings.lamp_strength * game_lamp_scale() * m_grade.lamps;
        }
        c.color[3] = 0.f;
        c.score = bright * radius / (dist + radius);
        candidates.push_back(c);
      }
    }
    ENH_CPU_SPLIT("level-lamps");
    // the fires and lamps the levels place (torches, braziers, gas lamps): each its own light,
    // stronger as the night falls, reaching further than the glows (a town's torches light its
    // gate from across the desert). Not where a glow already lights the same spot.
    {
      std::lock_guard<std::mutex> lock(g_enhanced_fires.mutex);
      if (g_enhanced_fires.count > 0) {
        m_fire_spawners.clear();
        for (int i = 0; i < g_enhanced_fires.count; i++) {
          const float* fp = g_enhanced_fires.pending[i];
          m_fire_spawners.push_back({fp[0], fp[1], fp[2], (float)g_enhanced_fires.kind[i]});
        }
        g_enhanced_fires.count = 0;
        m_fire_spawners_time = m_time;
      } else if (m_time - m_fire_spawners_time > 0.3) {
        m_fire_spawners.clear();
      }
    }
    // (fires' spawners the same under a roof)
    const float fire_night = std::max({m_night, night_darkness(), enclosed});
    const size_t glow_lights = candidates.size();
    for (const auto& fs : m_fire_spawners) {
      if (!on || !m_settings.fire) {
        break;
      }
      const int kind = (int)fs[3];
      // a little above the spawner: the flame, not its base; a fire's light wanders with its
      // flames (the shadows around it tremble)
      const float ft = (float)m_time + std::fmod(std::abs(fs[0] * 0.011f + fs[2] * 0.017f), 6.2832f);
      const float wander = kind == EnhancedFires::FIRE ? 0.12f * kMeter : 0.f;
      const float wp[3] = {fs[0] + wander * std::sin(ft * 7.3f) * std::sin(ft * 2.1f),
                           fs[1] + (kind == EnhancedFires::CANDLE ? 0.2f : 0.6f) * kMeter +
                               wander * 0.6f * std::sin(ft * 9.7f),
                           fs[2] + wander * std::cos(ft * 6.1f) * std::sin(ft * 1.7f)};
      Candidate c;
      c.instant = true;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (wp[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      const float reach = kind == EnhancedFires::CANDLE ? 4.f : (kind == EnhancedFires::FIRE ? 13.f : 11.f);
      // (reach and strength scales: at night a torch off screen laid its full 13 m of orange over half
      // of Spargus' street, where the game has blue moonlight)
      static const EnhancedTune t_fire_reach("lights.fire_reach", 0.8f);
      static const EnhancedTune t_fire_k("lights.fire_strength", 0.65f);
      c.pos[3] = reach * kMeter * (0.75f + 0.25f * m_settings.light_reach / 2.f) * t_fire_reach;
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // a placed fire close by also gets the embers and hot air (a torch's flame is often too
      // small a glow to be told a fire by its light)
      if (kind == EnhancedFires::FIRE && dist < 100.f * kMeter && c.pos[2] > -1.f * kMeter) {
        Fire v;
        std::memcpy(v.pos, c.pos, sizeof(float) * 3);
        v.pos[3] = 0.45f * kMeter;
        v.dist = dist;
        v.seed = std::fmod(std::abs(fs[0] * 0.013f + fs[2] * 0.007f), 6.2832f);
        fires.push_back(v);
      }
      bool lit_already = false;
      for (size_t j = 0; j < glow_lights && !lit_already; j++) {
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) {
          float dk = candidates[j].pos[k] - c.pos[k];
          d2 += dk * dk;
        }
        lit_already = d2 < 3.f * kMeter * 3.f * kMeter;
      }
      static const float kColors[4][3] = {
          {1.f, 0.55f, 0.22f}, {1.f, 0.82f, 0.55f}, {0.45f, 0.65f, 1.f}, {1.f, 0.7f, 0.35f}};
      const float* col3 = kColors[kind & 3];
      if (c.pos[2] > 0.f && dist > 40.f * kMeter && dist < 2500.f * kMeter) {
        Beacon b;
        std::memcpy(b.pos, c.pos, sizeof(float) * 3);
        b.pos[3] = 1.f * kMeter;
        std::memcpy(b.color, col3, sizeof(float) * 3);
        b.color[3] = std::clamp((dist / kMeter - 40.f) / 40.f, 0.f, 1.f);
        b.score = 2.f / (dist / kMeter);
        beacons.push_back(b);
      }
      if (lit_already || c.pos[2] < -c.pos[3] || dist > 400.f * kMeter) {
        continue;
      }
      // (by day a warm glow around the flame, at night far more: a torch's full 13 m of orange
      // light by day laid a bright orange pool on the sunlit sand)
      float k = (0.3f + 0.95f * fire_night) * m_settings.light_strength * m_grade.lamps * t_fire_k;
      if (kind == EnhancedFires::FIRE || kind == EnhancedFires::CANDLE) {
        float seed = std::fmod(std::abs(fs[0] * 0.013f + fs[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        k *= 0.8f + 0.2f * (0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                            0.2f * std::sin(t * 37.f + 2.3f * seed));
      }
      for (int i = 0; i < 3; i++) {
        c.color[i] = col3[i] * k * (kind == EnhancedFires::CANDLE ? 0.5f : 1.2f);
      }
      c.color[3] = 0.f;
      // far ones still count (their light on a gate across the sand), near ones first
      c.score = 1.5f * c.pos[3] / (dist + c.pos[3]);
      candidates.push_back(c);
    }
    ENH_CPU_SPLIT("placed-fires");
    // Jak's lantern: on foot or on the board in the desert as the night darkens, a Wastelander's
    // lantern bulb (Damas's people cross the sands by night with them), a little machine of its own:
    // it rises off Jak's shoulder as the dark comes, hovers up to his right and follows him on a
    // spring, lagging, swaying and settling, so its warm light shifts as he moves
    m_lantern[3] = 0.f;
    {
      const auto& pl = g_enhanced_player;
      const bool on_foot = pl.valid && pl.state != 2 && pl.state != 5 && pl.state != 7;
      // the first time Jak stands in the desert (once per install): night falls and the bulb rises,
      // with a word on it on the screen (request 10: the hour, the hint)
      if (!m_settings.lantern_intro_done && m_settings.desert_lamp && m_pond && pl.valid &&
          (pl.state == 1 || pl.state == 3) && !g_enhanced_scene.movie) {
        g_enhanced_menu.ask(10, "22");
        m_lamp_mode = 0;
        m_settings.lantern_intro_done = true;
        save_menu_settings();
      }
      // on by itself in the desert at night; holding triangle switches it on anywhere, any hour,
      // or off (switched off it stays off until the next desert night)
      const float auto_on = (m_settings.desert_lamp && m_pond)
                                ? std::clamp((std::max(m_night, night_darkness()) - 0.25f) / 0.4f, 0.f, 1.f)
                                : 0.f;
      if (pl.valid && pl.lamp_toggles != m_lamp_toggles_seen) {
        if (m_lamp_toggles_seen >= 0) {
          const bool lit = m_lamp_mode == 1 || (m_lamp_mode == 0 && auto_on > 0.5f);
          m_lamp_mode = lit ? 2 : 1;
        }
        m_lamp_toggles_seen = pl.lamp_toggles;
      }
      if (m_lamp_mode == 2 && auto_on <= 0.f) {
        m_lamp_mode = 0;
      }
      const float lit = m_lamp_mode == 1 ? 1.f : (m_lamp_mode == 2 ? 0.f : auto_on);
      const float want = on_foot ? lit : 0.f;
      m_lamp_on += (want - m_lamp_on) * 0.05f;
      const float dt = std::clamp((float)(m_time - m_bulb_time), 0.f, 0.1f);
      m_bulb_time = m_time;
      if (on && pl.valid && m_lamp_on > 0.01f) {
        float fx = cam.rot[0][2], fz = cam.rot[2][2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1e-3f);
        fx /= len;
        fz /= len;
        // the camera's right, flat
        const float rx = -fz, rz = fx;
        const float t = (float)m_time;
        // its spot: up to Jak's right and a little behind, drifting on its own
        const float drift_side = 0.18f * std::sin(t * 0.63f) + 0.07f * std::sin(t * 1.9f + 0.4f);
        const float drift_up = 0.14f * std::sin(t * 0.91f + 1.1f) + 0.05f * std::sin(t * 2.7f);
        const float drift_fwd = 0.15f * std::sin(t * 0.47f + 2.3f);
        // (far enough to the side that trailing him at a run doesn't bring it over him)
        const float side = 1.7f + drift_side, up = 2.35f + drift_up, fwd = -0.3f + drift_fwd;
        // its place next to Jak (m), where it heads
        const float target[3] = {rx * side + fx * fwd, up, rz * side + fz * fwd};
        // Jak's velocity (m/s): the bulb keeps up with him, but his starts, stops and turns jolt it
        float jv[3] = {0.f, 0.f, 0.f};
        if (m_bulb_out && dt > 0.f) {
          for (int k = 0; k < 3; k++) {
            jv[k] = (pl.pos[k] - m_bulb_jak[k]) / kMeter / dt;
          }
        }
        const float jump2 = jv[0] * jv[0] + jv[1] * jv[1] + jv[2] * jv[2];
        if (!m_bulb_out || jump2 > 40.f * 40.f) {
          // it starts on Jak's shoulder (or catches up after a teleport)
          m_bulb_off[0] = rx * 0.3f;
          m_bulb_off[1] = 1.5f;
          m_bulb_off[2] = rz * 0.3f;
          for (int k = 0; k < 3; k++) {
            m_bulb_vel[k] = 0.f;
            m_bulb_jv[k] = 0.f;
            jv[k] = 0.f;
          }
          m_bulb_out = true;
        }
        // a soft spring, near critical damping: it trails Jak a little as he moves (half a meter at a
        // run), sways a touch when he starts, stops or turns, settles without bouncing
        const float kSpring = 10.f, kDamp = 5.6f;
        for (int k = 0; k < 3; k++) {
          m_bulb_vel[k] -= (jv[k] - m_bulb_jv[k]) * 0.12f;
          m_bulb_jv[k] += (jv[k] - m_bulb_jv[k]) * std::min(1.f, dt * 6.f);
          const float trail = std::clamp(-m_bulb_jv[k] * 0.08f, -0.8f, 0.8f);
          float a = kSpring * (target[k] + trail - m_bulb_off[k]) - kDamp * m_bulb_vel[k];
          m_bulb_vel[k] += a * dt;
          m_bulb_off[k] += m_bulb_vel[k] * dt;
          m_bulb_jak[k] = pl.pos[k];
          m_bulb_pos[k] = pl.pos[k] + m_bulb_off[k] * kMeter;
        }
        Candidate c;
        for (int col = 0; col < 3; col++) {
          c.pos[col] = 0.f;
          for (int row = 0; row < 3; row++) {
            c.pos[col] += (m_bulb_pos[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        // (its reach and strength: at 14 m and full strength the whole foreground of a desert night went
        // salmon where the game has blue moonlight, a pool around Jak is enough)
        static const EnhancedTune t_lan_r("lights.lantern_radius_m", 9.f);
        static const EnhancedTune t_lan_k("lights.lantern_strength", 0.6f);
        c.pos[3] = t_lan_r * kMeter;
        // a steady bulb with a faint hum in it
        const float hum = 0.97f + 0.02f * std::sin(t * 17.f) * std::sin(t * 3.3f) +
                          0.01f * std::sin(t * 41.f);
        const float k = m_lamp_on * hum * m_settings.light_strength * t_lan_k;
        c.color[0] = 1.25f * k;
        c.color[1] = 0.62f * k;
        c.color[2] = 0.22f * k;
        c.color[3] = 1.f;  // with Jak: no shadow of him from it
        c.score = 100.f;  // always among the lights
        candidates.push_back(c);
        std::memcpy(m_lantern, c.pos, sizeof(float) * 3);
        m_lantern[3] = m_lamp_on * hum;
      } else if (m_lamp_on <= 0.01f) {
        m_bulb_out = false;  // next time it rises off the shoulder again
      }
    }
    ENH_CPU_SPLIT("lantern");
    // the lights placed by hand (lights.json), the night ones lit as the night falls
    for (const auto& ml : g_manual_lights[game_index()]) {
      const float lit = ml.night ? std::max(m_night, night_darkness()) : 1.f;
      if (!on || lit <= 0.01f) {
        continue;
      }
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (ml.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      c.pos[3] = std::min(ml.radius, 40.f * kMeter);
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // it has no bulb of its own in the game: its beacon is the lamp, near or far
      if (ml.bulb && c.pos[2] > 0.f && dist < 2500.f * kMeter) {
        float peak = std::max(ml.color[0], std::max(ml.color[1], ml.color[2]));
        if (peak > 0.f) {
          Beacon b;
          std::memcpy(b.pos, c.pos, sizeof(float) * 3);
          b.pos[3] = 0.4f * kMeter;
          for (int k = 0; k < 3; k++) {
            b.color[k] = ml.color[k] / peak;
          }
          b.color[3] = lit;
          b.score = 3.f * peak / std::max(dist / kMeter, 1.f);
          beacons.push_back(b);
        }
      }
      if (c.pos[2] < -c.pos[3] || dist > 150.f * kMeter) {
        continue;
      }
      float k = lit * m_settings.light_strength * m_settings.lamp_strength;
      if (ml.flicker) {
        c.instant = true;
        float seed = std::fmod(std::abs(ml.pos[0] * 0.013f + ml.pos[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        k *= 0.82f + 0.18f * (0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                              0.2f * std::sin(t * 37.f + 2.3f * seed));
      }
      for (int i = 0; i < 3; i++) {
        c.color[i] = ml.color[i] * ml.intensity * k;
      }
      c.color[3] = 0.f;
      c.score = (ml.color[0] + ml.color[1] + ml.color[2]) * ml.intensity * c.pos[3] / (dist + c.pos[3]);
      candidates.push_back(c);
    }
    ENH_CPU_SPLIT("manual");
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    g_light_stab.cand = std::max(g_light_stab.cand, (int)candidates.size());
    // Steady lights. Every light is followed from frame to frame by its place in the world (a track:
    // its smoothed color and reach, its fade, its shadow), whether in view or not, so:
    // - one that leaves the view and comes back (the camera turning) is back at full strength, and
    //   one coming into view from off screen is lit already (the view only decides what's uploaded);
    // - at most kLights are uploaded: a light losing its slot fades out in it, a new one fades in,
    //   none is ever dropped at full strength (Haven at night has 200+ lights in view);
    // - the shadow casters (the first 8 the composite looks at) are a set of their own, chosen with
    //   hysteresis: a lamp's shadow fades in and out with it (lamp_shadow_slot carries its weight),
    //   never switching on or off in one frame as the lights' order changes.
    // Explosions and fires (instant) are lit at once as before.
    // (tuning.json "lights": {"fade_in_s", "fade_in_near_s", "fade_out_s", "smooth_s", "keep_bonus",
    // "dwell_s", "caster_keep", "caster_dwell_s", "shadow_fade_s", "map_fade_s", "track_m"})
    struct LightTrack {
      u32 id = 0;
      float world[3] = {0.f, 0.f, 0.f};
      float color[4] = {0.f, 0.f, 0.f, 0.f};  // smoothed, before the fade
      float radius = 0.f;                       // smoothed
      float w = 0.f;                            // fade in/out
      float score = 0.f;
      float sw = 0.f;    // its shadow's weight (a caster)
      float mapw = 0.f;  // its shadow map's share of its shadow
      float prev_l = -1.f;  // r + g + b as uploaded last frame (-1: not uploaded)
      float vel[3] = {0.f, 0.f, 0.f};  // world units a second (a shot, a car's lights)
      float dist = 0.f;                // from the camera
      int map_slot = -1;
      double seen = 0.0, shown_since = 0.0, caster_since = 0.0, view_t = 0.0;
      int cand = -1;  // this frame's candidate
      bool instant = false, flash = false, lamp = false, in_view = false, shown = false;
      bool caster = false, leaving = false, target = false;
      bool mem = false;  // its light from the lamp's memory last
      bool moving = false;
      float cimp = -1.f;     // caster importance: eased, seen from ahead of the camera (-1: unset)
      float lpeak = -1.f;    // r + g + b held at its peak (lights.caster_peak_s; -1: unset)
      bool ever_moved = false;  // it moved once (a car's light): never a caster
      float sw_fade = 0.f;   // its shadow's fade in/out time now (s, 0: lights.shadow_fade_s)
      double view_since = -1.0;  // in view since (-1: out of view)
      bool map_want = false;     // its shadow map wanted last frame (as a caster or prewarmed)
    };
    static std::vector<LightTrack> tracks;
    std::vector<int> up_track;  // per uploaded light, its track
    const auto track_t0 = std::chrono::steady_clock::now();
    {
      static u32 next_id = 1;
      static double last_t = -1.0;
      static float last_cam[3] = {0.f, 0.f, 0.f};
      const bool last_t_was_unset = last_t < 0.0;
      const double raw_dt = last_t_was_unset ? 0.0 : m_time - last_t;
      last_t = m_time;
      const float dt = (float)std::clamp(raw_dt, 0.0, 0.25);
      const float cam_jump = std::sqrt((cam.trans[0] - last_cam[0]) * (cam.trans[0] - last_cam[0]) +
                                       (cam.trans[1] - last_cam[1]) * (cam.trans[1] - last_cam[1]) +
                                       (cam.trans[2] - last_cam[2]) * (cam.trans[2] - last_cam[2]));
      // (the camera's eased velocity, for the casters' look ahead: ~0.5 s)
      static float cam_vel[3] = {0.f, 0.f, 0.f};
      float cam_step[3];
      for (int k = 0; k < 3; k++) {
        cam_step[k] = cam.trans[k] - last_cam[k];
      }
      std::memcpy(last_cam, cam.trans, sizeof(last_cam));
      // (the first frame, or the camera cut or teleported: all lights as they are, at once)
      const bool cut = last_t_was_unset || cam_jump > 20.f * kMeter;
      for (int k = 0; k < 3; k++) {
        cam_vel[k] = (cut || dt <= 0.f)
                         ? 0.f
                         : cam_vel[k] + (cam_step[k] / dt - cam_vel[k]) * std::min(1.f, dt / 0.5f);
      }
      static const EnhancedTune t_fade_in("lights.fade_in_s", 0.4f);
      static const EnhancedTune t_fade_near("lights.fade_in_near_s", 0.15f);
      static const EnhancedTune t_fade_out("lights.fade_out_s", 0.5f);
      static const EnhancedTune t_smooth("lights.smooth_s", 0.25f);
      static const EnhancedTune t_keep("lights.keep_bonus", 0.8f);
      static const EnhancedTune t_dwell("lights.dwell_s", 1.f);
      // (the casters' set, see below. Old behaviour (before the steadier set): caster_keep 2,
      // caster_dwell_s 2, caster_out_s 1.5, caster_swap_s 0, caster_swap_fade_s 0.5,
      // caster_smooth_s 0, caster_lead_s 0, caster_near_m 0, caster_out_keep 0, caster_max_m 0,
      // caster_join_s 0, map_prewarm 0)
      static const EnhancedTune t_ckeep("lights.caster_keep", 3.f);
      static const EnhancedTune t_cdwell("lights.caster_dwell_s", 5.f);
      static const EnhancedTune t_cswap("lights.caster_swap_s", 3.f);
      static const EnhancedTune t_cswap_fade("lights.caster_swap_fade_s", 1.2f);
      static const EnhancedTune t_csmooth("lights.caster_smooth_s", 1.f);
      static const EnhancedTune t_clead("lights.caster_lead_s", 1.5f);
      static const EnhancedTune t_cnear("lights.caster_near_m", 15.f);
      static const EnhancedTune t_cout_keep("lights.caster_out_keep", 3.f);
      static const EnhancedTune t_cmax("lights.caster_max_m", 60.f);
      static const EnhancedTune t_cjoin("lights.caster_join_s", 3.5f);
      static const EnhancedTune t_sfade("lights.shadow_fade_s", 0.5f);
      static const EnhancedTune t_track_m("lights.track_m", 1.5f);
      static const EnhancedTune t_moving("lights.moving_mps", 3.f);
      static const EnhancedTune t_moving_in("lights.moving_fade_s", 0.15f);
      static const EnhancedTune t_near_bonus("lights.near_bonus", 1.f);
      static const EnhancedTune t_cout("lights.caster_out_s", 4.f);
      // (steadier still, for the signs at Hip Hog: casters keep their light's slot; importance by
      // the light's peak over lights.caster_peak_s, not its blinking value. Old: 0 and 0)
      static const EnhancedTune t_chold("lights.caster_hold", 1.f);
      static const EnhancedTune t_cpeak("lights.caster_peak_s", 8.f);
      // (and a lamp challenges only after lights.caster_settle_s in view, never a light that moved:
      // at Hip Hog the traffic's lights 13-18 m off came every second, each joined as "near Jak" and
      // pushed a lamp out after its 1 s dwell. Old: 0 and caster_moved 1)
      static const EnhancedTune t_csettle("lights.caster_settle_s", 1.f);
      static const EnhancedTune t_cmoved("lights.caster_moved", 0.f);
      const float rate_in = dt / std::max((float)t_fade_in, 0.01f);
      const float rate_in_near = dt / std::max((float)t_fade_near, 0.01f);
      const float rate_out = dt / std::max((float)t_fade_out, 0.01f);
      const float k_smooth = std::min(1.f, dt / std::max((float)t_smooth, 0.01f));
      // only what the reach (and halo, 1.5x) of touches the view is uploaded: every pixel loops over it
      const float ix = 1.f / std::max(std::abs(m_last_proj[2]), 1e-3f);
      const float iy = 1.f / std::max(std::abs(m_last_proj[3]), 1e-3f);
      const float nx = 1.f / std::sqrt(1.f + ix * ix), ny = 1.f / std::sqrt(1.f + iy * iy);
      // (the reach as the composite gets it, grown in the dark, and its halo)
      const float reach_x = 1.5f * (1.f + 0.6f * night_darkness());
      // (a light's change held to lights.max_luma_per_s of r+g+b a second: a bright one fading in
      // over 0.15 s, or a big step eased over 0.25 s, still lit a street in a few frames. Never
      // slower than 2 s for a fade, 1 s for a step)
      static const EnhancedTune t_slew("lights.max_luma_per_s", 3.f);
      const float max_dl = std::min(std::max((float)t_slew, 0.1f) * dt, 0.06f);  // (a long frame too)
      auto lum = [](const float* c) { return std::max(c[0] + c[1] + c[2], 1e-4f); };
      auto to_view = [&](const float* w, float* v) {
        for (int col = 0; col < 3; col++) {
          v[col] = 0.f;
          for (int row = 0; row < 3; row++) {
            v[col] += (w[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
      };
      auto touches = [&](const float* v, float r) {
        return v[2] > -r && (std::abs(v[0]) - v[2] * ix) * nx <= r &&
               (std::abs(v[1]) - v[2] * iy) * ny <= r;
      };
      auto on_screen = [&](const float* v) {
        return v[2] > 0.f && std::abs(v[0]) < v[2] * ix && std::abs(v[1]) < v[2] * iy;
      };
      const size_t nc = candidates.size();
      std::vector<float> cworld(nc * 3);
      std::vector<char> cview(nc);
      int n_in_view = 0;
      for (size_t i = 0; i < nc; i++) {
        const auto& c = candidates[i];
        float* wp = &cworld[i * 3];
        for (int row = 0; row < 3; row++) {
          wp[row] = cam.trans[row];
          for (int col = 0; col < 3; col++) {
            wp[row] += cam.rot[row][col] * c.pos[col];
          }
        }
        cview[i] = touches(c.pos, c.pos[3] * reach_x) ? 1 : 0;
        n_in_view += cview[i];
      }
      g_light_stab.inview = std::max(g_light_stab.inview, n_in_view);
      g_light_stab.over32 += n_in_view > kLights ? 1 : 0;
      // each candidate to the nearest track (each used once, nearest pairs first)
      for (auto& t : tracks) {
        t.cand = -1;
        t.prev_l = t.shown ? lum(t.color) * t.w : -1.f;
      }
      std::vector<int> cmatch(nc, -1);
      {
        // (where each track's light should be now: a moving one (a shot, a car's lights) moved on
        // since last frame, matched there, not lost and lit again from nothing every few frames)
        const float tm = std::clamp((float)t_track_m, 0.1f, 4.f) * kMeter;
        std::vector<float> pred(tracks.size() * 3);
        std::vector<float> reach(tracks.size());
        for (size_t j = 0; j < tracks.size(); j++) {
          const auto& t = tracks[j];
          float step = 0.f;
          for (int k = 0; k < 3; k++) {
            pred[j * 3 + k] = t.world[k] + t.vel[k] * dt;
            step += t.vel[k] * t.vel[k];
          }
          reach[j] = tm + std::min(std::sqrt(step) * dt * 0.5f, 4.f * kMeter);
        }
        auto hue_like = [&](const float* a, const float* b) {
          const float ab = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
          const float aa = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
          const float bb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
          return ab > 0.f && ab * ab > 0.81f * aa * bb;  // (within ~25 degrees of color)
        };
        for (int pass = 0; pass < 2; pass++) {
          // (pass 1: the lights seen last frame and not found near where they should be, to a new
          // one of their color within 8 m: a fast shot keeps its light, not a new one at nothing)
          std::vector<std::tuple<float, int, int>> pairs;
          for (size_t i = 0; i < nc; i++) {
            if (cmatch[i] >= 0) {
              continue;
            }
            const float* a = &cworld[i * 3];
            for (size_t j = 0; j < tracks.size(); j++) {
              const auto& t = tracks[j];
              // (a flash is its own light, never a lamp's next to it)
              if (t.cand >= 0 || t.flash != candidates[i].burst) {
                continue;
              }
              const float* b = &pred[j * 3];
              const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
              const float d2 = dx * dx + dy * dy + dz * dz;
              if (pass == 0 ? d2 < reach[j] * reach[j]
                            : (d2 < 64.f * kMeter * kMeter && m_time - t.seen <= raw_dt + 1e-4 &&
                               !t.mem && !candidates[i].mem && hue_like(t.color, candidates[i].color))) {
                pairs.emplace_back(d2, (int)i, (int)j);
              }
            }
          }
          std::sort(pairs.begin(), pairs.end());
          for (const auto& [d2, i, j] : pairs) {
            if (cmatch[i] < 0 && tracks[j].cand < 0) {
              cmatch[i] = j;
              tracks[j].cand = i;
            }
          }
        }
      }
      // the tracks whose light is gone: a light still uploaded fades out in its place; one out of
      // view waits a while (its glow off screen, the game may not draw it: back at full strength if
      // it comes back); one in view and not shown, or a fire or a flash, goes at once
      for (size_t j = 0; j < tracks.size();) {
        auto& t = tracks[j];
        bool drop = false;
        if (t.cand < 0) {
          float v[3];
          to_view(t.world, v);
          t.in_view = touches(v, t.radius * reach_x);
          if (t.flash || cut) {
            drop = true;  // (a flash is over at once)
          } else if (t.shown && t.in_view) {
            t.target = false;
            drop = t.w <= 0.f;  // (uploaded at nothing last frame)
          } else {
            drop = m_time - t.seen > 10.0 || (on_screen(v) && m_time - t.seen > 0.5) ||
                   (t.in_view && t.w <= 0.f);
          }
        }
        if (drop) {
          tracks[j] = tracks.back();
          tracks.pop_back();
          // (a swap moved the last track here: fix its candidate's link)
          if (j < tracks.size() && tracks[j].cand >= 0) {
            cmatch[tracks[j].cand] = (int)j;
          }
          continue;
        }
        j++;
      }
      // new lights: from nothing (out of view, or after a cut: as they are)
      for (size_t i = 0; i < nc; i++) {
        if (cmatch[i] >= 0) {
          continue;
        }
        const auto& c = candidates[i];
        LightTrack t;
        t.id = next_id++;
        std::memcpy(t.world, &cworld[i * 3], sizeof(t.world));
        std::memcpy(t.color, c.color, sizeof(t.color));
        t.radius = c.pos[3];
        t.flash = c.burst;
        t.w = (cut || !cview[i] || c.burst) ? 1.f : 0.f;
        t.view_t = m_time;
        tracks.push_back(t);
        cmatch[i] = (int)tracks.size() - 1;
        tracks.back().cand = (int)i;
      }
      // the lights seen this frame: their place, smoothed color and reach
      for (auto& t : tracks) {
        if (t.cand < 0) {
          continue;
        }
        const auto& c = candidates[t.cand];
        {
          // (follows its light, a moving one at once; only a glow handed over to the lamp's memory or
          // back (a little off) is eased over a few frames when more than half a meter off)
          const float* cw = &cworld[t.cand * 3];
          const float jx = cw[0] - t.world[0], jy = cw[1] - t.world[1], jz = cw[2] - t.world[2];
          const bool handover = t.mem != c.mem && jx * jx + jy * jy + jz * jz > 0.25f * kMeter * kMeter;
          const float kw = (cut || !handover) ? 1.f : 0.2f;
          float speed2 = 0.f;
          for (int k = 0; k < 3; k++) {
            const float d = (cw[k] - t.world[k]) * kw;
            t.world[k] += d;
            if (handover || cut || dt <= 0.f || t.mem || c.mem) {
              t.vel[k] = 0.f;
            } else {
              t.vel[k] += (d / dt - t.vel[k]) * 0.5f;
            }
            speed2 += t.vel[k] * t.vel[k];
          }
          if (speed2 > 300.f * kMeter * 300.f * kMeter) {
            for (float& v : t.vel) {
              v = 0.f;  // (a respawn, a teleported glow: not a speed)
            }
            speed2 = 0.f;
          }
          t.moving = speed2 > (float)t_moving * (float)t_moving * kMeter * kMeter;
          t.ever_moved |= t.moving;
          t.mem = c.mem;
        }
        t.seen = m_time;
        t.instant = c.instant;
        t.flash = c.burst;
        t.lamp = c.color[3] < 0.5f;
        t.score = c.score;
        if (c.instant || cut || dt <= 0.f) {
          std::memcpy(t.color, c.color, sizeof(t.color));
          t.radius = c.pos[3];
        } else {
          float ks = k_smooth;
          const float dl = std::abs(c.color[0] + c.color[1] + c.color[2] - t.color[0] - t.color[1] -
                                    t.color[2]) * t.w;
          if (dl * ks > max_dl && !t.moving) {
            ks = std::max(max_dl / dl, std::min(1.f, dt));
          }
          for (int k = 0; k < 4; k++) {
            t.color[k] += (c.color[k] - t.color[k]) * ks;
          }
          t.radius += (c.pos[3] - t.radius) * k_smooth;
        }
        {
          // (in view by where and how far it is uploaded: its eased place and reach)
          float v[3];
          to_view(t.world, v);
          t.in_view = touches(v, t.radius * reach_x);
          t.dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }
        if (t.in_view) {
          t.view_t = m_time;
        }
      }
      // the slots: the best in view (a light shown already counts keep_bonus more) wanted; one shown
      // and no longer wanted (after dwell_s) fades out in its slot; a wanted one gets a slot only
      // when one is free. (kLights - 4 wanted: room for the fading and a sudden flash)
      const int k_target = kLights - 4;
      std::vector<int> order;
      for (size_t j = 0; j < tracks.size(); j++) {
        auto& t = tracks[j];
        if (!t.in_view) {
          t.shown = false;  // (nothing of it shows: its slot is free, its fade kept)
        }
        if (t.cand >= 0 && t.in_view) {
          order.push_back((int)j);
        }
      }
      auto rank_score = [&](int j) {
        const auto& t = tracks[j];
        // (the lamps right around Jak first, their pools are what is seen: lights.near_bonus, 30 m)
        const float nearness = std::clamp(1.f - t.dist / (30.f * kMeter), 0.f, 1.f);
        return t.score * (1.f + (t.shown ? (float)t_keep * t.w : 0.f)) *
                   (1.f + std::max((float)t_near_bonus, 0.f) * nearness) +
               (t.flash ? 1e3f : 0.f);
      };
      std::stable_sort(order.begin(), order.end(),
                       [&](int a, int b) { return rank_score(a) > rank_score(b); });
      for (auto& t : tracks) {
        if (t.cand >= 0) {
          t.target = false;
        }
      }
      for (size_t r = 0; r < order.size() && (int)r < k_target; r++) {
        tracks[order[r]].target = true;
      }
      int n_shown = 0;
      for (auto& t : tracks) {
        if (t.shown && t.cand >= 0 && !t.target && m_time - t.shown_since < (float)t_dwell) {
          t.target = true;  // (not displaced right after it came)
        }
        // (a shadow caster keeps its slot (lights.caster_hold 1): with ~90 lights in view at Hip
        // Hog, the 28 slots churned by the frame's raw score and took the casters' lights with
        // them, each a seat lost and a new caster)
        if ((float)t_chold > 0.f && t.caster && !t.leaving && t.shown && t.cand >= 0 && t.in_view) {
          t.target = true;
        }
        n_shown += t.shown ? 1 : 0;
      }
      for (int j : order) {
        auto& t = tracks[j];
        if (!t.target || t.shown) {
          continue;
        }
        if (n_shown >= kLights) {
          if (!t.flash) {
            continue;  // (waits for a slot)
          }
          // a flash with no slot free: the faintest shown light makes room (rare)
          int worst = -1;
          for (size_t k = 0; k < tracks.size(); k++) {
            const auto& o = tracks[k];
            if (o.shown && !o.instant && (worst < 0 || o.w * (o.target ? 4.f : 1.f) <
                                                           tracks[worst].w * (tracks[worst].target ? 4.f : 1.f))) {
              worst = (int)k;
            }
          }
          if (worst < 0) {
            continue;
          }
          tracks[worst].shown = false;
          tracks[worst].caster = tracks[worst].leaving = false;
          tracks[worst].sw = 0.f;
          g_light_stab.capdrop++;
          n_shown--;
        }
        t.shown = true;
        t.shown_since = m_time;
        n_shown++;
      }
      // the fades
      for (auto& t : tracks) {
        if (t.flash) {
          t.w = 1.f;  // (fires and flickers fade in and out too: only their color is not eased)
          continue;
        }
        if (cut) {
          t.w = (t.target || !t.in_view) ? 1.f : 0.f;
          if (!t.target) {
            t.shown = false;
          }
        } else if (t.shown && t.target) {
          float v[3];
          to_view(t.world, v);
          const bool close = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 25.f * kMeter * 25.f * kMeter;
          // (a moving light, a shot or a car's lights, in at once (lights.moving_fade_s): it is
          // somewhere new every frame, no pop where something was lit)
          const float dw =
              t.moving ? dt / std::max((float)t_moving_in, 0.01f)
                       : std::max(std::min(close ? rate_in_near : rate_in, max_dl / lum(t.color)),
                                  0.5f * dt);
          t.w = std::min(1.f, t.w + dw);
        } else if (t.shown || t.in_view) {
          // (fading out in its slot, its slot freed once uploaded at nothing; or in view with no
          // slot: unseen, so its light starts from nothing when it gets one)
          if (t.shown && t.w <= 0.f) {
            t.shown = false;
          }
          // (in view with no slot: nothing at once, or it came in at its old strength when a slot
          // freed up, a pop where it shows)
          const float dw = t.shown ? std::max(std::min(rate_out, max_dl / lum(t.color)), 0.5f * dt)
                                   : 1.f;
          t.w = std::max(0.f, t.w - dw);
        }
        // (its color eased and its fade together: their sum held to max_luma_per_s too)
        if (!cut && !t.instant && !t.moving && t.shown && t.prev_l >= 0.f) {
          const float l = lum(t.color);
          const float lw = std::clamp(l * t.w, t.prev_l - max_dl, t.prev_l + max_dl);
          t.w = std::clamp(lw / l, 0.f, 1.f);
        }
      }
      // the shadow casters: at most 8 lamps shown, chosen with hysteresis so the set changes
      // seldom while walking (Haven at night: it changed every ~1-2 s, eased but visible):
      // - by importance (cimp): the lamp's eased light r+g+b x reach / (distance + reach), the
      //   lamps around Jak first (near_bonus, 30 m), seen from a point ahead of the camera
      //   (lights.caster_lead_s of its velocity: lamps ahead on the way join early, still faint),
      //   eased over lights.caster_smooth_s (no flicker, no frame-to-frame order changes);
      // - a member stays caster_dwell_s, and gives way only to one caster_keep times stronger, at
      //   most one such swap every lights.caster_swap_s (its shadow fading over
      //   caster_swap_fade_s, one leaving at a time);
      // - one out of view caster_out_s gives its seat, free of that budget, to one clearly stronger
      //   or one near Jak (lights.caster_near_m: the lamp right by him casts at once);
      // - a lamp's shadow comes at once when its light can't be seen changing (just lit, cut).
      {
        const float smooth_s = std::max((float)t_csmooth, 0.f);
        const bool old_score = smooth_s <= 0.f && (float)t_clead <= 0.f;
        const float k_imp = smooth_s > 0.f ? std::min(1.f, dt / smooth_s) : 1.f;
        const float lead = std::clamp((float)t_clead, 0.f, 5.f);
        const float near_u = std::max((float)t_cnear, 0.f) * kMeter;
        float focus[3];
        for (int k = 0; k < 3; k++) {
          focus[k] = cam.trans[k] + cam_vel[k] * lead;
        }
        auto focus_d = [&](const LightTrack& t) {
          const float x = t.world[0] - focus[0], y = t.world[1] - focus[1], z = t.world[2] - focus[2];
          return std::sqrt(x * x + y * y + z * z);
        };
        // (the lamp whose pool Jak stands in: within caster_near_m of him and of 80% of its reach.
        // By the camera alone, a dim lamp 14 m off counted "near" and waited a minute for a seat it
        // couldn't win; without Jak: the camera's look-ahead point)
        const auto& jpl = g_enhanced_player;
        auto in_pool = [&](const LightTrack& t) {
          float d = focus_d(t);
          if (jpl.valid) {
            const float x = t.world[0] - jpl.pos[0], y = t.world[1] - jpl.pos[1],
                        z = t.world[2] - jpl.pos[2];
            d = std::sqrt(x * x + y * y + z * z);
          }
          return d < near_u && d < 0.8f * t.radius;
        };
        for (auto& t : tracks) {
          if (t.in_view) {
            if (t.view_since < 0.0) {
              t.view_since = m_time;
            }
          } else {
            t.view_since = -1.0;
          }
          if (!t.lamp || t.flash || old_score) {
            continue;
          }
          // (a blinking sign or a cycling light by its peak, decaying over caster_peak_s: by its
          // value now it fell and rose in the ranking, seats swapped at every blink)
          const float peak_s = (float)t_cpeak;
          const float l_now = lum(t.color);
          if (peak_s <= 0.f || t.lpeak < 0.f || cut) {
            t.lpeak = l_now;
          } else {
            t.lpeak = std::max(l_now, t.lpeak - t.lpeak * std::min(1.f, dt / peak_s));
          }
          const float d = focus_d(t);
          const float nearness = std::clamp(1.f - d / (30.f * kMeter), 0.f, 1.f);
          const float imp = t.lpeak * t.radius / (d + std::max(t.radius, 1.f)) *
                            (1.f + std::max((float)t_near_bonus, 0.f) * nearness);
          t.cimp = (t.cimp < 0.f || cut) ? imp : t.cimp + (imp - t.cimp) * k_imp;
        }
        // (old: the frame's own score, by the camera)
        auto cscore = [&](const LightTrack& t) {
          if (old_score) {
            const float nearness = std::clamp(1.f - t.dist / (30.f * kMeter), 0.f, 1.f);
            return t.score * (1.f + std::max((float)t_near_bonus, 0.f) * nearness);
          }
          return std::max(t.cimp, 0.f);
        };
        const float sfade = std::max((float)t_sfade, 0.01f);
        const float swap_fade = std::max((float)t_cswap_fade, 0.01f);
        static double last_swap_t = -1e9, last_join_t = -1e9;
        int members = 0;
        for (auto& t : tracks) {
          if (!t.caster) {
            continue;
          }
          // (out of view a while, or its light gone: out of the set at once, nothing of it shows)
          if ((!t.shown && (m_time - t.view_t > 8.0 || t.w <= 0.02f)) || !t.lamp || t.flash) {
            t.caster = t.leaving = false;
            t.sw = 0.f;
            g_light_stab.cdrop++;
            continue;
          }
          members++;
        }
        // (lights.caster_max_m: no casters further: a shadow 100 m away can't be told, and the
        // game's far lamps come and go from its list, each time a new caster; 0: any distance.
        // A member 25% further gives its seat freely)
        const float max_u = std::max((float)t_cmax, 0.f) * kMeter;
        std::vector<int> challengers;
        for (size_t j = 0; j < tracks.size(); j++) {
          const auto& t = tracks[j];
          if (!t.caster && t.shown && t.in_view && t.lamp && !t.flash && t.cand >= 0 &&
              (t.target || t.instant) && (max_u <= 0.f || t.dist < max_u) &&
              ((float)t_cmoved > 0.5f || !t.ever_moved)) {
            challengers.push_back((int)j);
          }
        }
        std::sort(challengers.begin(), challengers.end(),
                  [&](int a, int b) { return cscore(tracks[a]) > cscore(tracks[b]); });
        bool swapping = false;
        const float keep = std::max((float)t_ckeep, 1.f);
        for (int j : challengers) {
          auto& t = tracks[j];
          // (a lamp right by Jak: its pool is what is seen)
          const bool by_jak = in_pool(t);
          // (in view long enough to be a lamp, not a blink or a passer-by; a lamp just lit or just
          // come into view still takes a free seat at once, its shadow with its light)
          const bool settled = cut || (t.view_since >= 0.0 && m_time - t.view_since >= (float)t_csettle);
          const bool just_lit = t.w * lum(t.color) < 0.05f || t.shown_since == m_time;
          if (!settled && !(just_lit && members < 8)) {
            continue;
          }
          if (members >= 8) {
            // a member out of view gives its place (free of the swap budget) to one clearly
            // stronger or near; else the weakest in view that has stayed long enough starts
            // leaving (one at a time, one every caster_swap_s), if the challenger is clearly
            // stronger
            int out = -1, weak = -1, too_far = -1;
            for (size_t k = 0; k < tracks.size(); k++) {
              const auto& o = tracks[k];
              if (!o.caster) {
                continue;
              }
              if (o.shown && !o.leaving && max_u > 0.f && o.dist > 1.25f * max_u &&
                  (too_far < 0 || o.dist > tracks[too_far].dist)) {
                too_far = (int)k;
              }
              // (out of view lights.caster_out_s: a camera swaying at a lamp's edge kept swapping
              // it; a near challenger doesn't wait that long)
              if (!o.shown) {
                if (m_time - o.view_t > (by_jak ? std::min((float)t_cout, 0.5f) : (float)t_cout) &&
                    (out < 0 || cscore(o) < cscore(tracks[out]))) {
                  out = (int)k;
                }
              } else if (!o.leaving && !(by_jak && in_pool(o)) &&
                         (weak < 0 || cscore(o) < cscore(tracks[weak]))) {
                weak = (int)k;
              }
            }
            if (out >= 0 && (by_jak || cscore(t) > std::max((float)t_cout_keep, 0.f) * cscore(tracks[out]))) {
              tracks[out].caster = tracks[out].leaving = false;
              tracks[out].sw = 0.f;
              members--;
              g_light_stab.cseat++;
            } else {
              bool leaving_now = false;
              for (const auto& o : tracks) {
                leaving_now |= o.caster && o.leaving;
              }
              // (a near one: no budget, a short dwell, only stronger; its seat freed quickly)
              const bool budget = by_jak || m_time - last_swap_t > (float)t_cswap;
              const float dwell = by_jak ? std::min((float)t_cdwell, 1.f) : (float)t_cdwell;
              if (!swapping && !leaving_now && too_far >= 0) {
                // (one too far to be told leaves first, free of the budget)
                tracks[too_far].leaving = true;
                tracks[too_far].sw_fade = swap_fade;
                swapping = true;
                g_light_stab.cweak++;
              } else if (!swapping && !leaving_now && weak >= 0 && budget &&
                  m_time - tracks[weak].caster_since > dwell &&
                  (by_jak || cscore(t) > keep * cscore(tracks[weak]))) {
                tracks[weak].leaving = true;
                tracks[weak].sw_fade = by_jak ? sfade : swap_fade;
                swapping = true;
                last_swap_t = m_time;
                g_light_stab.cweak++;
              }
              continue;
            }
          }
          // (a free seat: one join every lights.caster_join_s, the best challenger first; free for
          // one near Jak and one whose light can't be seen changing (just lit))
          const bool join_free =
              cut || by_jak || t.w * lum(t.color) < 0.05f || t.shown_since == m_time;
          if (!join_free && m_time - last_join_t < (float)t_cjoin) {
            continue;
          }
          if (!join_free) {
            last_join_t = m_time;
          }
          t.caster = true;
          t.leaving = false;
          t.caster_since = m_time;
          // (lit just now, or out of view until now: its shadow at once)
          t.sw = (cut || t.w * lum(t.color) < 0.05f || t.shown_since == m_time) ? 1.f : 0.f;
          t.sw_fade = by_jak ? sfade : swap_fade;
          t.mapw = 0.f;
          members++;
          g_light_stab.cjoin++;
          if (g_light_stab.events) {
            lg::info("[enhanced] lightstab+ join {:.1f} m imp {:.3f} near {} sw {:.2f} members {} id {} "
                     "moved {} view {:.1f} s",
                     t.dist / kMeter, cscore(t), by_jak ? 1 : 0, t.sw, members, t.id,
                     t.ever_moved ? 1 : 0, t.view_since >= 0.0 ? m_time - t.view_since : -1.0);
          }
        }
        for (auto& t : tracks) {
          if (!t.caster) {
            t.sw = 0.f;
            continue;
          }
          // (never more than 0.19 a frame: a long frame is no pop)
          const float rate = std::min(dt / (t.sw_fade > 0.f ? t.sw_fade : sfade), 0.19f);
          if (t.leaving) {
            t.sw -= rate;
            if (t.sw <= 0.f || t.w <= 0.02f) {
              t.caster = t.leaving = false;
              t.sw = 0.f;
            }
          } else {
            t.sw = cut ? 1.f : std::min(1.f, t.sw + rate);
          }
        }
        // (the metric: a near lamp in view 1.5 s and still no caster)
        if (!cut) {
          for (const auto& t : tracks) {
            if (t.lamp && !t.flash && !t.ever_moved && t.shown && t.in_view && t.cand >= 0 && !t.caster &&
                in_pool(t) && t.view_since >= 0.0 && m_time - t.view_since > 1.5) {
              g_light_stab.nearmiss++;
              break;
            }
          }
        }
      }
      // (the CPU time of the tracks and the casters, for the stability log's cpu_us)
      g_light_stab.cpu_ns += (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - track_t0)
                                 .count();
      g_light_stab.cpu_n++;
      // what's uploaded: the casters first (the first 8 the composite looks at for shadows), the
      // flashes (shadowed too, when there's room), then the others, strongest first
      std::vector<int> up;
      for (size_t j = 0; j < tracks.size(); j++) {
        if (tracks[j].shown && tracks[j].caster) {
          up.push_back((int)j);
        }
      }
      std::sort(up.begin(), up.end(), [&](int a, int b) { return tracks[a].id < tracks[b].id; });
      std::vector<int> rest;
      for (size_t j = 0; j < tracks.size(); j++) {
        if (tracks[j].shown && !tracks[j].caster) {
          rest.push_back((int)j);
        }
      }
      std::sort(rest.begin(), rest.end(), [&](int a, int b) {
        if (tracks[a].flash != tracks[b].flash) {
          return tracks[a].flash;
        }
        return tracks[a].score > tracks[b].score;
      });
      up.insert(up.end(), rest.begin(), rest.end());
      if (up.size() > (size_t)kLights) {
        up.resize(kLights);
      }
      std::vector<Candidate> out;
      out.reserve(up.size());
      for (int j : up) {
        const auto& t = tracks[j];
        Candidate c;
        if (t.cand >= 0) {
          c = candidates[t.cand];
        } else {
          c.instant = t.instant;
          c.score = 0.f;
        }
        to_view(t.world, c.pos);
        c.pos[3] = t.radius;
        for (int k = 0; k < 3; k++) {
          c.color[k] = t.color[k] * t.w;
        }
        c.color[3] = t.color[3];
        out.push_back(c);
        up_track.push_back(j);
      }
      candidates = std::move(out);
    }
    const int n = std::min((int)candidates.size(), kLights);
    float pos[kLights * 4] = {};
    float color[kLights * 4] = {};
    bool light_instant[kLights] = {};
    for (int i = 0; i < n; i++) {
      std::memcpy(pos + i * 4, candidates[i].pos, sizeof(float) * 4);
      std::memcpy(color + i * 4, candidates[i].color, sizeof(float) * 4);
      light_instant[i] = candidates[i].instant;
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
    ENH_CPU_SPLIT("sort+upload");
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
        // (the glow of a placed fire: that one, sized by its glow)
        bool same = false;
        for (auto& o : fires) {
          float d2 = 0.f;
          for (int k = 0; k < 3; k++) {
            d2 += (o.pos[k] - v.pos[k]) * (o.pos[k] - v.pos[k]);
          }
          if (d2 < 2.f * kMeter * 2.f * kMeter) {
            o = v;
            same = true;
            break;
          }
        }
        if (!same) {
          fires.push_back(v);
        }
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
    glUniform1f(glGetUniformLocation(id, "materials"), m_settings.materials);
    glUniform1f(glGetUniformLocation(id, "reflect_amount"), g_world.reflect[game_index()]);
    glUniform1f(glGetUniformLocation(id, "reflect_rough"), g_world.rough[game_index()]);
    glUniform1f(glGetUniformLocation(id, "relief"), m_settings.relief);
    ENH_CPU_SPLIT("fires");
    {
      // Jak's recent path: a point every 0.1 s while he moves (every 0.5 s standing), kept 2 s;
      // the shader starts a ring on the water from each one that lies at the water's height
      // (riding over the water: a point every 0.05 s, foam churned along them)
      constexpr int kWake = 32;
      const auto& pl = g_enhanced_player;
      const bool riding_water = pl.valid && pl.state == 5;
      if (pl.valid && m_settings.wake > 0.f) {
        double since = m_wake.empty() ? 1e9 : m_time - m_wake.back().time;
        float moved = 1e9f;
        if (!m_wake.empty()) {
          const float* q = m_wake.back().pos;
          float dx = pl.pos[0] - q[0], dz = pl.pos[2] - q[2];
          moved = std::sqrt(dx * dx + dz * dz);
        }
        if ((since >= (riding_water ? 0.05 : 0.1) && moved > 0.1f * kMeter) || since >= 0.5) {
          m_wake.push_back({{pl.pos[0], pl.pos[1], pl.pos[2]}, m_time, riding_water});
        }
        // a jump into the water: falling faster than 5 m/s, then stopped (the shader rings only the
        // points at the water's height, so a landing on dry ground shows nothing).
        // (tuning.json "water": {"splash_ring": 0}: off)
        static const EnhancedTune t_splash_ring("water.splash_ring", 1.f);
        const double dt = m_time - m_wake_prev_t;
        if (m_wake_prev_t >= 0.0 && dt > 1e-4 && dt < 0.25) {
          const float vy = (pl.pos[1] - m_wake_prev_y) / (float)dt;  // game units / s
          if (vy < -5.f * kMeter) {
            m_wake_falling = true;
          } else if (m_wake_falling && vy > -1.5f * kMeter) {
            m_wake_falling = false;
            if (t_splash_ring > 0.5f && !riding_water) {
              WakePoint sp{{pl.pos[0], pl.pos[1], pl.pos[2]}, m_time, false};
              sp.splash = true;
              m_wake.push_back(sp);
            }
          }
        }
        m_wake_prev_y = pl.pos[1];
        m_wake_prev_t = m_time;
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
        // age, + 10 behind a vehicle or the board, + 20 where he jumped in
        wake[wake_count * 4 + 3] = (float)(m_time - w.time) + (w.splash ? 20.f : w.ride ? 10.f : 0.f);
        wake_count++;
      }
      glUniform1i(glGetUniformLocation(id, "wake_count"), wake_count);
      glUniform4fv(glGetUniformLocation(id, "wake"), kWake, wake);
      glUniform1f(glGetUniformLocation(id, "wake_strength"), m_settings.wake);
    }
    ENH_CPU_SPLIT("b1417");
    {
      // footprints where Jak walks on soft ground (the game knows the ground's material), ruts where
      // he drives; kept 90 s
      constexpr int kPrints = 96;
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
      // a landing from a jump: a big splash where he comes down
      const bool on_foot = pl.state == 1 || pl.state == 3;
      if (pl.valid && on_foot && m_last_player_state == 0) {
        m_steps.push_back({{pl.pos[0], pl.pos[1], pl.pos[2]}, 2.f, m_time});
        for (int k = 0; k < 3; k++) {
          const float a = k * 2.094f + (float)(std::rand() % 100) * 0.01f;
          const float at[3] = {pl.pos[0] + std::cos(a) * 0.4f * kMeter, pl.pos[1],
                               pl.pos[2] + std::sin(a) * 0.4f * kMeter};
          ground_puff(at, 0.7f, 1.6f, false);
        }
      }
      m_last_player_state = pl.valid ? pl.state : 0;
      // (a hover vehicle, flag 8: no ruts; tuning "water": {"hover_puffs": 1}: as before)
      static const EnhancedTune t_hover_ruts("water.hover_puffs", 0.f);
      if (pl.valid && pl.state > 0 && pl.state <= 3 &&
          !(pl.state == 2 && (pl.flags & 8) && t_hover_ruts < 0.5f)) {
        bool rut = pl.state == 2;
        const bool soft = pl.state == 1 || pl.state == 2;
        // (ruts: a piece every meter, each one reaching back to the one before: a solid track)
        float stride = (rut ? 1.f : 0.7f) * kMeter;
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
            p.seg = moved;
            // (in lying snow any ground takes them)
            if ((soft || m_snow_cover > 0.15f) && m_settings.prints > 0.f) {
              m_prints.push_back(p);
            }
            if (!rut) {
              m_steps.push_back({{p.pos[0], p.pos[1], p.pos[2]}, 1.f, m_time});
              // running kicks up more than walking
              if (moved > 0.9f * kMeter || (std::rand() % 2) == 0) {
                ground_puff(p.pos, 0.3f, 1.0f, true);
              }
            }
            m_print_left = !m_print_left;
          }
          std::memcpy(m_last_print, pl.pos, sizeof(m_last_print));
          m_last_print_valid = true;
        }
      }
      while (!m_prints.empty() &&
             (m_time - m_prints.front().time > 90.0 || m_prints.size() > (size_t)kPrints)) {
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
        // (ruts: 2 + the piece's length in meters)
        print_info[print_count * 2 + 1] = (float)p.kind + (p.kind == 2 ? p.seg / kMeter : 0.f);
        print_count++;
      }
      glUniform1i(glGetUniformLocation(id, "print_count"), print_count);
      glUniform4fv(glGetUniformLocation(id, "prints"), kPrints, prints);
      glUniform2fv(glGetUniformLocation(id, "print_info"), kPrints, print_info);
      glUniform1f(glGetUniformLocation(id, "prints_strength"), m_settings.prints);

      // splashes: the steps of the last second (the shader checks for a puddle under them)
      constexpr int kSplashes = 16;
      static bool test_splash = std::getenv("JAK_ENHANCED_SPLASH_TEST") != nullptr;
      static double test_splash_t = 0;
      if (test_splash && pl.valid && m_time - test_splash_t > 0.15) {
        // test switch: steps landing one after another on a line ahead of Jak, away from the camera
        test_splash_t = m_time;
        static int n = 0;
        float fx = pl.pos[0] - cam.trans[0], fz = pl.pos[2] - cam.trans[2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1.f);
        const float along = (-1.5f + (n % 6) * 0.6f) * kMeter;
        const float side = ((n % 3) - 1) * 1.2f * kMeter;
        m_steps.push_back({{pl.pos[0] + fx / len * along + fz / len * side, pl.pos[1],
                            pl.pos[2] + fz / len * along - fx / len * side},
                           (n % 4) == 0 ? 2.f : 1.f, m_time});
        n++;
      }
      while (!m_steps.empty() &&
             (m_time - m_steps.front().time > 1.2 || m_steps.size() > (size_t)kSplashes / 2)) {
        m_steps.erase(m_steps.begin());
      }
      // in the rain, drops bounce off the ground around Jak (2-6 m, on his level ground): a few
      // droplets each, hopping a few centimeters
      if (m_rain > 0.1f && pl.valid && (pl.state == 1 || pl.state == 3)) {
        static double bounce_t = 0;
        const double every = 0.11 / m_rain;
        if (m_time - bounce_t > every) {
          bounce_t = m_time;
          const float a = (float)(std::rand() % 10000) / 10000.f * 6.2832f;
          const float r = (2.f + (float)(std::rand() % 1000) / 250.f) * kMeter;
          m_bounces.push_back({{pl.pos[0] + std::cos(a) * r, pl.pos[1], pl.pos[2] + std::sin(a) * r},
                               0.f, m_time});
        }
      }
      while (!m_bounces.empty() &&
             (m_time - m_bounces.front().time > 0.45 || m_bounces.size() > (size_t)kSplashes / 2)) {
        m_bounces.erase(m_bounces.begin());
      }
      float splash[kSplashes * 4] = {};
      int splash_count = 0;
      for (const auto* list : {&m_steps, &m_bounces})
      for (const auto& st : *list) {
        std::memcpy(splash + splash_count * 4, st.pos, sizeof(float) * 3);
        // age, with the strength in the integer part above 10 s: 1 step, 2 landing
        splash[splash_count * 4 + 3] = (float)(m_time - st.time) + 10.f * st.strength;
        splash_count++;
      }
      glUniform1i(glGetUniformLocation(id, "splash_count"),
                  std::max(m_wet, m_puddle) > 0.05f ? splash_count : 0);
      glUniform4fv(glGetUniformLocation(id, "splash"), kSplashes, splash);

      // dust behind a vehicle or the board crossing the desert (two plumes, off the back wheels),
      // spray where they cross water, and a burst where they hit it
      constexpr int kPuffs = 32;
      const bool riding = pl.valid && (pl.state == 2 || pl.state >= 4);
      if (riding && m_ride_last_time >= 0.0 && m_time > m_ride_last_time) {
        float dx = pl.pos[0] - m_ride_last[0], dz = pl.pos[2] - m_ride_last[2];
        float dist = std::sqrt(dx * dx + dz * dz) / kMeter;
        float v = dist / (float)(m_time - m_ride_last_time);
        if (v < 200.f) {  // not a teleport (the boosted board does ~100 m/s)
          m_ride_speed += (v - m_ride_speed) * 0.2f;
          if (dist > 0.02f) {
            m_ride_dir[0] = dx / (dist * kMeter);
            m_ride_dir[1] = dz / (dist * kMeter);
          }
        }
      } else if (!riding) {
        m_ride_speed = 0.f;
      }
      std::memcpy(m_ride_last, pl.pos, sizeof(m_ride_last));
      m_ride_last_time = m_time;
      auto add_puff = [&](float back, float side, float down, float size, int kind, float life) {
        Puff f;
        const float fx = m_ride_dir[0], fz = m_ride_dir[1];
        f.pos[0] = pl.pos[0] - fx * back * kMeter + fz * side * kMeter;
        f.pos[1] = pl.pos[1] - down * kMeter;
        f.pos[2] = pl.pos[2] - fz * back * kMeter - fx * side * kMeter;
        f.size = size;
        f.kind = kind;
        f.seed = (float)(std::rand() % 1000) / 1000.f;
        f.life = life;
        f.time = m_time;
        m_puffs.push_back(f);
      };
      // test switch: puffs behind Jak at any speed, anywhere (dust; spray with the value "water")
      static const char* puff_test = std::getenv("JAK_ENHANCED_PUFF_TEST");
      static bool test_puffs = puff_test != nullptr;
      static bool test_spray = puff_test && std::strcmp(puff_test, "water") == 0;
      if (pl.state == 2 || pl.state == 4 || pl.state == 6 || pl.state == 7) {
        m_last_ride_state = pl.state == 6 ? 4 : pl.state == 7 ? 2 : pl.state;
      }
      const bool board = pl.state == 4 || pl.state == 6 || (pl.state == 5 && m_last_ride_state == 4);
      const bool on_water = pl.state == 5 || test_spray;
      // a hover vehicle (zoomers, hover cars, the gunship; flag 8 from logic-target.gc) has no
      // wheels: no spray off wet ground, no dust, no plumes over water. Its two plumes were big
      // white-blue blobs behind a zoomer in Haven's rain. (tuning.json "water": {"hover_puffs": 1}:
      // the old plumes)
      static const EnhancedTune t_hover_puffs("water.hover_puffs", 0.f);
      const bool hover = pl.valid && (pl.flags & 8) && t_hover_puffs < 0.5f;
      // (none while the board or the vehicle is in the air: a jump leaves no dust)
      if ((riding || test_puffs) && !hover && pl.valid && pl.state < 6 &&
          m_time - m_puff_time > 0.06) {
        const float sp = test_puffs ? 20.f : m_ride_speed;
        const float side = (m_puff_left ? 1.f : -1.f) * (board ? 0.2f : 1.1f);
        // (the test: as if moving, along a 12 m trail)
        // (Jak sits ~1.4 m over the ground in a vehicle: the dust starts on the ground behind the
        // back wheels, not up by the seat)
        const float back = (board ? 0.6f : 2.2f) + (test_puffs ? (float)std::fmod(m_time * 6.0, 12.0) : 0.f);
        if (on_water && sp > 3.f && m_settings.wake > 0.f) {
          add_puff(back, side, board ? 0.2f : 0.6f, std::min(0.5f + sp * 0.04f, 2.f), 1, 1.1f);
        } else if (!on_water && std::max(m_wet, m_puddle) > 0.3f && sp > 6.f &&
                   m_settings.wake > 0.f) {
          // wet ground (rain, puddles): the wheels throw spray, not dust
          add_puff(back, side, board ? 0.1f : 1.3f, std::min(0.4f + sp * 0.03f, 1.6f), 1, 0.8f);
        } else if (!on_water && (m_pond || test_puffs) && sp > 5.f && m_settings.prints > 0.f) {
          // (the board under its L2 boost: a bigger plume, hanging longer, starting farther back: a
          // cloud swelling around Jak himself veiled him, he looked see-through)
          const float b = board ? g_enhanced_board_boost.load() : 0.f;
          add_puff(back + 2.5f * b, side, board ? 0.1f : 1.4f,
                   std::min(0.6f + sp * 0.05f, 2.4f) * (1.f + 0.3f * b), 0, 2.2f * (1.f + 0.5f * b));
        }
        m_puff_left = !m_puff_left;
        m_puff_time = m_time;
      }
      // hitting the water at speed: a burst all around
      static int last_state = 0;
      if (pl.valid && pl.state == 5 && (last_state == 2 || last_state == 4) && m_ride_speed > 5.f &&
          m_settings.wake > 0.f && !hover) {
        for (int k = 0; k < 4; k++) {
          add_puff((k - 1.5f) * 0.8f, (k % 2 ? 1.f : -1.f) * 1.2f, 0.4f, 2.6f, 1, 1.4f);
        }
      }
      last_state = pl.valid ? pl.state : 0;
      while (!m_puffs.empty() && (m_time - m_puffs.front().time > m_puffs.front().life ||
                                  m_puffs.size() > (size_t)kPuffs)) {
        m_puffs.erase(m_puffs.begin());
      }
      float puff[kPuffs * 4] = {};
      float puff_info[kPuffs * 4] = {};
      int puff_count = 0;
      for (const auto& f : m_puffs) {
        if (m_time - f.time > f.life) {
          continue;
        }
        std::memcpy(puff + puff_count * 4, f.pos, sizeof(float) * 3);
        puff[puff_count * 4 + 3] = (float)(m_time - f.time);
        puff_info[puff_count * 4] = f.size;
        puff_info[puff_count * 4 + 1] = (float)f.kind;
        puff_info[puff_count * 4 + 2] = f.seed;
        puff_info[puff_count * 4 + 3] = f.life;
        puff_count++;
      }
      glUniform1i(glGetUniformLocation(id, "puff_count"), cam.valid ? puff_count : 0);
      glUniform4fv(glGetUniformLocation(id, "puff"), kPuffs, puff);
      glUniform4fv(glGetUniformLocation(id, "puff_info"), kPuffs, puff_info);
    }
    // the nearest explosions in view for the shockwave: view space + radius, age
    ENH_CPU_SPLIT("b1454");
    {
      constexpr int kBlasts = 4;
      float blast_pos[kBlasts * 4] = {};
      // (per blast x age, y seed, z ground: one vec4 in the composite, blast_info; as three float[4]
      // they took 12 of NVIDIA's 1024 constant registers, and the merged composite failed to link)
      float blast_info[kBlasts * 4] = {};
      float blast_dir[kBlasts * 4] = {};
      int blast_count = 0;
      // (fx_detail: the newest first, the older ones linger now)
      const bool newest_first = m_settings.fx_detail > 0.f;
      for (size_t k = 0; k < m_blasts.size(); k++) {
        const auto& b = m_blasts[newest_first ? m_blasts.size() - 1 - k : k];
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
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            blast_dir[blast_count * 4 + col] += b.dir[row] * cam.rot[row][col];
          }
        }
        blast_dir[blast_count * 4 + 3] = (float)b.kind;
        // (> 0: no ground known)
        blast_info[blast_count * 4 + 2] =
            b.ground < 1e11f ? std::min(b.ground - b.pos[1], 0.f) : 1.f;
        blast_info[blast_count * 4 + 1] = (float)std::fmod(b.start * 7.31, 100.0);
        blast_info[blast_count * 4] = (float)(m_time - b.start);
        blast_count++;
      }
      glUniform1i(glGetUniformLocation(id, "blast_count"), blast_count);
      glUniform4fv(glGetUniformLocation(id, "blast_pos"), kBlasts, blast_pos);
      glUniform4fv(glGetUniformLocation(id, "blast_info"), kBlasts, blast_info);
      glUniform4fv(glGetUniformLocation(id, "blast_dir"), kBlasts, blast_dir);
      glUniform1f(glGetUniformLocation(id, "blast_strength"), m_settings.blast);
      enhanced_upload_decals(id, cam.trans, (float)m_time, on && m_settings.fx_detail > 0.f);
    }
    ENH_CPU_SPLIT("b1709");
    // fireflies at night, seeds and pollen drifting by day: out in the wild (not the desert but its
    // oasis, not a city), dry and calm, a few dozen meters around Jak. Fireflies keep to swarms
    // fixed in the world (a jittered 16 m lattice: some cells hold one, of their own size), drift
    // lazily around them, and each fades in and out at the end of its life: no pops
    {
      constexpr int kMotes = 24;
      constexpr float kCell = 16.f;  // m
      const auto& pl = g_enhanced_player;
      static const bool force = [] {
        const char* m = std::getenv("JAK_ENHANCED_MOTES");
        return m && std::strcmp(m, "force") == 0;
      }();
      // (in Jak 3's desert the ocean renderer draws only the oases' ponds: water drawn lately = by
      // an oasis, its grass and palms)
      if (m_pond && m_ocean_ready) {
        m_motes_pond = m_time;
      }
      const bool oasis = m_pond && m_time - m_motes_pond < 8.0;
      const bool wild = force || ((oasis || (!m_city && !m_pond)) && m_snow < 0.1f &&
                                  m_rain < 0.1f && m_storm < 0.1f);
      const float dt = m_motes_time < 0 ? 0.f : (float)std::min(0.1, m_time - m_motes_time);
      m_motes_time = m_time;
      // (not tied to the dynamic lights setting: they're glows of their own)
      const float target =
          wild && pl.valid && have_scene && cam.valid && m_settings.motes > 0.f ? 1.f : 0.f;
      m_motes_on += (target - m_motes_on) * std::min(1.f, dt * (force ? 5.f : 0.5f));
      const bool night = m_night > 0.5f;
      if (night != m_motes_night) {
        m_motes.clear();  // (both kinds are faded out at the switch)
        m_motes_night = night;
      }
      static std::mt19937 rng{std::random_device{}()};
      std::uniform_real_distribution<float> u(0.f, 1.f);
      auto hash = [](int x, int z, uint32_t salt) {
        uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u ^ salt * 83492791u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return (float)(h & 0xffffu) / 65535.f;
      };
      // the swarms near Jak (m): x, z, weight
      float swarm[26][3];  // (5x5 cells, plus the test one)
      int swarms = 0;
      float swarm_w = 0.f;
      if (night) {
        const int cx = (int)std::floor(pl.pos[0] / kMeter / kCell);
        const int cz = (int)std::floor(pl.pos[2] / kMeter / kCell);
        for (int i = -2; i <= 2; i++) {
          for (int j = -2; j <= 2; j++) {
            const int x = cx + i, z = cz + j;
            if (hash(x, z, 1) > 0.45f) {
              continue;  // an empty cell
            }
            const float sx = (x + hash(x, z, 2)) * kCell, sz = (z + hash(x, z, 3)) * kCell;
            const float dx = sx - pl.pos[0] / kMeter, dz = sz - pl.pos[2] / kMeter;
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > 22.f || d < 8.f) {
              continue;  // (none right around Jak: they're seen out there)
            }
            const float w = (0.25f + hash(x, z, 4)) * (1.f - d / 30.f);
            swarm[swarms][0] = sx;
            swarm[swarms][1] = sz;
            swarm[swarms][2] = w;
            swarm_w += w;
            swarms++;
          }
        }
        if (force) {
          // (screenshots: one swarm 13 m ahead of the camera, past Jak, too)
          float fx = cam.rot[0][2], fz = cam.rot[2][2];
          const float fl = std::max(1e-3f, std::sqrt(fx * fx + fz * fz));
          swarm[swarms][0] = pl.pos[0] / kMeter + fx / fl * 13.f;
          swarm[swarms][1] = pl.pos[2] / kMeter + fz / fl * 13.f;
          swarm[swarms][2] = 1.f;
          swarm_w += 1.f;
          swarms++;
        }
      }
      auto spawn = [&](Mote& m) {
        m.vel[0] = m.vel[1] = m.vel[2] = 0.f;
        m.seed = std::floor(u(rng) * 999.f);
        m.age = 0.f;
        m.fade = 0.f;
        m.life = 0.f;
        float hx, hz;
        if (night) {
          if (swarms == 0) {
            return;  // no swarm around: it waits
          }
          float pick = u(rng) * swarm_w;
          int s = 0;
          while (s < swarms - 1 && pick > swarm[s][2]) {
            pick -= swarm[s][2];
            s++;
          }
          // a bigger swarm also spreads wider
          const float a = u(rng) * 6.2832f, r = (1.f + 4.f * swarm[s][2]) * std::sqrt(u(rng));
          hx = swarm[s][0] + std::cos(a) * r;
          hz = swarm[s][1] + std::sin(a) * r;
          // kept 7 m or more from Jak
          const float jx = hx - pl.pos[0] / kMeter, jz = hz - pl.pos[2] / kMeter;
          const float jd = std::sqrt(jx * jx + jz * jz);
          if (jd < 7.f) {
            const float k = 7.f / std::max(jd, 1e-3f);
            hx = pl.pos[0] / kMeter + jx * k;
            hz = pl.pos[2] / kMeter + jz * k;
          }
          m.life = 14.f + 26.f * u(rng);
        } else {
          const float a = u(rng) * 6.2832f, r = 3.f + 19.f * std::sqrt(u(rng));
          hx = pl.pos[0] / kMeter + std::cos(a) * r;
          hz = pl.pos[2] / kMeter + std::sin(a) * r;
          m.life = 10.f + 20.f * u(rng);
        }
        m.home[0] = hx * kMeter;
        m.home[1] = pl.pos[1] + (night ? 0.3f + 1.6f * u(rng) : 0.3f + 2.2f * u(rng)) * kMeter;
        m.home[2] = hz * kMeter;
        for (int k = 0; k < 3; k++) {
          m.pos[k] = m.home[k];
        }
      };
      if (m_motes_on <= 0.01f || !pl.valid) {
        m_motes.clear();
      } else {
        // fireflies: few and sparse (half the slots)
        const int want_n = night ? kMotes / 2 : kMotes;
        if ((int)m_motes.size() > want_n) {
          m_motes.resize(want_n);
        }
        while ((int)m_motes.size() < want_n) {
          m_motes.emplace_back();
          spawn(m_motes.back());
          // (the first ones start at different points of their lives)
          m_motes.back().age = m_motes.back().life * 0.5f * u(rng);
        }
        const float t = (float)std::fmod(m_time, 10000.0);
        for (auto& m : m_motes) {
          m.age += dt;
          const float dx = m.pos[0] - pl.pos[0], dz = m.pos[2] - pl.pos[2];
          const float d = std::sqrt(dx * dx + dz * dz) / kMeter;
          // left behind: it dies off quickly (fades), then comes back somewhere around Jak
          if (d > 26.f) {
            m.age = std::max(m.age, m.life - 1.5f);
          }
          if (m.age >= m.life) {
            spawn(m);
            continue;
          }
          m.fade = std::min({1.f, m.age / 2.f, (m.life - m.age) / 1.5f}) *
                   (1.f - std::clamp((d - 18.f) / 8.f, 0.f, 1.f));
          // a lazy flight: a smooth wander made of two slow swings per axis, the fly's own
          const float s = m.seed;
          float want[3];
          for (int k = 0; k < 3; k++) {
            const float f1 = 0.13f + 0.11f * std::fmod(s * (0.37f + k * 0.11f), 1.f);
            const float f2 = 0.31f + 0.23f * std::fmod(s * (0.71f + k * 0.17f), 1.f);
            want[k] = std::sin(t * f1 + s * (1.3f + k)) + 0.5f * std::sin(t * f2 + s * (2.9f + k));
          }
          const float amp_h = night ? 0.35f : 0.25f, amp_v = night ? 0.12f : 0.08f;
          want[0] *= amp_h;
          want[1] *= amp_v;
          want[2] *= amp_h;
          if (night) {
            // kept to its swarm by a soft pull, stronger the further it strays
            for (int k = 0; k < 3; k++) {
              const float off = (m.home[k] - m.pos[k]) / kMeter;
              want[k] += off * (k == 1 ? 0.5f : 0.12f);
            }
          } else {
            // seeds drift with the breeze and sink slowly
            want[0] += 0.3f;
            want[2] += 0.15f;
            want[1] -= 0.05f;
          }
          // between a few decimeters and three meters over Jak's ground
          const float above = (m.pos[1] - pl.pos[1]) / kMeter;
          if (above < 0.25f) {
            want[1] += 0.4f;
          }
          if (above > 3.f) {
            want[1] -= 0.4f;
          }
          const float ease = std::min(1.f, dt * 1.2f);
          for (int k = 0; k < 3; k++) {
            m.vel[k] += (want[k] * kMeter - m.vel[k]) * ease;
            m.pos[k] += m.vel[k] * dt;
          }
        }
      }
      float mote[kMotes * 4] = {};
      int mote_count = 0;
      for (const auto& m : m_motes) {
        if (m.life <= 0.f || m.fade <= 0.002f) {
          continue;
        }
        float v[3] = {0.f, 0.f, 0.f};
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            v[col] += (m.pos[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        if (v[2] < 0.3f * kMeter) {
          continue;
        }
        std::memcpy(mote + mote_count * 4, v, sizeof(v));
        // w: the seed (whole part) and how faded in it is (fraction)
        mote[mote_count * 4 + 3] = m.seed + std::min(m.fade, 0.999f);
        mote_count++;
      }
      glUniform1i(glGetUniformLocation(id, "mote_count"), cam.valid ? mote_count : 0);
      glUniform4fv(glGetUniformLocation(id, "mote"), kMotes, mote);
      // kind (0 fireflies, 1 seeds and pollen), strength, time (s). Fireflies wake at dusk and the
      // seeds settle before it: both are out at the switch
      auto ss = [](float a, float b, float x) {
        const float k = std::clamp((x - a) / (b - a), 0.f, 1.f);
        return k * k * (3.f - 2.f * k);
      };
      glUniform3f(glGetUniformLocation(id, "mote_style"), night ? 0.f : 1.f,
                  m_motes_on * m_settings.motes *
                      (night ? ss(0.5f, 0.85f, m_night) : 1.f - ss(0.15f, 0.5f, m_night)),
                  (float)std::fmod(m_time, 1000.0));
    }
    glUniform1i(glGetUniformLocation(id, "fire_count"), fire_count);
    glUniform4fv(glGetUniformLocation(id, "fire_pos"), kFires, fire_pos);
    static_assert(kFires == 4, "fire_seed4 is one vec4");
    glUniform4fv(glGetUniformLocation(id, "fire_seed4"), 1, fire_seed);
    glUniform3f(glGetUniformLocation(id, "fire"), m_settings.fire_haze, m_settings.fire_embers,
                (float)std::fmod(m_time, 1000.0));
    ENH_CPU_SPLIT("fireflies");
    // dark nights: how dark (the desert darkest, a city least), and the lights reach further and
    // shine stronger into it
    const float dark = night_darkness();
    glUniform2f(glGetUniformLocation(id, "night_dark"), dark, 1.f + 1.6f * dark);
    glUniform1f(glGetUniformLocation(id, "light_spill"), m_settings.light_spill);
    {
      // Spargus lights the sky over it at night, a warm glow on the horizon seen from the desert
      // (out of town, fading with the distance)
      const float sx = 2050.f * kMeter, sz = -230.f * kMeter;
      const float dx = cam.trans[0] - sx, dz = cam.trans[2] - sz;
      const float dist_m = std::sqrt(dx * dx + dz * dz) / kMeter;
      const float k = (g_game_version == GameVersion::Jak3 && m_pond)
                          ? dark * std::clamp((dist_m - 350.f) / 400.f, 0.f, 1.f) *
                                std::exp(-dist_m / 4000.f) * m_settings.light_strength
                          : 0.f;
      glUniform4f(glGetUniformLocation(id, "town_glow"), sx, sz, k, 0.f);
    }
    glUniform1f(glGetUniformLocation(id, "cloud_shadows"), m_settings.cloud_shadows ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(id, "lens_flare"),
                g_enhanced_scene.movie ? 0.f : m_settings.lens_flare);
    {
      // Jak in view space: the dark spares a little around him (he stays readable)
      const auto& pl = g_enhanced_player;
      float v[3] = {0.f, 0.f, 0.f};
      for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
          v[col] += (pl.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      glUniform4f(glGetUniformLocation(id, "player_view"), v[0], v[1] + 1.f * kMeter, v[2],
                  pl.valid && cam.valid ? 1.f : 0.f);
    }
    for (int i = 0; i < n; i++) {
      pos[i * 4 + 3] *= 1.f + 0.6f * dark;
    }
    {
      // headlights: the nearest few, in view space, shown in the dark only
      {
        std::lock_guard<std::mutex> lock(g_enhanced_headlights.mutex);
        if (g_enhanced_headlights.count > 0) {
          m_headlights.clear();
          for (int i = 0; i < g_enhanced_headlights.count; i++) {
            std::array<float, 8> h;
            std::memcpy(h.data(), g_enhanced_headlights.pending[i], sizeof(float) * 8);
            m_headlights.push_back(h);
          }
          g_enhanced_headlights.count = 0;
          m_headlights_time = m_time;
        } else if (m_time - m_headlights_time > 0.2) {
          m_headlights.clear();
        }
      }
      // test switch: two headlights on Jak, pointing where the camera looks
      static bool test_headlights = std::getenv("JAK_ENHANCED_HEADLIGHT_TEST") != nullptr;
      if (test_headlights && g_enhanced_player.valid) {
        const auto& pl = g_enhanced_player;
        float fx = cam.rot[0][2], fz = cam.rot[2][2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1e-3f);
        fx /= len;
        fz /= len;
        m_headlights.clear();
        for (int k = 0; k < 2; k++) {
          float side = (k ? 0.7f : -0.7f) * kMeter;
          m_headlights.push_back({pl.pos[0] + fz * side + fx * kMeter, pl.pos[1] + 0.9f * kMeter,
                                  pl.pos[2] - fx * side + fz * kMeter, 1.f, fx * 0.99f, -0.07f,
                                  fz * 0.99f, 0.f});
        }
      }
      constexpr int kSpots = 8;
      const auto& spots_src = m_headlights;
      std::vector<std::pair<float, int>> order;
      for (int i = 0; i < (int)spots_src.size(); i++) {
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) {
          float dk = spots_src[i][k] - cam.trans[k];
          d2 += dk * dk;
        }
        order.push_back({d2, i});
      }
      std::sort(order.begin(), order.end());
      float spot_pos[kSpots * 4] = {};
      float spot_dir[kSpots * 4] = {};
      int spots = 0;
      const float shown = std::max(m_night, dark);
      for (const auto& [d2, i] : order) {
        if (spots == kSpots || shown <= 0.01f || !m_settings.lights) {
          break;
        }
        const auto& h = spots_src[i];
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            spot_pos[spots * 4 + col] += (h[row] - cam.trans[row]) * cam.rot[row][col];
            spot_dir[spots * 4 + col] += h[4 + row] * cam.rot[row][col];
          }
        }
        spot_pos[spots * 4 + 3] = h[3] * shown * m_grade.headlights;
        spots++;
      }
      glUniform1i(glGetUniformLocation(id, "spot_count"), spots);
      glUniform4fv(glGetUniformLocation(id, "spot_pos"), kSpots, spot_pos);
      glUniform4fv(glGetUniformLocation(id, "spot_dir"), kSpots, spot_dir);
    }
    glUniform1i(glGetUniformLocation(id, "light_count"), n);
    ENH_CPU_SPLIT("dark+beacons");
    // the screen tiles' masks, uploaded after the beacons: per 32x18 tile r = the lights that may
    // reach it, g/b = the beacons 0-31/32-63 whose point and halo may show in it
    constexpr int kTx = 32, kTy = 18;
    uint32_t tile_bits[kTx * kTy * 4] = {};
    int tile_flags = 0;  // 1 lights, 2 beacons (light_tiles_on)
    {
      // light tiles (clustered lights, step 1): the screen in 32x18 tiles, per tile a mask of the
      // lights whose sphere's projection may touch it, so the composite's main light loop runs only
      // those. Conservative: the hull of the projected corners of each sphere's box; a sphere
      // reaching the camera plane covers every tile. lights.tiles 0: off (every pixel, every light).
      // Default on: the main loop, the metal highlights and the air glow/halo loops use it; at
      // 2560x1600 Spargus 21h the composite 11.6 -> 11.0 ms, frames equal to the TAA noise
      // The composite declares the uniforms only with ENH_LIGHT_TILES defined (uniform budget): without
      // it these calls find location -1 and do nothing
      static const EnhancedTune t_tiles("lights.tiles", 1.f);
      const bool tiles_on = t_tiles > 0.5f;
      if (tiles_on) {
        tile_flags |= 1;
        const float fx = m_last_proj[2], fy = m_last_proj[3];
        for (int i = 0; i < n; i++) {
          const float* lp = pos + i * 4;
          // (grown to what every loop over the lights reaches: highlights and the air's glow 1.6
          // radii, the halo ~2.4 x its spread of at least 0.01 rad)
          // (a fire under a roof, alpha 0.25, lights the floor out to tune_fire_floor radii: its
          // tiles grow with it, or the pool ends in tile-shaped steps)
          static const EnhancedTune t_fire_floor("shaders.tune_fire_floor", 3.f);
          const bool roof_fire = std::abs(color[i * 4 + 3] - 0.25f) < 0.1f;
          const float reach =
              roof_fire ? std::max(1.6f, 1.07f * std::min((float)t_fire_floor, 3.f)) : 1.6f;
          const float r = std::max(reach * lp[3],
                                   0.03f * std::sqrt(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]));
          int x0 = 0, x1 = kTx - 1, y0 = 0, y1 = kTy - 1;
          if (lp[2] - r > 0.1f * kMeter) {
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            for (int k = 0; k < 8; k++) {
              const float x = lp[0] + ((k & 1) ? r : -r);
              const float y = lp[1] + ((k & 2) ? r : -r);
              const float z = lp[2] + ((k & 4) ? r : -r);
              const float u = 0.5f * fx * x / z + 0.5f, v = 0.5f * fy * y / z + 0.5f;
              u0 = std::min(u0, u);
              u1 = std::max(u1, u);
              v0 = std::min(v0, v);
              v1 = std::max(v1, v);
            }
            // (half a tile of margin: the TAA's jitter, rounding)
            x0 = std::max((int)std::floor(u0 * kTx - 0.5f), 0);
            x1 = std::min((int)std::floor(u1 * kTx + 0.5f), kTx - 1);
            y0 = std::max((int)std::floor(v0 * kTy - 0.5f), 0);
            y1 = std::min((int)std::floor(v1 * kTy + 0.5f), kTy - 1);
          }
          for (int ty = y0; ty <= y1; ty++) {
            for (int tx = x0; tx <= x1; tx++) {
              tile_bits[(ty * kTx + tx) * 4] |= 1u << i;
            }
          }
        }
      }
    }
    glUniform4fv(glGetUniformLocation(id, "lantern"), 1, m_lantern);
    glUniform4fv(glGetUniformLocation(id, "light_pos"), kLights, pos);
    glUniform4fv(glGetUniformLocation(id, "light_color"), kLights, color);
    {
      // lamps' shadow maps (lights.shadow_maps): the shadow casters (the steady set of up to 8
      // lamps, see Steady lights) back in the world for next frame's pass, strongest first; this
      // frame's ready maps matched to them by place
      m_lamp_want.clear();
      {
        std::vector<int> cs;
        for (size_t j = 0; j < tracks.size(); j++) {
          if (tracks[j].caster) {
            cs.push_back((int)j);
          }
        }
        // (by the casters' importance, else the frame's score)
        auto imp = [&](int j) { return tracks[j].cimp >= 0.f ? tracks[j].cimp : tracks[j].score; };
        std::sort(cs.begin(), cs.end(), [&](int a, int b) { return imp(a) > imp(b); });
        // (maps only for the casters within lights.shadow_map_m, 10 m more for one that has its map:
        // a lamp 80 m away got one, its shadow too small to tell from the march, and as the camera
        // turned the far ones came and went, maps redrawn on every turn. 45 m, not 35: the casters
        // join out there (out of view or faint), so walking toward one its map is ready before it
        // shows, no march-to-map switch at 32-35 m (Haven 22h walk: mapsw2 3-6 -> ~1 per 20 s,
        // lamp pass +0.06 ms)
        static const EnhancedTune t_map_m("lights.shadow_map_m", 45.f);
        // (wanted last frame counts as having it: the slot is matched a frame after the want, and
        // a lamp walking out past 35 m in that frame lost its map at once, a map-march switch)
        std::vector<char> was_wanted(tracks.size());
        for (size_t j = 0; j < tracks.size(); j++) {
          was_wanted[j] = tracks[j].map_want || tracks[j].map_slot >= 0;
          tracks[j].map_want = false;
        }
        for (int j : cs) {
          const float far_m = ((float)t_map_m + (was_wanted[j] ? 10.f : 0.f)) * kMeter;
          if (tracks[j].dist > far_m) {
            continue;
          }
          tracks[j].map_want = true;
          LampWant lw;
          std::memcpy(lw.pos, tracks[j].world, sizeof(lw.pos));
          lw.radius = tracks[j].radius * (1.f + 0.6f * dark);
          m_lamp_want.push_back(lw);
        }
        // the lamps likely to cast next (lights.map_prewarm of them, the most important ones not
        // casting, still, within shadow_map_m): their static maps drawn ahead in the spare slots,
        // so one joining the casters has its map ready (lights.map_prewarm 0: off)
        static const EnhancedTune t_prewarm("lights.map_prewarm", 4.f);
        static const EnhancedTune t_maps_n("lights.shadow_maps", 12.f);
        const int n_pre = std::min((int)(float)t_prewarm,
                                   std::clamp((int)(float)t_maps_n, 0, kLampSlots) -
                                       std::min((int)m_lamp_want.size(), 8));
        if (n_pre > 0 && m_settings.light_shadow > 0.f) {
          std::vector<int> pre;
          for (size_t j = 0; j < tracks.size(); j++) {
            const auto& t = tracks[j];
            if (!t.caster && t.lamp && !t.flash && !t.moving && !t.instant && t.cand >= 0 &&
                t.dist < ((float)t_map_m + (was_wanted[j] ? 10.f : 0.f)) * kMeter) {
              pre.push_back((int)j);
            }
          }
          std::sort(pre.begin(), pre.end(), [&](int a, int b) { return imp(a) > imp(b); });
          for (int k = 0; k < (int)pre.size() && k < n_pre; k++) {
            tracks[pre[k]].map_want = true;
            LampWant lw;
            std::memcpy(lw.pos, tracks[pre[k]].world, sizeof(lw.pos));
            lw.radius = tracks[pre[k]].radius * (1.f + 0.6f * dark);
            lw.prewarm = true;
            m_lamp_want.push_back(lw);
          }
        }
      }
      // (a slot is kept at the place its lamp was first seen still: a flame's light jitters around it)
      static const EnhancedTune t_still_m("lights.shadow_still_m", 0.5f);
      const float match_u =
          std::max(std::clamp((float)t_still_m, 0.02f, 2.f), 0.3f) * kMeter;
      const float match2 = match_u * match_u;
      // per light (the first 8), its shadow for the composite in one float: 32 x its weight (0-15)
      // + the slot of its map (16: none) + 0.9 x the map's share of the shadow (eased in as the map
      // gets ready, lights.map_fade_s; the rest the screen-space march). Per light, its map's place
      // and far plane (the slots are more than 8)
      static const EnhancedTune t_map_fade("lights.map_fade_s", 0.4f);
      static double map_last_t = 0.0;
      const float map_rate =
          (float)std::clamp(m_time - map_last_t, 0.0, 0.25) / std::max((float)t_map_fade, 0.01f);
      map_last_t = m_time;
      float light_slot[8];
      float slot_pos[8 * 4] = {};
      float stab_sw[8];
      int stab_slot[8];
      for (int i = 0; i < 8; i++) {
        light_slot[i] = 16.f;
        stab_sw[i] = 0.f;
        stab_slot[i] = -1;
        if (i >= n || i >= (int)up_track.size()) {
          continue;
        }
        auto& t = tracks[up_track[i]];
        const float sw = t.caster ? t.sw : (t.flash ? 1.f : 0.f);
        int slot = -1;
        for (int s = 0; t.caster && m_lamp_ready && s < m_lamp_alloc; s++) {
          const auto& sl = m_lamp_slots[s];
          const float dx = sl.pos[0] - t.world[0], dy = sl.pos[1] - t.world[1],
                      dz = sl.pos[2] - t.world[2];
          // (two lights in one place share its map)
          if (sl.used && sl.faces >= 6 && sl.seen == m_lamp_frame &&
              dx * dx + dy * dy + dz * dz < match2) {
            slot = s;
            std::memcpy(slot_pos + i * 4, sl.pos, sizeof(float) * 3);
            slot_pos[i * 4 + 3] = sl.far_u;
            break;
          }
        }
        if (slot < 0 && t.caster && m_lamp_ready && t.map_slot >= 0 && t.map_slot < m_lamp_alloc &&
            t.mapw > 0.f && sw > 0.f) {
          // (its map no longer wanted, but still drawn for it: eased out, not a switch to the march)
          const auto& sl = m_lamp_slots[t.map_slot];
          const float dx = sl.pos[0] - t.world[0], dy = sl.pos[1] - t.world[1],
                      dz = sl.pos[2] - t.world[2];
          if (sl.used && sl.faces >= 6 && dx * dx + dy * dy + dz * dz < match2) {
            t.mapw = std::max(0.f, t.mapw - map_rate);
            if (t.mapw > 0.f) {
              slot = t.map_slot;
              std::memcpy(slot_pos + i * 4, sl.pos, sizeof(float) * 3);
              slot_pos[i * 4 + 3] = sl.far_u;
            }
          }
        } else if (slot >= 0) {
          // (a lamp lit just now, or after a cut: its map at once)
          t.mapw = (t.w < 0.1f || sw <= 0.f) ? 1.f : std::min(1.f, t.mapw + map_rate);
        }
        if (slot < 0) {
          t.mapw = 0.f;
        }
        t.map_slot = slot;
        const float q = std::round(std::clamp(sw, 0.f, 1.f) * 15.f);
        light_slot[i] = 32.f * q + (slot >= 0 ? (float)slot + 0.9f * t.mapw : 16.f);
        stab_sw[i] = q / 15.f;
        stab_slot[i] = slot;
      }
      static const EnhancedTune t_stab("lights.stab_log", 0.f);
      if (t_stab > 0.f) {
        auto& st = g_light_stab;
        const float ix = 1.f / std::max(std::abs(m_last_proj[2]), 1e-3f);
        const float iy = 1.f / std::max(std::abs(m_last_proj[3]), 1e-3f);
        const float nx = 1.f / std::sqrt(1.f + ix * ix), ny = 1.f / std::sqrt(1.f + iy * iy);
        auto touches_view = [&](const float* w, float r, bool before = false) {
          const float* tr = before ? g_light_stab.prev_trans : cam.trans;
          float v[3] = {0.f, 0.f, 0.f};
          for (int col = 0; col < 3; col++) {
            for (int row = 0; row < 3; row++) {
              v[col] += (w[row] - tr[row]) *
                        (before ? g_light_stab.prev_rot[row][col] : cam.rot[row][col]);
            }
          }
          return v[2] > -r && (std::abs(v[0]) - v[2] * ix) * nx <= r &&
                 (std::abs(v[1]) - v[2] * iy) * ny <= r;
        };
        std::vector<LightStab::L> cur(n);
        for (int i = 0; i < n; i++) {
          auto& l = cur[i];
          for (int row = 0; row < 3; row++) {
            l.world[row] = cam.trans[row];
            for (int col = 0; col < 3; col++) {
              l.world[row] += cam.rot[row][col] * pos[i * 4 + col];
            }
          }
          l.radius = pos[i * 4 + 3];
          l.luma = color[i * 4] + color[i * 4 + 1] + color[i * 4 + 2];
          l.sw = i < 8 ? stab_sw[i] : 0.f;
          l.slot = i < 8 ? stab_slot[i] : -1;
          l.instant = light_instant[i];
          l.id = i < (int)up_track.size() ? tracks[up_track[i]].id : 0;
          l.moving = i < (int)up_track.size() && tracks[up_track[i]].moving;
        }
        // (a cut: the first frame, or the camera jumped: no events)
        static float last_cam[3] = {0.f, 0.f, 0.f};
        const float cj = std::sqrt((cam.trans[0] - last_cam[0]) * (cam.trans[0] - last_cam[0]) +
                                   (cam.trans[1] - last_cam[1]) * (cam.trans[1] - last_cam[1]) +
                                   (cam.trans[2] - last_cam[2]) * (cam.trans[2] - last_cam[2]));
        std::memcpy(last_cam, cam.trans, sizeof(last_cam));
        const bool cut = st.first || cj > 20.f * kMeter;
        st.first = false;
        std::vector<int> cm(n, -1);
        std::vector<char> pm(st.prev.size(), 0);
        {
          std::vector<std::tuple<float, int, int>> pairs;
          for (int i = 0; i < n; i++) {
            for (int j = 0; j < (int)st.prev.size(); j++) {
              const auto& a = cur[i].world;
              const auto& b = st.prev[j].world;
              const float d2 = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                               (a[2] - b[2]) * (a[2] - b[2]);
              // (the same track; by place only a light with none: two tracks at one lamp, one
              // leaving the view as the other comes in, read as one light's jump)
              if (cur[i].id != 0 && cur[i].id == st.prev[j].id) {
                pairs.emplace_back(-1.f, i, j);
              } else if ((cur[i].id == 0 || st.prev[j].id == 0) && d2 < kMeter * kMeter) {
                pairs.emplace_back(d2, i, j);
              }
            }
          }
          std::sort(pairs.begin(), pairs.end());
          for (const auto& [d2, i, j] : pairs) {
            if (cm[i] < 0 && !pm[j]) {
              cm[i] = j;
              pm[j] = 1;
            }
          }
        }
        const bool ev = t_stab > 1.5f;
        st.events = ev;
        st.used = std::max(st.used, n);
        st.frames++;
        for (int i = 0; i < n && !cut; i++) {
          auto& l = cur[i];
          l.cast_t0 = m_time;
          if (cm[i] >= 0 && st.prev[cm[i]].sw > 0.f) {
            l.cast_t0 = st.prev[cm[i]].cast_t0;
          }
          const float d = std::sqrt(pos[i * 4] * pos[i * 4] + pos[i * 4 + 1] * pos[i * 4 + 1] +
                                    pos[i * 4 + 2] * pos[i * 4 + 2]) / kMeter;
          if (cm[i] < 0) {
            if (!l.instant) {
              st.nw++;
              st.newnear += d < 25.f ? 1 : 0;
              st.newpop += l.luma > 0.1f ? 1 : 0;
              st.newvis += (l.luma > 0.1f && touches_view(l.world, l.radius, true)) ? 1 : 0;
              if (ev) {
                lg::info("[enhanced] lightstab+ new {:.1f} m luma {:.3f} idx {} sw {:.2f}", d, l.luma, i, l.sw);
              }
            }
            continue;
          }
          const auto& p = st.prev[cm[i]];
          if (l.instant || p.instant) {
            continue;
          }
          const float dj = std::abs(l.luma - p.luma);
          if (!l.moving && !p.moving) {
            st.jump = std::max(st.jump, dj);
          }
          if (dj > 0.08f && (l.moving || p.moving)) {
            st.mjumps++;
          } else if (dj > 0.08f) {
            st.jumps++;
            if (ev) {
              const LightTrack* t = i < (int)up_track.size() ? &tracks[up_track[i]] : nullptr;
              lg::info("[enhanced] lightstab+ jump {:.1f} m {:.3f} -> {:.3f} idx {} id {} prev id {} w {:.2f} "
                       "shown {:.2f} s cand {} mem {} moving {} r {:.1f} m",
                       d, p.luma, l.luma, i, l.id, p.id, t ? t->w : -1.f,
                       t ? m_time - t->shown_since : -1.0, t ? t->cand : -2, t && t->mem ? 1 : 0,
                       t && t->moving ? 1 : 0, l.radius / kMeter);
            }
          }
          if (l.luma > 0.05f || p.luma > 0.05f) {
            if ((l.sw > 0.f) != (p.sw > 0.f)) {
              st.cast++;
              st.swaps += l.sw > 0.f ? 1 : 0;
              st.castvis += (d < 30.f && touches_view(l.world, l.radius)) ? 1 : 0;
              if (ev) {
                lg::info("[enhanced] lightstab+ cast {:.1f} m sw {:.2f} -> {:.2f} idx {} id {} luma {:.3f} "
                         "r {:.1f} m view {}",
                         d, p.sw, l.sw, i, l.id, l.luma, l.radius / kMeter,
                         touches_view(l.world, l.radius) ? 1 : 0);
              }
            }
            if (std::abs(l.sw - p.sw) > 0.2f) {
              st.castpop++;
              if (ev) {
                lg::info("[enhanced] lightstab+ castpop {:.1f} m sw {:.2f} -> {:.2f} idx {} luma {:.3f}", d,
                         p.sw, l.sw, i, l.luma);
              }
            }
            if (l.slot != p.slot && l.sw > 0.f) {
              st.mapsw++;
              // (a real switch: from one map, or from the march after it showed a while)
              st.mapsw2 += (p.sw > 0.f && (p.slot >= 0 || m_time - p.cast_t0 > 1.0)) ? 1 : 0;
              if (ev) {
                lg::info("[enhanced] lightstab+ mapsw {:.1f} m slot {} -> {} idx {} sw {:.2f} ready {}", d,
                         p.slot, l.slot, i, l.sw, m_lamp_ready ? 1 : 0);
              }
            }
          }
        }
        for (size_t j = 0; j < st.prev.size() && !cut; j++) {
          const auto& p = st.prev[j];
          if (!pm[j] && !p.instant && p.luma > 0.05f && touches_view(p.world, p.radius)) {
            st.gone++;
            if (ev) {
              const LightTrack* t = nullptr;
              for (const auto& o : tracks) {
                t = o.id == p.id ? &o : t;
              }
              lg::info("[enhanced] lightstab+ gone luma {:.3f} sw {:.2f} id {} track {} w {:.2f} shown {} "
                       "in_view {} cand {} moving {} flash {}",
                       p.luma, p.sw, p.id, t ? 1 : 0, t ? t->w : -1.f, t && t->shown ? 1 : 0,
                       t && t->in_view ? 1 : 0, t ? t->cand : -2, p.moving ? 1 : 0, t && t->flash ? 1 : 0);
            }
          }
        }
        st.prev = std::move(cur);
        std::memcpy(st.prev_trans, cam.trans, sizeof(st.prev_trans));
        std::memcpy(st.prev_rot, cam.rot, sizeof(st.prev_rot));
        if (m_time - st.last_print >= 1.0) {
          static const EnhancedTune t_tag("lights.stab_tag", 0.f);
          lg::info("[enhanced] lightstab tag {} cand {} inview {} used {} over32 {} new {} newnear {} "
                   "newpop {} newvis {} gone {} capdrop {} jump {:.3f} jumps {} mjumps {} cast {} castpop {} "
                   "mapsw {} assigns {} forgot {} frames {} swaps {} castvis {} mapsw2 {} nearmiss {} "
                   "cpu_us {:.1f} cjoin {} cdrop {} cseat {} cweak {}",
                   (int)(float)t_tag, st.cand, st.inview, st.used, st.over32, st.nw, st.newnear,
                   st.newpop, st.newvis, st.gone, st.capdrop, st.jump, st.jumps, st.mjumps, st.cast, st.castpop,
                   st.mapsw,
                   m_lamp_assigns >= st.assigns0 ? m_lamp_assigns - st.assigns0 : m_lamp_assigns,
                   st.forgot, st.frames, st.swaps, st.castvis, st.mapsw2, st.nearmiss,
                   st.cpu_n ? st.cpu_ns / st.cpu_n / 1000.0 : 0.0, st.cjoin, st.cdrop, st.cseat,
                   st.cweak);
          st.swaps = st.castvis = st.mapsw2 = st.nearmiss = st.cpu_n = 0;
          st.cjoin = st.cdrop = st.cseat = st.cweak = 0;
          st.cpu_ns = 0.0;
          st.last_print = m_time;
          st.assigns0 = m_lamp_assigns;
          st.cand = st.inview = st.used = st.over32 = st.nw = st.newnear = st.newpop = st.gone = 0;
          st.newvis = 0;
          st.capdrop = st.jumps = st.cast = st.castpop = st.mapsw = st.forgot = st.frames = st.mjumps = 0;
          st.jump = 0.f;
        }
      }
      static const EnhancedTune t_near("lights.shadow_near_m", 0.5f);
      glUniform4fv(glGetUniformLocation(id, "lamp_shadow_slot"), 2, light_slot);
      glUniform4fv(glGetUniformLocation(id, "lamp_shadow_pos"), 8, slot_pos);
      glUniform2f(glGetUniformLocation(id, "lamp_shadow_params"),
                  std::max((float)t_near, 0.05f) * kMeter, 2.f / std::max(m_lamp_res, 1));
      glActiveTexture(GL_TEXTURE0 + 35);
      glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, m_lamp_tex);
      glActiveTexture(GL_TEXTURE0);
      glUniform1i(glGetUniformLocation(id, "tex_T35"), 35);
    }
    ENH_CPU_SPLIT("tile-masks");
    {
      // the beacons, the brightest and nearest first, shown as the night falls
      // (a town's every lamp: Spargus's gate and walls are more than twenty)
      constexpr int kBeacons = 64;
      std::sort(beacons.begin(), beacons.end(),
                [](const Beacon& a, const Beacon& b) { return a.score > b.score; });
      const float shown = std::max(m_night, night_darkness());
      float bpos[kBeacons * 4] = {};
      float bcol[kBeacons * 4] = {};
      int nb = 0;
      for (const auto& b : beacons) {
        if (shown <= 0.01f || nb == kBeacons) {
          break;
        }
        // one point per lamp: a glow next to a brighter one already taken (a flame is several
        // glows) is part of it
        bool merged = false;
        for (int j = 0; j < nb && !merged; j++) {
          float d2 = 0.f;
          for (int k = 0; k < 3; k++) {
            float dk = bpos[j * 4 + k] - b.pos[k];
            d2 += dk * dk;
          }
          merged = d2 < 4.f * kMeter * 4.f * kMeter;
        }
        if (merged) {
          continue;
        }
        std::memcpy(bpos + nb * 4, b.pos, sizeof(float) * 4);
        std::memcpy(bcol + nb * 4, b.color, sizeof(float) * 4);
        bcol[nb * 4 + 3] *= shown * m_settings.light_strength;
        nb++;
      }
      glUniform1i(glGetUniformLocation(id, "beacon_count"), nb);
      glUniform4fv(glGetUniformLocation(id, "beacon_pos"), kBeacons, bpos);
      glUniform4fv(glGetUniformLocation(id, "beacon_color"), kBeacons, bcol);
      if (m_settings.ssao_debug && (debug_beacons++ % 300) == 0) {
        lg::info("[enhanced] beacons: {} far glows, {} shown", beacons.size(), nb);
      }
      // beacon tiles (beacons.tiles): the composite draws a beacon only within 40 px of its point
      // and only when that point is on screen, so each one's tiles are that square's (the same
      // projection as the shader's, half a tile of margin); its loop then walks only those.
      // Default on: 2560x1600 Spargus 21h composite 11.70 -> 11.17 ms in two A/B pairs of one run,
      // frames off/on equal to two frames of one setting (the beacons' tiles checked drawn)
      static const EnhancedTune t_btiles("beacons.tiles", 1.f);
      if (t_btiles > 0.5f && nb > 0) {
        tile_flags |= 2;
        const float fx = m_last_proj[2], fy = m_last_proj[3];
        const float ru = 40.f / std::max(w, 1), rv = 40.f / std::max(h, 1);
        for (int i = 0; i < nb; i++) {
          const float* bp = bpos + i * 4;
          if (bp[2] == 0.f) {
            continue;
          }
          const float u = 0.5f * fx * bp[0] / bp[2] + 0.5f, v = 0.5f * fy * bp[1] / bp[2] + 0.5f;
          if (!(u > 0.f && u < 1.f && v > 0.f && v < 1.f)) {
            continue;
          }
          const int x0 = std::max((int)std::floor((u - ru) * kTx - 0.5f), 0);
          const int x1 = std::min((int)std::floor((u + ru) * kTx + 0.5f), kTx - 1);
          const int y0 = std::max((int)std::floor((v - rv) * kTy - 0.5f), 0);
          const int y1 = std::min((int)std::floor((v + rv) * kTy + 0.5f), kTy - 1);
          for (int ty = y0; ty <= y1; ty++) {
            for (int tx = x0; tx <= x1; tx++) {
              tile_bits[(ty * kTx + tx) * 4 + 1 + (i >> 5)] |= 1u << (i & 31);
            }
          }
        }
      }
    }
    ENH_CPU_SPLIT("b2003");
    {
      // (as a 32x18 integer texture on unit 28: as uniforms they took the composite past NVIDIA's
      // 1024 constant registers)
      if (tile_flags) {
        static GLuint tiles_tex = 0;
        glActiveTexture(GL_TEXTURE28);
        if (!tiles_tex) {
          glGenTextures(1, &tiles_tex);
          glBindTexture(GL_TEXTURE_2D, tiles_tex);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, kTx, kTy, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                       nullptr);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        glBindTexture(GL_TEXTURE_2D, tiles_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTx, kTy, GL_RGBA_INTEGER, GL_UNSIGNED_INT,
                        tile_bits);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(id, "tex_T28"), 28);
      }
      glUniform1f(glGetUniformLocation(id, "light_tiles_on"), (float)tile_flags);
    }
    static int debug_counter = 0;
    ENH_CPU_SPLIT("b2073");
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
  ENH_CPU_SPLIT("ssao-dbg");
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
  ENH_CPU_SPLIT("b2108");
  m_ocean_ready = false;

  // camera motion blur: view -> previous view is R^T Rp, with the camera's move as an offset
  float strength = 0.f;
  if (reproject && m_settings.motion_blur > 0.f) {
    strength = m_settings.motion_blur;
    glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
    glUniform3f(glGetUniformLocation(id, "to_prev_offset"), offset[0], offset[1], offset[2]);
  }
  // (the board's L2 boost: a stronger, longer blur)
  const float boost = have_scene && g_enhanced_player.valid ? g_enhanced_board_boost.load() : 0.f;
  glUniform4f(glGetUniformLocation(id, "motion"), m_prev_camera.fx, m_prev_fy,
              strength * (1.f + 1.5f * boost), 0.02f * (1.f + boost));
  // (sharp out past Jak: the blur takes the world as still, and Jak moves with the camera. Boosting,
  // the camera trails him farther than 10 m and his own pixels were smeared into the ground behind:
  // he looked see-through)
  float keep = 10.f * kMeter;
  if (have_scene && g_enhanced_player.valid) {
    float d2 = 0.f;
    for (int c = 0; c < 3; c++) {
      d2 += (g_enhanced_player.pos[c] - cam.trans[c]) * (g_enhanced_player.pos[c] - cam.trans[c]);
    }
    keep = std::max(keep, std::sqrt(d2) + 4.f * kMeter);
  }
  glUniform2f(glGetUniformLocation(id, "motion_keep"), keep, keep + 15.f * kMeter);

  // the clouds first, at a quarter of the pixels (the composite then reads them)
  // (JAK_ENHANCED_CLOUDS_FULL: marched at every pixel, as before, to compare)
  static const bool clouds_full = std::getenv("JAK_ENHANCED_CLOUDS_FULL") != nullptr;
  const bool clouds = have_scene && m_settings.clouds && m_settings.cloud_amount > 0.f && !clouds_full;
  if (clouds) {
    if (reproject) {
      glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
    }
    draw_clouds(id, w, h, reproject, render_fbo, prof, render_state);
  } else {
    m_cloud_history = false;
    glUniform4f(glGetUniformLocation(id, "cloud_info"), 0.f, 0.f, 0.f, 0.f);
  }
  // the world's sun shadow looks at half size (tuning "shadow.half")
  ENH_CPU_SPLIT("mb+player+clouds");
  {
    // the composite's shadow samplers (sun maps T9/T22/T26, lamp maps T35) never on an empty unit:
    // a map not made this frame left texture 0 there, and every draw logged GL's "undefined
    // behavior" warning (~40 a second in Spargus at dusk). A 1x1 depth texture, compared, lit
    static GLuint dummy_2d = 0, dummy_cube = 0;
    if (!dummy_2d) {
      enh_post::KeepTexture2D keep;
      const float one[6] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
      glGenTextures(1, &dummy_2d);
      glBindTexture(GL_TEXTURE_2D, dummy_2d);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, one);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
      GLint old_cube = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP_ARRAY, &old_cube);
      glGenTextures(1, &dummy_cube);
      glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, dummy_cube);
      glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_DEPTH_COMPONENT32F, 1, 1, 6, 0, GL_DEPTH_COMPONENT,
                   GL_FLOAT, one);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
      glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, old_cube);
    }
    for (int unit : {9, 22, 26}) {
      glActiveTexture(GL_TEXTURE0 + unit);
      GLint bound = 0;
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
      if (!bound) {
        glBindTexture(GL_TEXTURE_2D, dummy_2d);
      }
    }
    glActiveTexture(GL_TEXTURE0 + 35);
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP_ARRAY, &bound);
    if (!bound) {
      glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, dummy_cube);
      glUniform1i(glGetUniformLocation(id, "tex_T35"), 35);
    }
    glActiveTexture(GL_TEXTURE0);
  }
  if (have_scene) {
    draw_sun_half(id, w, h, render_fbo, prof, render_state);
  } else {
    glUniform1f(glGetUniformLocation(id, "sun_half_on"), 0.f);
  }

  // (test switch, what each part of the composite costs, in one run:
  // JAK_ENHANCED_COMPOSITE_OFF="sand;rock_detail;light_count,lantern": every 3 s the next group of
  // uniforms is zeroed (first: none), the draw timed on the GPU (waited for: the clock stays up);
  // after two rounds the log has each group's average)
  ENH_CPU_SPLIT("b2160");
  struct CostProbe {
    std::vector<std::vector<std::string>> groups;
    std::vector<double> sum;
    std::vector<int> count;
    double start = -1.0;
    bool done = false;
    GLuint query = 0;
  };
  static CostProbe probe = [] {
    CostProbe out;
    if (const char* env = std::getenv("JAK_ENHANCED_COMPOSITE_OFF")) {
      std::string s = env;
      out.groups.push_back({});  // everything on
      std::vector<std::string> group;
      std::string name;
      for (size_t i = 0; i <= s.size(); i++) {
        char ch = i < s.size() ? s[i] : ';';
        if (ch == ',' || ch == ';') {
          if (!name.empty()) {
            group.push_back(name);
          }
          name.clear();
          if (ch == ';' && !group.empty()) {
            out.groups.push_back(group);
            group.clear();
          }
        } else {
          name += ch;
        }
      }
      out.sum.assign(out.groups.size(), 0.0);
      out.count.assign(out.groups.size(), 0);
    }
    return out;
  }();
  int probe_group = -1;
  // (test switch "debug": {"composite_probe": n}: each new n > 0 runs the probe again from the
  // start, 2 s later, so it can time a place reached with debug.tp / debug.hour in the same run;
  // < 0 at boot: no run of its own 10 s after the first frame)
  static const EnhancedTune t_probe("debug.composite_probe", 0.f);
  if (!probe.groups.empty() && probe.start < 0.0 && !probe.done && (float)t_probe < 0.f) {
    probe.done = true;
  }
  {
    static float s_probe = 0.f;
    if (!probe.groups.empty() && (float)t_probe > 0.f && (float)t_probe != s_probe) {
      s_probe = (float)t_probe;
      const double now =
          std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      if (!probe.query) {
        glGenQueries(1, &probe.query);
      }
      probe.start = now + 2.0;
      probe.done = false;
      probe.sum.assign(probe.groups.size(), 0.0);
      probe.count.assign(probe.groups.size(), 0);
      lg::info("[enhanced] composite cost probe {} restarted", s_probe);
    }
  }
  if (!probe.groups.empty() && !probe.done) {
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (probe.start < 0.0) {
      probe.start = now + 10.0;  // (the level settles first)
      glGenQueries(1, &probe.query);
    }
    const double t = now - probe.start;
    const int slots = (int)probe.groups.size() * 2;
    // (JAK_ENHANCED_COMPOSITE_SLOT: seconds per group, default 3; a long list in less time)
    static const double slot = [] {
      const char* e = std::getenv("JAK_ENHANCED_COMPOSITE_SLOT");
      return e ? std::clamp(std::atof(e), 0.8, 10.0) : 3.0;
    }();
    if (t >= 0.0 && t < slots * slot) {
      probe_group = (int)(t / slot) % (int)probe.groups.size();
      if (std::fmod(t, slot) < std::min(0.5, slot * 0.3)) {
        probe_group = -probe_group - 2;  // (switched just now: not timed yet, but zeroed)
      }
    } else if (t >= slots * slot) {
      probe.done = true;
      for (size_t g = 0; g < probe.groups.size(); g++) {
        std::string names;
        for (auto& n : probe.groups[g]) {
          names += (names.empty() ? "" : ",") + n;
        }
        lg::info("[enhanced] composite cost: {:6.2f} ms without {}",
                 probe.count[g] ? probe.sum[g] / probe.count[g] : 0.0,
                 names.empty() ? "(nothing: all on)" : names);
      }
    }
  }
  const int zero_group = probe_group >= 0 ? probe_group : (probe_group < -1 ? -probe_group - 2 : -1);
  if (zero_group >= 0) {
    GLint n = 0;
    glGetProgramiv(id, GL_ACTIVE_UNIFORMS, &n);
    for (auto& name : probe.groups[zero_group]) {
      GLint loc = glGetUniformLocation(id, name.c_str());
      GLenum type = 0;
      for (GLint i = 0; loc >= 0 && i < n; i++) {
        char buf[128];
        GLint size;
        GLenum t;
        glGetActiveUniform(id, i, sizeof(buf), nullptr, &size, &t, buf);
        if (name == buf) {
          type = t;
        }
      }
      if (type == GL_INT) {
        glUniform1i(loc, 0);
      } else if (type == GL_FLOAT) {
        glUniform1f(loc, 0.f);
      } else if (type == GL_FLOAT_VEC2) {
        glUniform2f(loc, 0.f, 0.f);
      } else if (type == GL_FLOAT_VEC3) {
        glUniform3f(loc, 0.f, 0.f, 0.f);
      } else if (type == GL_FLOAT_VEC4) {
        glUniform4f(loc, 0.f, 0.f, 0.f, 0.f);
      }
    }
  }
  if (probe_group >= 0) {
    glBeginQuery(GL_TIME_ELAPSED, probe.query);
  }
  ENH_CPU_SPLIT("probe");
  setup_span.reset();
  {
    static double cpu_setup = 0.0, cpu_total = 0.0;
    static int cpu_n = 0;
    static auto cpu_t = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    cpu_setup += std::chrono::duration<double, std::milli>(now - setup_cpu0).count();
    cpu_total += std::chrono::duration<double, std::milli>(now - post_cpu0).count();
    cpu_n++;
    if (std::chrono::duration<double>(now - cpu_t).count() >= 5.0) {
      lg::info("[enhanced] post cpu: to composite {:.2f} ms, of it setup {:.2f} ms (n={})",
               cpu_total / cpu_n, cpu_setup / cpu_n, cpu_n);
      if (lag_probes) {
        lg::info("[enhanced] post cpu setup: {}", g_setup_splits.report(cpu_n));
        lg::info("[enhanced] gpu lag behind cpu (ms): frame {:.2f}, {}, {}, {}, {}, {}", m_gpu_lag_ms,
                 g_lag_start.report(), g_lag_composite.report(), g_lag_blit.report(), g_lag_taa.report(), g_lag_end.report());
      }
      cpu_setup = cpu_total = 0.0;
      cpu_n = 0;
      cpu_t = now;
    }
  }
  if (lag_probes) {
    g_lag_composite.mark(pass_timer_frame());
  }
  // (the scene on unit 0 once more, right before the draw: the passes run since it was bound
  // (clouds, their rays, the half-size sun shadows) make their own textures now and then, and one
  // that left unit 0 empty cost a whole frame, the world black under the sky)
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  vrs::begin();
  // (post.dry_composite 1: the composite in two programs. enh_composite_dry, without the water's
  // surface, draws every pixel but the water's; then, where water was drawn this frame, this one
  // with cloud_pass 2 only the water's pixels. The water's ~700 lines set every pixel's registers,
  // so fewer pixels ran at once: Spargus 18h at 2560x1600, all full rate, 30 -> 15 ms. Same picture)
  static const EnhancedTune t_dry("post.dry_composite", 1.f);
  const Shader& dry = render_state->shaders[ShaderId::ENH_COMPOSITE_DRY];
  const bool use_dry = (float)t_dry > 0.5f && dry.okay();
  float water_on = 0.f;
  if (use_dry) {
    glGetUniformfv(id, glGetUniformLocation(id, "water_strength"), &water_on);
    dry.copy_uniforms_from(shader);  // (binds it)
  }
  ENH_TIMED("enh-composite", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
  if (use_dry) {
    glUseProgram(id);
    if (water_on > 0.f) {
      glUniform1i(glGetUniformLocation(id, "cloud_pass"), 2);
      ENH_TIMED("enh-composite-wet", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
      glUniform1i(glGetUniformLocation(id, "cloud_pass"), 0);
    }
  }
  {
    // (test switch "debug": {"gpu_burn": n}: the composite drawn n more times, the same picture,
    // to make the graphics card the bottleneck when testing the auto quality)
    static const EnhancedTune t_gpu_burn("debug.gpu_burn", 0.f);
    for (int i = 0; i < std::min(64, (int)(float)t_gpu_burn); i++) {
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
  }
  vrs::end();
  if (probe_group >= 0) {
    glEndQuery(GL_TIME_ELAPSED);
    GLuint64 ns = 0;
    glGetQueryObjectui64v(probe.query, GL_QUERY_RESULT, &ns);
    probe.sum[probe_group] += ns / 1e6;
    probe.count[probe_group]++;
  }
  if (post_flush & 4) {
    glFlush();
  }
  static enh_post::PassTimer t_tail_span("enh-tail-span");
  enh_post::PassTimerScope tail_span(t_tail_span);
  prof.add_draw_call();
  glBindSampler(13, 0);
  if (taa && taa_mode == 1) {
    // the finished frame through the TAA, and back
    glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (lag_probes) {
      g_lag_blit.mark(pass_timer_frame());
    }
    resolve_taa(m_scene_tex);
    if (lag_probes) {
      g_lag_taa.mark(pass_timer_frame());
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_taa_fbo[1 - m_taa_cur]);  // (the one just written)
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, render_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // (test switch "debug": {"dump_frame": n}: each new n saves this frame at its full size, after
    // the TAA and before the HUD, as <JAK_ENHANCED_DUMP_DIR>/frame_<n>.png; one stall)
    static const EnhancedTune t_dump("debug.dump_frame", 0.f);
    static int dumped = 0;
    const int dump_n = (int)(float)t_dump;
    const char* dump_dir = std::getenv("JAK_ENHANCED_DUMP_DIR");
    if (dump_n > 0 && dump_n != dumped && dump_dir) {
      dumped = dump_n;
      std::vector<float> px((size_t)w * h * 4);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, px.data());
      std::vector<u32> out((size_t)w * h);
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          const float* p = &px[((size_t)(h - 1 - y) * w + x) * 4];
          u32 v = 0xff000000;
          for (int k = 0; k < 3; k++) {
            v |= (u32)std::clamp((int)std::lround(p[k] * 255.f), 0, 255) << (8 * k);
          }
          out[(size_t)y * w + x] = v;
        }
      }
      const std::string path = fmt::format("{}/frame_{}.png", dump_dir, dump_n);
      file_util::write_rgba_png(path, out.data(), w, h);
      lg::info("[enhanced] dump_frame: {} ({}x{})", path, w, h);
    }
  }

  m_prev_camera = cam;
  m_prev_fy = cam.fy * kScissorHeightScale;
  enh_probe::sample(enh_probe::kPost, render_fbo, 0, 0, w, h);

  if (lag_probes) {
    g_lag_end.mark(pass_timer_frame());
  }
  // the end of this frame's 3D and post work, for the auto quality
  if (m_gpu_query_open) {
    glQueryCounter(m_gpu_query[m_gpu_query_slot][2], GL_TIMESTAMP);
    m_gpu_query_used[m_gpu_query_slot] = true;
    m_gpu_query_slot = (m_gpu_query_slot + 1) % 4;
    m_gpu_query_open = false;
  }

  // leave the state the way the bucket renderers expect it
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
}

