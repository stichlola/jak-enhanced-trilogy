#include "EnhancedParticles.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/graphics/texture/TexturePool.h"

EnhancedFx g_enhanced_fx;
EnhancedShots g_enhanced_shots;

namespace {

// (units past those the game's shaders bind for themselves at draw time)
constexpr int kNoiseUnit = 30;
constexpr int kDepthUnit = 31;
constexpr float kMeter = 4096.f;

// a small 3D noise, tiling on all sides, two independent channels: fractal value noise made here
// (no game data). Its mips fade to grey, so far-off sprites stop being eroded by themselves
GLuint noise_texture() {
  static GLuint tex = 0;
  if (tex) {
    return tex;
  }
  constexpr int kSize = 32;
  std::vector<float> values(kSize * kSize * kSize * 2, 0.f);
  u32 state = 0x1234567u;
  auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return (float)(state >> 8) / (float)(1u << 24);
  };
  auto fade = [](float t) { return t * t * (3.f - 2.f * t); };
  auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  for (int channel = 0; channel < 2; channel++) {
    float amp = 1.f;
    for (int cells = 4; cells <= 16; cells *= 2, amp *= 0.5f) {
      std::vector<float> lattice(cells * cells * cells);
      for (auto& v : lattice) {
        v = next();
      }
      auto at = [&](int x, int y, int z) {
        return lattice[(x % cells) + (y % cells) * cells + (z % cells) * cells * cells];
      };
      const float scale = (float)cells / kSize;
      for (int z = 0; z < kSize; z++) {
        for (int y = 0; y < kSize; y++) {
          for (int x = 0; x < kSize; x++) {
            const float fx = x * scale, fy = y * scale, fz = z * scale;
            const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
            const float ux = fade(fx - ix), uy = fade(fy - iy), uz = fade(fz - iz);
            const float v = lerp(lerp(lerp(at(ix, iy, iz), at(ix + 1, iy, iz), ux),
                                      lerp(at(ix, iy + 1, iz), at(ix + 1, iy + 1, iz), ux), uy),
                                 lerp(lerp(at(ix, iy, iz + 1), at(ix + 1, iy, iz + 1), ux),
                                      lerp(at(ix, iy + 1, iz + 1), at(ix + 1, iy + 1, iz + 1), ux),
                                      uy),
                                 uz);
            values[(x + y * kSize + z * kSize * kSize) * 2 + channel] += v * amp;
          }
        }
      }
    }
    // stretched over the full range
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = channel; i < values.size(); i += 2) {
      lo = std::min(lo, values[i]);
      hi = std::max(hi, values[i]);
    }
    for (size_t i = channel; i < values.size(); i += 2) {
      values[i] = (values[i] - lo) / std::max(hi - lo, 1e-5f);
    }
  }
  std::vector<u8> bytes(values.size());
  for (size_t i = 0; i < values.size(); i++) {
    bytes[i] = (u8)std::clamp((int)(values[i] * 255.f + 0.5f), 0, 255);
  }
  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0 + kNoiseUnit);
  glBindTexture(GL_TEXTURE_3D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RG8, kSize, kSize, kSize, 0, GL_RG, GL_UNSIGNED_BYTE,
               bytes.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glGenerateMipmap(GL_TEXTURE_3D);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
  glActiveTexture(GL_TEXTURE0);
  return tex;
}

}  // namespace

