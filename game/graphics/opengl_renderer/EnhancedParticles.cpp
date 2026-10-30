#include "EnhancedParticles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/graphics/texture/TexturePool.h"

EnhancedFx g_enhanced_fx;

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
