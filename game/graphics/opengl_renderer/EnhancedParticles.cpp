#include "EnhancedParticles.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/graphics/texture/TexturePool.h"

EnhancedFx g_enhanced_fx;

namespace {

// (units past those the game's shaders bind for themselves at draw time)
constexpr int kNoiseUnit = 14;

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

void EnhancedSpriteFx::begin_flush(SharedRenderState* render_state, bool on) {
  const auto shader = render_state->shaders[ShaderId::SPRITE3].id();
  glUniform1i(glGetUniformLocation(shader, "enh_noise"), kNoiseUnit);
  m_class_loc = glGetUniformLocation(shader, "enh_fx_class");
  m_on = on && g_enhanced_fx.detail > 0.f;
  glUniform1i(m_class_loc, 0);
  if (!m_on) {
    return;
  }
  glActiveTexture(GL_TEXTURE0 + kNoiseUnit);
  glBindTexture(GL_TEXTURE_3D, noise_texture());
  glActiveTexture(GL_TEXTURE0);
  static const bool debug = std::getenv("JAK_ENHANCED_FX_DEBUG") != nullptr;
  glUniform1i(glGetUniformLocation(shader, "enh_fx_debug"), debug ? 1 : 0);
  glUniform2f(glGetUniformLocation(shader, "enh_fx"), g_enhanced_fx.time, g_enhanced_fx.detail);
}

void EnhancedSpriteFx::set_bucket(SharedRenderState* render_state,
                                  u32 tbp,
                                  u64 gl_texture,
                                  bool alpha_blended,
                                  bool additive) {
  if (m_on) {
    glUniform1i(m_class_loc, classify(render_state, tbp, gl_texture, alpha_blended, additive));
  }
}