// The classes sprite3_3d.vert tells apart further by the sprite's color:
// 1 normally blended smoke and flames: warm = a flame, grey = smoke, any other color (eco, magic)
//   left alone
// 2 added flames: always a flame
// 3 added puffs: a flame if warm (bigpuff is also the glow around eco and shots)
int EnhancedSpriteFx::classify(SharedRenderState* render_state,
                               u32 tbp,
                               u64 gl_texture,
                               bool alpha_blended,
                               bool additive) {
  const u64 key = (gl_texture << 2) | (alpha_blended ? 1 : 0) | (additive ? 2 : 0);
  const auto it = m_classes.find(key);
  if (it != m_classes.end()) {
    return it->second;
  }
  int result = 0;
  if (alpha_blended || additive) {
    // "page/name" (the same names in all three games)
    std::string name = render_state->texture_pool->get_debug_texture_name_from_tbp(tbp);
    const auto slash = name.rfind('/');
    if (slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
    static const char* const smoke[] = {"bigpuff", "dirtpuff01", "big-cloud", "ceiling-dust"};
    static const char* const flame[] = {"flame01", "explosion-nebula", "explosion-edge",
                                        "explo-texture", "mech-flame"};
    const auto is_in = [&](const auto& list) {
      return std::any_of(std::begin(list), std::end(list),
                         [&](const char* n) { return name == n; });
    };
    if (is_in(flame)) {
      result = alpha_blended ? 1 : 2;
    } else if (is_in(smoke)) {
      result = alpha_blended ? 1 : 3;
    }
    static const bool debug = std::getenv("JAK_ENHANCED_FX_DEBUG") != nullptr;
    if (debug) {
      lg::info("[fx] sprite texture {} {}: class {}", name, alpha_blended ? "blended" : "added",
               result);
    }
  }
  m_classes[key] = result;
  return result;
}

void EnhancedSpriteFx::copy_depth(SharedRenderState* render_state) {
  const int w = render_state->render_fb_w, h = render_state->render_fb_h;
  glActiveTexture(GL_TEXTURE0 + kDepthUnit);
  if (!m_depth_fbo || w != m_depth_w || h != m_depth_h) {
    if (!m_depth_fbo) {
      glGenFramebuffers(1, &m_depth_fbo);
      glGenTextures(1, &m_depth_tex);
    }
    m_depth_w = w;
    m_depth_h = h;
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    // (the render buffer's format: a depth blit needs the same on both sides)
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
  }
  // (and for msaa, resolves it)
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_state->render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
}

void EnhancedSpriteFx::begin_flush(SharedRenderState* render_state,
                                   bool on,
                                   const float* camera) {
  const auto shader = render_state->shaders[ShaderId::SPRITE3].id();
  glUniform1i(glGetUniformLocation(shader, "enh_noise"), kNoiseUnit);
  glUniform1i(glGetUniformLocation(shader, "enh_depth"), kDepthUnit);
  m_class_loc = glGetUniformLocation(shader, "enh_fx_class");
  m_on = on && g_enhanced_fx.detail > 0.f;
  glUniform1i(m_class_loc, 0);
  if (!m_on) {
    return;
  }
  glActiveTexture(GL_TEXTURE0 + kNoiseUnit);
  glBindTexture(GL_TEXTURE_3D, noise_texture());
  // (the depth copied earlier this frame, for a later flush)
  glActiveTexture(GL_TEXTURE0 + kDepthUnit);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  static const bool debug = std::getenv("JAK_ENHANCED_FX_DEBUG") != nullptr;
  glUniform1i(glGetUniformLocation(shader, "enh_fx_debug"), debug ? 1 : 0);
  glUniform2f(glGetUniformLocation(shader, "enh_fx"), g_enhanced_fx.time, g_enhanced_fx.detail);
  // the depth buffer's numbers to distances (no game camera this frame: no soft edges)
  glUniform2f(glGetUniformLocation(shader, "enh_depth_ab"),
              g_enhanced_camera.valid ? g_enhanced_camera.depth_a : 0.f,
              g_enhanced_camera.valid ? g_enhanced_camera.depth_b : 0.f);
  // the sun and world up along the screen (the framebuffer's x and y) and toward the camera, read
  // off the matrix the sprites themselves are placed with: x, -y (the PS2's y points down) and -w
  float axis[3][3];
  for (int a = 0; a < 3; a++) {
    const int row = a == 2 ? 3 : a;
    const float sign = a == 0 ? 1.f : -1.f;
    float len = 0.f;
    for (int k = 0; k < 3; k++) {
      axis[a][k] = camera[k * 4 + row];
      len += axis[a][k] * axis[a][k];
    }
    len = std::sqrt(std::max(len, 1e-12f));
    for (int k = 0; k < 3; k++) {
      axis[a][k] *= sign / len;
    }
  }
  const float* s = g_enhanced_fx.sun;
  glUniform4f(glGetUniformLocation(shader, "enh_sun"),
              s[0] * axis[0][0] + s[1] * axis[0][1] + s[2] * axis[0][2],
              s[0] * axis[1][0] + s[1] * axis[1][1] + s[2] * axis[1][2],
              s[0] * axis[2][0] + s[1] * axis[2][1] + s[2] * axis[2][2], s[3]);
  glUniform2f(glGetUniformLocation(shader, "enh_rise"), axis[0][1], axis[1][1]);
}

void EnhancedSpriteFx::set_bucket(SharedRenderState* render_state,
                                  u32 tbp,
                                  u64 gl_texture,
                                  bool alpha_blended,
                                  bool additive) {
  if (m_on) {
    const int c = classify(render_state, tbp, gl_texture, alpha_blended, additive);
    if (c != 0 && m_depth_frame != render_state->frame_idx) {
      m_depth_frame = render_state->frame_idx;
      copy_depth(render_state);
    }
    glUniform1i(m_class_loc, c);
  }
}

namespace {
struct Decal {
  float pos[3];
  float radius;
  int kind;
  float time;
};
std::vector<Decal> g_decals;
constexpr int kMaxScorches = 12, kMaxMarks = 16;
}  // namespace

void enhanced_add_decal(const float* pos, float radius, int kind, float time) {
  // full: the oldest of its sort goes (rapid fire doesn't wipe the scorch marks)
  const int most = kind == 0 ? kMaxScorches : kMaxMarks;
  int count = 0;
  int oldest = -1;
  for (int i = 0; i < (int)g_decals.size(); i++) {
    if ((g_decals[i].kind == 0) == (kind == 0)) {
      count++;
      if (oldest < 0) {
        oldest = i;
      }
    }
  }
  if (count >= most) {
    g_decals.erase(g_decals.begin() + oldest);
  }
  g_decals.push_back({{pos[0], pos[1], pos[2]}, radius, kind, time});
}

void enhanced_upload_decals(u64 shader, const float* cam_pos, float time, bool on) {
  constexpr int kMax = kMaxScorches + kMaxMarks;
  std::erase_if(g_decals,
                [&](const Decal& d) { return time - d.time > (d.kind == 0 ? 60.f : 25.f); });
  float decal[kMax * 4] = {};
  float info[kMax * 2] = {};
  int count = 0;
  for (const auto& d : g_decals) {
    // (the composite looks no further than 110 m)
    if (!on || count == kMax ||
        std::hypot(d.pos[0] - cam_pos[0], d.pos[1] - cam_pos[1], d.pos[2] - cam_pos[2]) >
            120.f * kMeter) {
      continue;
    }
    std::memcpy(decal + count * 4, d.pos, sizeof(d.pos));
    decal[count * 4 + 3] = d.radius;
    info[count * 2] = time - d.time;
    info[count * 2 + 1] = (float)d.kind;
    count++;
  }
  glUniform1i(glGetUniformLocation(shader, "decal_count"), count);
  glUniform4fv(glGetUniformLocation(shader, "decal"), kMax, decal);
  glUniform2fv(glGetUniformLocation(shader, "decal_info"), kMax, info);
}

int EnhancedGroundProbe::ask(const float* pos,
                             const EnhancedCamera& cam,
                             const std::array<float, 4>& proj,
                             u32 depth_fbo,
                             int w,
                             int h) {
  const int ticket = m_next_ticket++;
  auto& s = m_slots[ticket % kSlots];
  if (!s.pbo) {
    glGenBuffers(1, &s.pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, kPoints * sizeof(float), nullptr, GL_STREAM_READ);
  }
  if (s.fence) {
    glDeleteSync((GLsync)s.fence);
  }
  s.ticket = ticket;
  std::memcpy(s.top, pos, sizeof(s.top));
  for (int col = 0; col < 3; col++) {
    s.cam_rot_y[col] = cam.rot[1][col];
  }
  s.cam_y = cam.trans[1];
  s.depth_a = proj[0];
  s.depth_b = proj[1];
  GLint old_read = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
  for (int k = 0; k < kPoints; k++) {
    float* v = s.view[k];
    for (int col = 0; col < 3; col++) {
      v[col] = 0.f;
      for (int row = 0; row < 3; row++) {
        const float world = pos[row] - (row == 1 ? k * 0.75f * kMeter : 0.f);
        v[col] += (world - cam.trans[row]) * cam.rot[row][col];
      }
    }
    s.on_screen[k] = false;
    if (v[2] > 0.5f * kMeter) {
      const int px = (int)((v[0] * proj[2] / v[2] * 0.5f + 0.5f) * w);
      const int py = (int)((v[1] * proj[3] / v[2] * 0.5f + 0.5f) * h);
      if (px >= 0 && px < w && py >= 0 && py < h) {
        s.on_screen[k] = true;
        glReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void*)(k * sizeof(float)));
      }
    }
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
  s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  return ticket;
}

