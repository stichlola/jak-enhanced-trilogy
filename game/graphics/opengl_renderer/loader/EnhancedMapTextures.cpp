#include "EnhancedMapTextures.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/runtime.h"

#include "third-party/stb_image/stb_image.h"

namespace {
std::mutex g_scale_mutex;
std::unordered_map<GLuint, float> g_texel_scale;

bool ends_with(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
}  // namespace

bool enhanced_upscale_map(const tfrag3::Texture& tex, std::vector<u32>& out) {
  if (!ends_with(tex.debug_tpage_name, "-minimap") || tex.debug_name.rfind("map-", 0) != 0 ||
      tex.w < 64 || tex.h < 64 || tex.data.size() != (size_t)tex.w * tex.h) {
    return false;
  }
  // only the flat 4-bit maps (the guard's view cone and such are gradients)
  std::vector<u32> palette;
  for (u32 c : tex.data) {
    if (std::find(palette.begin(), palette.end(), c) == palette.end()) {
      palette.push_back(c);
      if (palette.size() > 16) {
        return false;
      }
    }
  }
  const int w = tex.w, h = tex.h;
  constexpr int S = kEnhancedMapScale;
  auto at = [&](int x, int y) {
    return tex.data[std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)];
  };
  // how much of color c is around texel (x, y): a 3x3 blur of its mask
  auto cover = [&](u32 c, int x, int y) {
    static constexpr float k[3] = {1.f, 2.f, 1.f};
    float sum = 0.f;
    for (int dy = -1; dy <= 1; dy++) {
      for (int dx = -1; dx <= 1; dx++) {
        if (at(x + dx, y + dy) == c) {
          sum += k[dx + 1] * k[dy + 1];
        }
      }
    }
    return sum;
  };
  out.resize((size_t)w * S * h * S);
  u32 cands[16];
  for (int oy = 0; oy < h * S; oy++) {
    // the output pixel's center in source texels, and its 2x2 bilinear taps
    const float fy = (oy + 0.5f) / S - 0.5f;
    const int y0 = (int)std::floor(fy);
    const float ty = fy - y0;
    for (int ox = 0; ox < w * S; ox++) {
      const float fx = (ox + 0.5f) / S - 0.5f;
      const int x0 = (int)std::floor(fx);
      const float tx = fx - x0;
      // the colors that can reach here (the taps and their blur)
      int n = 0;
      for (int dy = -1; dy <= 2; dy++) {
        for (int dx = -1; dx <= 2; dx++) {
          const u32 c = at(x0 + dx, y0 + dy);
          if (std::find(cands, cands + n, c) == cands + n) {
            cands[n++] = c;
          }
        }
      }
      u32 best = cands[0];
      if (n > 1) {
        // the color that covers the most of this spot wins: smooth edges at 4x, still flat colors
        float best_w = -1.f;
        for (int i = 0; i < n; i++) {
          const float v = (1 - tx) * (1 - ty) * cover(cands[i], x0, y0) +
                          tx * (1 - ty) * cover(cands[i], x0 + 1, y0) +
                          (1 - tx) * ty * cover(cands[i], x0, y0 + 1) +
                          tx * ty * cover(cands[i], x0 + 1, y0 + 1);
          if (v > best_w) {
            best_w = v;
            best = cands[i];
          }
        }
      }
      out[(size_t)oy * w * S + ox] = best;
    }
  }
  return true;
}

bool enhanced_hd_minimap_texture(const tfrag3::Texture& tex, std::vector<u32>& out, int& scale) {
  if (!ends_with(tex.debug_tpage_name, "-minimap") || tex.w <= 0 || tex.h <= 0) {
    return false;
  }
  const auto path = file_util::get_jak_project_dir() / "out" / "enhanced" / "minimap" /
                    version_to_game_name(g_game_version) / tex.debug_tpage_name /
                    (tex.debug_name + ".png");
  if (!fs::exists(path)) {
    return false;
  }
  int w, h;
  u8* data = stbi_load(path.string().c_str(), &w, &h, nullptr, 4);
  if (!data) {
    return false;
  }
  scale = w / tex.w;
  const bool ok = scale >= 1 && w == tex.w * scale && h == tex.h * scale;
  if (ok) {
    out.resize((size_t)w * h);
    std::memcpy(out.data(), data, out.size() * 4);
    lg::info("[enhanced] {} {}: HD x{}", tex.debug_tpage_name, tex.debug_name, scale);
  } else {
    lg::error("[enhanced] {}: {}x{} is not a multiple of {}x{}", path.string(), w, h, tex.w, tex.h);
  }
  stbi_image_free(data);
  return ok;
}

void enhanced_set_texel_scale(GLuint tex, float scale) {
  std::lock_guard<std::mutex> lock(g_scale_mutex);
  g_texel_scale[tex] = scale;
}

void enhanced_forget_texture(GLuint tex) {
  std::lock_guard<std::mutex> lock(g_scale_mutex);
  g_texel_scale.erase(tex);
}

float enhanced_texel_scale(GLuint tex) {
  std::lock_guard<std::mutex> lock(g_scale_mutex);
  auto it = g_texel_scale.find(tex);
  return it == g_texel_scale.end() ? 1.f : it->second;
}
