#include "EnhancedTieWind.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <unordered_map>

#include "common/log/log.h"

namespace {

constexpr float kMeter = 4096.f;
constexpr float kCell = 2.f * kMeter;  // the foot field's cells
constexpr int kFootReach = 3;          // a foot is the lowest point within 3 cells (6-7 m)

enum Kind : u8 { kNone = 0, kTrunk = 1, kLeaf = 2 };

bool has_any(const std::string& name, std::initializer_list<const char*> words) {
  for (auto w : words) {
    if (name.find(w) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// what a texture is to the wind. Leaves: see-through cards with a plant's name (not a fence, a
// grille, a net, a sign...). Trunks: a tree's or palm's solid parts, so they bend with their leaves
Kind texture_kind(const tfrag3::Texture& tex) {
  std::string name = tex.debug_name;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (has_any(name, {"fence", "grate", "grill", "rail", "chain", "wire", "net", "sign", "window",
                     "glass", "metal", "rope", "cage", "bars", "web", "lamp", "light", "flag",
                     "banner", "cloth", "wall", "roof", "brick", "stone", "rock", "water", "sky"})) {
    return kNone;
  }
  const bool plant = has_any(name, {"leaf", "leaves", "tree", "palm", "fern", "bush", "frond",
                                    "branch", "foliage", "vine", "plant", "ivy", "hedge",
                                    "shrub", "flower", "weed", "reed", "bamboo", "twig"});
  const bool trunk = has_any(name, {"trunk", "bark", "tree", "palm"});
  if (!plant && !trunk) {
    return kNone;
  }
  bool holes = false;
  for (u32 px : tex.data) {
    if ((px >> 24) < 0x40) {
      holes = true;
      break;
    }
  }
  if (plant && holes) {
    return kLeaf;
  }
  return trunk ? kTrunk : kNone;
}

u64 cell_key(int x, int z) {
  return ((u64)(u32)x << 32) | (u32)z;
}

}  // namespace

void enhanced_tie_wind_weights(tfrag3::Level& level, const std::string& name) {
  static const bool log_names = std::getenv("JAK_ENHANCED_TIE_WIND_LOG") != nullptr;
  std::vector<u8> kinds(level.textures.size(), kNone);
  for (size_t i = 0; i < level.textures.size(); i++) {
    kinds[i] = texture_kind(level.textures[i]);
  }

  int draws[3] = {}, verts[3] = {};
  std::set<std::string> logged;
  for (auto& geo : level.tie_trees) {
    for (auto& tree : geo) {
      auto& vs = tree.unpacked.vertices;
      const auto& is = tree.unpacked.indices;
      const auto& sd = tree.static_draws;
      // each vertex's kind (a leaf wins over a trunk), from the draws that use it
      std::vector<u8> vkind(vs.size(), kNone);
      bool any = false;
      if (log_names && !tree.wind_instance_info.empty()) {
        std::set<std::string> wind_tex;
        for (const auto& wd : tree.instanced_wind_draws) {
          if (wd.tree_tex_id >= 0 && wd.tree_tex_id < (int)level.textures.size()) {
            wind_tex.insert(level.textures[wd.tree_tex_id].debug_name);
          }
        }
        std::string all;
        for (const auto& t : wind_tex) {
          all += t + " ";
        }
        lg::info("[enhanced] tie wind {}: game wind {} instances, {} draws: {}", name,
                 tree.wind_instance_info.size(), tree.instanced_wind_draws.size(), all);
      }
      for (size_t d = 0; d < sd.size(); d++) {
        const int tex = sd[d].tree_tex_id;
        const Kind k = tex >= 0 && tex < (int)kinds.size() ? (Kind)kinds[tex] : kNone;
        if (log_names && tex >= 0 && tex < (int)level.textures.size() &&
            (k != kNone || sd[d].mode.get_at_enable())) {
          const auto& tn = level.textures[tex].debug_name;
          if (logged.insert(tn).second) {
            lg::info("[enhanced] tie wind {}: {} kind {}", name, tn, (int)k);
          }
        }
        if (k == kNone) {
          continue;
        }
        draws[k]++;
        any = true;
        const size_t first = sd[d].unpacked.idx_of_first_idx_in_full_buffer;
        const size_t end = d + 1 < sd.size() ? sd[d + 1].unpacked.idx_of_first_idx_in_full_buffer
                                             : is.size();
        for (size_t i = first; i < end && i < is.size(); i++) {
          const u32 vi = is[i];
          if (vi < vs.size()) {
            vkind[vi] = std::max(vkind[vi], (u8)k);
          }
        }
      }
      if (!any) {
        continue;
      }
      // the lowest swaying vertex of each cell, then of each cell's neighbourhood: the plants' feet
      std::unordered_map<u64, float> low;
      for (size_t v = 0; v < vs.size(); v++) {
        if (vkind[v] == kNone) {
          continue;
        }
        const u64 key = cell_key((int)std::floor(vs[v].x / kCell), (int)std::floor(vs[v].z / kCell));
        auto it = low.find(key);
        if (it == low.end()) {
          low.emplace(key, vs[v].y);
        } else {
          it->second = std::min(it->second, vs[v].y);
        }
      }
      std::unordered_map<u64, float> foot;
      auto foot_at = [&](int cx, int cz) {
        const u64 key = cell_key(cx, cz);
        auto it = foot.find(key);
        if (it != foot.end()) {
          return it->second;
        }
        float f = INFINITY;
        for (int dx = -kFootReach; dx <= kFootReach; dx++) {
          for (int dz = -kFootReach; dz <= kFootReach; dz++) {
            auto l = low.find(cell_key(cx + dx, cz + dz));
            if (l != low.end()) {
              f = std::min(f, l->second);
            }
          }
        }
        foot.emplace(key, f);
        return f;
      };
      for (size_t v = 0; v < vs.size(); v++) {
        if (vkind[v] == kNone) {
          continue;
        }
        verts[vkind[v]]++;
        // blended between the four nearest cells' centres: a smooth field, so nothing tears
        const float fx = vs[v].x / kCell - 0.5f, fz = vs[v].z / kCell - 0.5f;
        const int x0 = (int)std::floor(fx), z0 = (int)std::floor(fz);
        const float tx = fx - x0, tz = fz - z0;
        float f00 = foot_at(x0, z0), f10 = foot_at(x0 + 1, z0);
        float f01 = foot_at(x0, z0 + 1), f11 = foot_at(x0 + 1, z0 + 1);
        // (a corner with nothing near: its neighbours' value)
        const float any_f = std::min(std::min(f00, f10), std::min(f01, f11));
        if (!std::isfinite(any_f)) {
          continue;
        }
        for (float* f : {&f00, &f10, &f01, &f11}) {
          if (!std::isfinite(*f)) {
            *f = any_f;
          }
        }
        const float base = (f00 * (1 - tx) + f10 * tx) * (1 - tz) + (f01 * (1 - tx) + f11 * tx) * tz;
        const float h = std::clamp((vs[v].y - base) / (kEnhTieSwayMaxM * kMeter), 0.f, 1.f);
        vs[v].enh_sway = (u16)std::lround(h * 32767.f) | (vkind[v] == kLeaf ? 0x8000 : 0);
      }
    }
  }
  if (log_names) {
    std::set<std::string> shrub_tex;
    for (const auto& st : level.shrub_trees) {
      for (const auto& d : st.static_draws) {
        if (d.tree_tex_id >= 0 && d.tree_tex_id < (int)level.textures.size()) {
          shrub_tex.insert(level.textures[d.tree_tex_id].debug_name);
        }
      }
    }
    std::string all;
    for (const auto& t : shrub_tex) {
      all += t + " ";
    }
    lg::info("[enhanced] tie wind {}: shrubs: {}", name, all);
  }
  if (draws[kTrunk] + draws[kLeaf] > 0) {
    lg::info("[enhanced] tie wind {}: {} leaf draws ({} verts), {} trunk draws ({} verts)", name,
             draws[kLeaf], verts[kLeaf], draws[kTrunk], verts[kTrunk]);
  }
}