bool EnhancedGroundProbe::poll(int ticket, bool* found, float* y) {
  *found = false;
  auto& s = m_slots[ticket % kSlots];
  if (s.ticket != ticket || !s.fence) {
    return true;  // dropped
  }
  const GLenum state = glClientWaitSync((GLsync)s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
  if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED) {
    return false;
  }
  glDeleteSync((GLsync)s.fence);
  s.fence = nullptr;
  s.ticket = -1;
  glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
  if (const auto* d = (const float*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                                     kPoints * sizeof(float), GL_MAP_READ_BIT)) {
    // where a point gone under the ground is hidden, the depth there is the ground on its way: the
    // lowest such hit (higher ones are legs, the body the blast hit). Nothing further than 10 m off:
    // that's a wall in front of it all
    for (int k = 0; k < kPoints; k++) {
      if (!s.on_screen[k] || d[k] <= 1e-7f) {
        continue;
      }
      const float* v = s.view[k];
      const float z = std::abs(s.depth_b / (d[k] * 2.f - 1.f - s.depth_a));
      if (z > v[2] + 0.1f * kMeter || (v[2] - z) * std::hypot(v[0] / v[2], 1.f) > 10.f * kMeter) {
        continue;
      }
      const float scale = z / v[2];
      const float hit_y = s.cam_y + (s.cam_rot_y[0] * v[0] + s.cam_rot_y[1] * v[1] +
                                     s.cam_rot_y[2] * v[2]) * scale;
      const float point_y = s.top[1] - k * 0.75f * kMeter;
      if (hit_y > s.top[1] + 0.3f * kMeter || hit_y < point_y - 0.3f * kMeter) {
        continue;
      }
      if (!*found || hit_y < *y) {
        *found = true;
        *y = hit_y;
      }
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  return true;
}

// the explosions' layers beyond what the composite draws by itself: for each new one, once the ground
// under it is known, the smoke that stays behind (the game's puffs). Test switch:
// JAK_ENHANCED_BLAST_TEST=seconds[,radius m]: an explosion 6 m ahead of Jak every so often
void EnhancedPost::update_blast_fx(const EnhancedCamera& cam, bool on, int w, int h) {
  const auto& pl = g_enhanced_player;
  static const char* test = std::getenv("JAK_ENHANCED_BLAST_TEST");
  static double test_last = -1e9;
  if (test && on && pl.valid) {
    float every = 4.f, radius = 4.f;
    std::sscanf(test, "%f,%f", &every, &radius);
    if (m_time - test_last > every) {
      test_last = m_time;
      const float fx = cam.rot[0][2], fz = cam.rot[2][2];
      const float len = std::max(std::sqrt(fx * fx + fz * fz), 0.01f);
      Blast b{{pl.pos[0] + fx / len * 6.f * kMeter, pl.pos[1] + 0.5f * kMeter,
               pl.pos[2] + fz / len * 6.f * kMeter},
              radius * kMeter,
              m_time};
      m_blasts.push_back(b);
    }
  }
  if (!on || m_settings.fx_detail <= 0.f || m_settings.blast <= 0.f) {
    return;
  }
  for (auto& b : m_blasts) {
    if (b.kind != 0 || b.fx_done) {
      continue;
    }
    if (b.probe < 0) {
      b.probe = m_ground_probe.ask(b.pos, cam, m_last_proj, m_depth_fbo, w, h);
    }
    bool found = false;
    float y = 0.f;
    if (!m_ground_probe.poll(b.probe, &found, &y)) {
      continue;
    }
    b.fx_done = true;
    // off screen or hidden: Jak's ground, if he is near and on it
    if (!found && pl.valid && pl.state != 0 && pl.state != 6 && pl.state != 7 &&
        std::abs(pl.pos[1] - b.pos[1]) < 5.f * kMeter &&
        std::hypot(pl.pos[0] - b.pos[0], pl.pos[2] - b.pos[2]) < 40.f * kMeter) {
      found = true;
      y = pl.pos[1];
    }
    if (found) {
      b.ground = y;
      // (not for one going off high in the air)
      if (b.pos[1] - y < std::max(2.5f * kMeter, b.radius * 0.8f)) {
        const float at[3] = {b.pos[0], y, b.pos[2]};
        enhanced_add_decal(at, std::clamp(b.radius * 0.55f, 1.f * kMeter, 5.5f * kMeter), 0,
                           (float)m_time);
      }
    }
    // what stays behind: a few columns of smoke drifting off, sized by the blast
    if (m_settings.blast_sparks > 0.f) {
      const float r = std::clamp(b.radius / kMeter, 1.5f, 10.f);
      for (int k = 0; k < 3; k++) {
        Puff f;
        const float a = k * 2.1f + (float)(std::rand() % 100) * 0.01f;
        f.pos[0] = b.pos[0] + std::cos(a) * r * 0.25f * kMeter;
        f.pos[1] = found ? y : b.pos[1];
        f.pos[2] = b.pos[2] + std::sin(a) * r * 0.25f * kMeter;
        f.size = std::clamp(r * 0.45f, 1.f, 4.f);
        f.kind = 3;
        f.seed = (float)(std::rand() % 1000) / 1000.f;
        f.life = 4.f + k;
        f.time = m_time;
        m_puffs.push_back(f);
      }
    }
  }
}

namespace {

// what each gun's shot leaves: color, half width (m), how long a point of it lasts (s)
struct ShotStyle {
  float color[3];
  float width;
  float life;
};
constexpr ShotStyle kShotStyles[5] = {
    {{1.f, 1.f, 1.f}, 0.1f, 0.3f},       // (no such gun)
    {{1.f, 0.25f, 0.1f}, 0.16f, 0.35f},  // red: the grenade's hot streak
    {{0.35f, 0.7f, 1.f}, 0.07f, 0.22f},  // blue: thin, crackling
    {{1.f, 0.75f, 0.2f}, 0.09f, 0.3f},   // yellow: a long tracer
    {{0.7f, 0.2f, 1.f}, 0.22f, 0.5f},    // dark: wide, wavering
};

struct ShotPoint {
  float pos[3];
  float time;
};
struct ShotTrail {
  int id;
  int kind;
  static constexpr int kPoints = 16;
  ShotPoint points[kPoints];
  int count = 0;
};
struct ShotSpark {
  float pos[3];
  float vel[3];
  float born, life;
  int kind;
};

struct ShotVertex {
  float pos[3];
  float side;
  float color[4];
};
static_assert(sizeof(ShotVertex) == 32);

std::vector<ShotTrail> g_trails;
std::vector<ShotSpark> g_sparks;
std::vector<ShotVertex> g_shot_vertices;
float g_shots_time = 0.f;

float rnd() {
  return (float)(std::rand() % 1000) / 1000.f;
}

void shot_seen(int id, int kind, const float* pos, float now) {
  ShotTrail* t = nullptr;
  for (auto& o : g_trails) {
    if (o.id == id && o.kind == kind) {
      t = &o;
      break;
    }
  }
  if (!t) {
    if (g_trails.size() >= 16) {
      g_trails.erase(g_trails.begin());  // (the oldest)
    }
    g_trails.emplace_back();
    t = &g_trails.back();
    t->id = id;
    t->kind = kind;
  }
  // (still: nothing to trail)
  if (t->count > 0) {
    const float* last = t->points[t->count - 1].pos;
    if (std::hypot(pos[0] - last[0], pos[1] - last[1], pos[2] - last[2]) < 0.02f * kMeter) {
      return;
    }
  }
  if (t->count == ShotTrail::kPoints) {
    std::memmove(t->points, t->points + 1, sizeof(ShotPoint) * (ShotTrail::kPoints - 1));
    t->count--;
  }
  auto& p = t->points[t->count++];
  std::memcpy(p.pos, pos, sizeof(p.pos));
  p.time = now;
}

void shot_hit(int kind, const float* pos, float now, bool mark) {
  if (mark) {
    static constexpr float kMarks[5] = {0.3f, 0.5f, 0.25f, 0.3f, 0.55f};
    enhanced_add_decal(pos, kMarks[kind] * kMeter, kind, now);
  }
  const int n = kind == 2 ? 6 : 9;
  for (int i = 0; i < n && g_sparks.size() < 64; i++) {
    ShotSpark s;
    std::memcpy(s.pos, pos, sizeof(s.pos));
    const float a = rnd() * 6.2832f, speed = (3.f + 6.f * rnd()) * kMeter;
    const float up = 0.2f + 0.8f * rnd();
    s.vel[0] = std::cos(a) * speed * (1.f - up * 0.6f);
    s.vel[1] = up * speed;
    s.vel[2] = std::sin(a) * speed * (1.f - up * 0.6f);
    s.born = now;
    s.life = 0.2f + 0.25f * rnd();
    s.kind = kind;
    g_sparks.push_back(s);
  }
}

// test switch: shots flying ahead of Jak, each gun in turn
void shot_test(float now) {
  static const bool test = std::getenv("JAK_ENHANCED_SHOT_TEST") != nullptr;
  const auto& pl = g_enhanced_player;
  if (!test || !pl.valid || !g_enhanced_camera.valid) {
    return;
  }
  static float start = -1e9f;
  static float from[3], dir[3];
  static int kind = 1, id = 1000000;
  if (now - start > 0.6f) {
    start = now;
    kind = kind % 4 + 1;
    id++;
    const float fx = g_enhanced_camera.rot[0][2], fz = g_enhanced_camera.rot[2][2];
    const float len = std::max(std::hypot(fx, fz), 0.01f);
    dir[0] = fx / len;
    dir[1] = -2.f / 35.f;  // (down to the ground in the half second)
    dir[2] = fz / len;
    from[0] = pl.pos[0];
    from[1] = pl.pos[1] + 2.f * kMeter;
    from[2] = pl.pos[2];
  }
  const float age = now - start;
  float p[3];
  for (int k = 0; k < 3; k++) {
    p[k] = from[k] + dir[k] * 70.f * kMeter * std::min(age, 0.5f);
  }
  static float hit_at = 0.f;
  if (age < 0.5f) {
    shot_seen(id, kind, p, now);
  } else if (hit_at != start) {
    hit_at = start;
    shot_hit(kind, p, now, true);
  }
}

// a polyline's camera-facing strip (fade: alpha at each point, width in game units)
void add_ribbon(const std::vector<std::array<float, 4>>& line,
                const float* color,
                float width,
                const float* cam) {
  for (size_t i = 0; i + 1 < line.size(); i++) {
    ShotVertex quad[4];
    for (int end = 0; end < 2; end++) {
      const auto& p = line[i + end];
      const auto& before = line[i + end == 0 ? 0 : i + end - 1];
      const auto& after = line[std::min(i + end + 1, line.size() - 1)];
      const float tx = after[0] - before[0], ty = after[1] - before[1], tz = after[2] - before[2];
      const float cx = cam[0] - p[0], cy = cam[1] - p[1], cz = cam[2] - p[2];
      float sx = ty * cz - tz * cy, sy = tz * cx - tx * cz, sz = tx * cy - ty * cx;
      const float len = std::max(std::hypot(sx, sy, sz), 1e-3f);
      const float w = width * (0.4f + 0.6f * p[3]) / len;
      sx *= w;
      sy *= w;
      sz *= w;
      for (int s = 0; s < 2; s++) {
        const float sign = s == 0 ? -1.f : 1.f;
        auto& v = quad[end * 2 + s];
        v.pos[0] = p[0] + sx * sign;
        v.pos[1] = p[1] + sy * sign;
        v.pos[2] = p[2] + sz * sign;
        v.side = sign;
        v.color[0] = color[0];
        v.color[1] = color[1];
        v.color[2] = color[2];
        v.color[3] = p[3];
      }
    }
    for (int idx : {0, 1, 2, 2, 1, 3}) {
      g_shot_vertices.push_back(quad[idx]);
    }
  }
}

void build_shot_vertices(float now, const float* cam, float detail) {
  g_shot_vertices.clear();
  std::vector<std::array<float, 4>> line;
  for (const auto& t : g_trails) {
    if (t.count < 2) {
      continue;
    }
    const auto& style = kShotStyles[t.kind];
    line.clear();
    float along = 0.f;
    for (int i = 0; i + 1 < t.count; i++) {
      const auto& a = t.points[i];
      const auto& b = t.points[i + 1];
      const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
      const float len = std::hypot(dx, dy, dz);
      // the blue crackles and the dark wavers: points in between, pushed sideways; the others stay
      // straight (the game's own shot draws over them)
      const float step = t.kind == 2 ? 0.8f * kMeter : t.kind == 4 ? 0.5f * kMeter : len + 1.f;
      const int parts = std::clamp((int)(len / step) + 1, 1, 48);
      for (int k = 0; k < parts; k++) {
        const float u = (float)k / parts;
        float p[4] = {a.pos[0] + dx * u, a.pos[1] + dy * u, a.pos[2] + dz * u, 0.f};
        const float time = a.time + (b.time - a.time) * u;
        p[3] = std::clamp(1.f - (now - time) / style.life, 0.f, 1.f);
        along += len / parts;
        if (t.kind == 2 && (i > 0 || k > 0)) {
          p[0] += (rnd() - 0.5f) * 0.3f * kMeter;
          p[1] += (rnd() - 0.5f) * 0.3f * kMeter;
          p[2] += (rnd() - 0.5f) * 0.3f * kMeter;
        } else if (t.kind == 4) {
          const float wave = std::sin(along / kMeter * 2.5f + now * 18.f) * 0.14f * kMeter;
          p[0] += wave;
          p[1] += std::cos(along / kMeter * 2.1f + now * 14.f) * 0.14f * kMeter;
          p[2] -= wave;
        }
        line.push_back({p[0], p[1], p[2], p[3] * detail});
      }
    }
    const auto& last = t.points[t.count - 1];
    line.push_back({last.pos[0], last.pos[1], last.pos[2],
                    std::clamp(1.f - (now - last.time) / style.life, 0.f, 1.f) * detail});
    add_ribbon(line, style.color, style.width * kMeter, cam);
  }
  for (const auto& s : g_sparks) {
    const float u = (now - s.born) / s.life;
    const float fade = std::clamp(1.f - u, 0.f, 1.f) * detail;
    line.clear();
    line.push_back({s.pos[0], s.pos[1], s.pos[2], fade});
    line.push_back({s.pos[0] - s.vel[0] * 0.025f, s.pos[1] - s.vel[1] * 0.025f,
                    s.pos[2] - s.vel[2] * 0.025f, fade * 0.3f});
    add_ribbon(line, kShotStyles[s.kind].color, 0.03f * kMeter, cam);
  }
}

}  // namespace

void enhanced_draw_shots(SharedRenderState* render_state,
                         const float* camera,
                         const float* hvdf_offset,
                         float pfog0) {
  const float now = g_enhanced_fx.time;
  const float dt = std::clamp(now - g_shots_time, 0.f, 0.1f);
  g_shots_time = now;
  const bool on = g_enhanced_fx.detail > 0.f;
  {
    std::lock_guard<std::mutex> lock(g_enhanced_shots.mutex);
    for (int i = 0; on && i < g_enhanced_shots.count; i++) {
      const auto& e = g_enhanced_shots.pending[i];
      if (e.kind < 1 || e.kind > 4) {
        continue;
      }
      if (e.id >= 0) {
        shot_seen(e.id, e.kind, e.pos, now);
      } else {
        shot_hit(e.kind, e.pos, now, e.id == -1);
      }
    }
    g_enhanced_shots.count = 0;
  }
  if (!on) {
    g_trails.clear();
    g_sparks.clear();
    return;
  }
  shot_test(now);
  // the old parts go; a shot's process can end, its trail just fades
  for (auto& t : g_trails) {
    const float life = kShotStyles[t.kind].life;
    int gone = 0;
    while (gone < t.count && now - t.points[gone].time > life) {
      gone++;
    }
    if (gone > 0) {
      std::memmove(t.points, t.points + gone, sizeof(ShotPoint) * (t.count - gone));
      t.count -= gone;
    }
  }
  std::erase_if(g_trails, [](const ShotTrail& t) { return t.count == 0; });
  std::erase_if(g_sparks, [&](const ShotSpark& s) { return now - s.born > s.life; });
  for (auto& s : g_sparks) {
    s.vel[1] -= 9.8f * 0.7f * kMeter * dt;
    for (int k = 0; k < 3; k++) {
      s.pos[k] += s.vel[k] * dt;
    }
  }
  if ((g_trails.empty() && g_sparks.empty()) || !g_enhanced_camera.valid) {
    return;
  }
  build_shot_vertices(now, g_enhanced_camera.trans, g_enhanced_fx.detail);
  if (g_shot_vertices.empty()) {
    return;
  }

  static std::unique_ptr<Shader> shader;
  static GLuint vao = 0, vbo = 0;
  if (!shader) {
    shader = std::make_unique<Shader>("enh_trail", render_state->version);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(ShotVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ShotVertex),
                          (void*)offsetof(ShotVertex, color));
  }
  shader->activate();
  const auto id = shader->id();
  glUniformMatrix4fv(glGetUniformLocation(id, "camera"), 1, GL_FALSE, camera);
  glUniform4fv(glGetUniformLocation(id, "hvdf_offset"), 1, hvdf_offset);
  glUniform1f(glGetUniformLocation(id, "pfog0"), pfog0);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, g_shot_vertices.size() * sizeof(ShotVertex), g_shot_vertices.data(),
               GL_STREAM_DRAW);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);
  glDepthMask(GL_FALSE);
  glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g_shot_vertices.size());
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  // (the HUD's sprites come next)
  render_state->shaders[ShaderId::SPRITE3].activate();
}
