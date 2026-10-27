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

bool enhanced_hd_screen_texture(const tfrag3::Texture& tex, std::vector<u32>& out, int& scale) {
  if (tex.w <= 0 || tex.h <= 0) {
    return false;
  }
  // only pages that have a folder (asked once per page)
  // made on this machine (out/, from the game's art) first, then the repo's own (game/assets: the
  // minimap icons redrawn with Tripo)
  static std::mutex dirs_mutex;
  static std::unordered_map<std::string, std::vector<fs::path>> dirs;
  std::vector<fs::path> roots;
  {
    std::lock_guard<std::mutex> lock(dirs_mutex);
    auto it = dirs.find(tex.debug_tpage_name);
    if (it == dirs.end()) {
      const auto game = version_to_game_name(g_game_version);
      std::vector<fs::path> found;
      for (const auto& root : {file_util::get_jak_project_dir() / "out" / "enhanced" / "hud",
                               file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" /
                                   "hud"}) {
        if (fs::is_directory(root / game / tex.debug_tpage_name)) {
          found.push_back(root / game / tex.debug_tpage_name);
        }
      }
      it = dirs.emplace(tex.debug_tpage_name, found).first;
    }
    roots = it->second;
  }
  fs::path path;
  for (const auto& dir : roots) {
    if (fs::exists(dir / (tex.debug_name + ".png"))) {
      path = dir / (tex.debug_name + ".png");
      break;
    }
  }
  if (path.empty()) {
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
    // see-through magenta (alpha 0): not redrawn, the game's own texel there, enlarged smoothly
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        u32& px = out[(size_t)y * w + x];
        if (px != 0x00ff00ffu) {
          continue;
        }
        const float fx = std::clamp((x + 0.5f) / scale - 0.5f, 0.f, tex.w - 1.f);
        const float fy = std::clamp((y + 0.5f) / scale - 0.5f, 0.f, tex.h - 1.f);
        const int x0 = (int)fx, y0 = (int)fy;
        const int x1 = std::min(x0 + 1, (int)tex.w - 1), y1 = std::min(y0 + 1, (int)tex.h - 1);
        const float tx = fx - x0, ty = fy - y0;
        u32 mixed = 0;
        for (int c = 0; c < 4; c++) {
          auto ch = [&](int sx, int sy) {
            return (float)((tex.data[(size_t)sy * tex.w + sx] >> (8 * c)) & 0xff);
          };
          const float v = (ch(x0, y0) * (1 - tx) + ch(x1, y0) * tx) * (1 - ty) +
                          (ch(x0, y1) * (1 - tx) + ch(x1, y1) * tx) * ty;
          mixed |= (u32)std::clamp((int)(v + 0.5f), 0, 255) << (8 * c);
        }
        px = mixed;
      }
    }
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
