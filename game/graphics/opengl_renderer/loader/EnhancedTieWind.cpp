#include "EnhancedTieWind.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <set>
#include <unordered_map>

#include "common/log/log.h"

#include "fmt/format.h"

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
  // the trunk proper stays nearly still; a tree's or palm's other solid parts (a palm's crown and
  // frond stems: des-palm-top) swing with the leaves they hold, or the leaves tear off them
  const bool trunk = has_any(name, {"trunk", "bark", "root", "stump"});
  const bool tree_part = has_any(name, {"tree", "palm"});
  if (trunk) {
    return kTrunk;
  }
  if (!plant) {
    return kNone;
  }
  bool holes = false;
  for (u32 px : tex.data) {
    if ((px >> 24) < 0x40) {
      holes = true;
      break;
    }
  }
  return holes || tree_part ? kLeaf : kNone;
}

u64 cell_key(int x, int z) {
  return ((u64)(u32)x << 32) | (u32)z;
}

u64 cell_key3(int x, int y, int z) {
  return ((u64)(x & 0x1fffff) << 42) | ((u64)(y & 0x1fffff) << 21) | (u64)(z & 0x1fffff);
}

// the trunks' points (one per 0.5 m), hashed in 2 m cells: how far a leaf is from its plant's trunk
struct TrunkPoints {
  static constexpr float kFine = 0.5f * kMeter;
  static constexpr float kCoarse = 2.f * kMeter;
  std::unordered_map<u64, std::vector<math::Vector3f>> cells;

  void add(const tfrag3::PreloadedVertex& v, std::set<u64>& fine) {
    if (!fine.insert(cell_key3((int)std::floor(v.x / kFine), (int)std::floor(v.y / kFine),
                               (int)std::floor(v.z / kFine)))
             .second) {
      return;
    }
    cells[cell_key3((int)std::floor(v.x / kCoarse), (int)std::floor(v.y / kCoarse),
                    (int)std::floor(v.z / kCoarse))]
        .push_back(math::Vector3f(v.x, v.y, v.z));
  }

  // meters to the nearest trunk point, up to ~6 m (INFINITY: none)
  float nearest(const tfrag3::PreloadedVertex& v) const {
    if (cells.empty()) {
      return INFINITY;
    }
    const int cx = (int)std::floor(v.x / kCoarse), cy = (int)std::floor(v.y / kCoarse),
              cz = (int)std::floor(v.z / kCoarse);
    float best = INFINITY;
    // growing shells: once the best is within the shell's sure radius, nothing farther can win
    for (int r = 1; r <= 3; r++) {
      for (int dx = -r; dx <= r; dx++) {
        for (int dy = -r; dy <= r; dy++) {
          for (int dz = -r; dz <= r; dz++) {
            if (std::max(std::abs(dx), std::max(std::abs(dy), std::abs(dz))) != r && r > 1) {
              continue;  // (inner cells already seen)
            }
            auto it = cells.find(cell_key3(cx + dx, cy + dy, cz + dz));
            if (it == cells.end()) {
              continue;
            }
            for (const auto& p : it->second) {
              const float ex = p.x() - v.x, ey = p.y() - v.y, ez = p.z() - v.z;
              best = std::min(best, ex * ex + ey * ey + ez * ez);
            }
          }
        }
      }
      if (best <= (float)(r * r) * kCoarse * kCoarse) {
        break;
      }
    }
    return std::sqrt(best) / kMeter;
  }
};

}  // namespace

bool enhanced_tie_wind_is_leaf(const tfrag3::Texture& tex) {
  if (texture_kind(tex) == kLeaf) {
    return true;
  }
  // a wind tie's leaves may be solid cards (Spargus' palms: wascity-palm-leaf-worn)
  std::string name = tex.debug_name;
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return texture_kind(tex) != kNone && has_any(name, {"leaf", "leaves", "frond", "foliage"}) &&
         !has_any(name, {"trunk", "bark", "root"});
}

void enhanced_tie_wind_weights(tfrag3::Level& level, const std::string& name) {
  static const bool log_names = std::getenv("JAK_ENHANCED_TIE_WIND_LOG") != nullptr;
  std::vector<u8> kinds(level.textures.size(), kNone);
  for (size_t i = 0; i < level.textures.size(); i++) {
    kinds[i] = texture_kind(level.textures[i]);
  }

  int draws[3] = {}, verts[3] = {};
  double reach_sum = 0.;
  int tfrag_trees = 0;
  const auto t0 = std::chrono::steady_clock::now();
  std::set<std::string> logged;
  // every swaying vertex's cell, lowest y: the wind ties' feet (their crowns lean with the trunks)
  std::unordered_map<u64, float> level_low;
  // one tree of static geometry (TIE, or tfrag: Spargus' palms and Haven's trees are tfrag)
  auto weigh = [&](std::vector<tfrag3::PreloadedVertex>& vs, const std::vector<u32>& is,
                   const std::vector<tfrag3::StripDraw>& sd, const char* what) {
      // each vertex's kind (a leaf wins over a trunk), from the draws that use it
      std::vector<u8> vkind(vs.size(), kNone);
      bool any = false;
      for (size_t d = 0; d < sd.size(); d++) {
        const int tex = sd[d].tree_tex_id;
        const Kind k = tex >= 0 && tex < (int)kinds.size() ? (Kind)kinds[tex] : kNone;
        if (log_names && tex >= 0 && tex < (int)level.textures.size() &&
            (k != kNone || sd[d].mode.get_at_enable())) {
          const auto& tn = level.textures[tex].debug_name;
          if (logged.insert(tn).second) {
            // (with a spot to go and look at it: the draw's first vertex, in meters)
            const size_t fi = sd[d].unpacked.idx_of_first_idx_in_full_buffer;
            const u32 v0 = fi < is.size() && is[fi] < vs.size() ? is[fi] : 0;
            lg::info("[enhanced] tie wind {}: {} {} kind {} at {:.0f} {:.0f} {:.0f}", name, what, tn, (int)k,
                     vs.empty() ? 0.f : vs[v0].x / kMeter, vs.empty() ? 0.f : vs[v0].y / kMeter,
                     vs.empty() ? 0.f : vs[v0].z / kMeter);
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
        return;
      }
      if (what[1] == 'f') {
        tfrag_trees++;
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
      for (const auto& [key, y] : low) {
        auto it = level_low.find(key);
        if (it == level_low.end()) {
          level_low.emplace(key, y);
        } else {
          it->second = std::min(it->second, y);
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
      TrunkPoints trunks;
      {
        std::set<u64> fine;
        for (size_t v = 0; v < vs.size(); v++) {
          if (vkind[v] == kTrunk) {
            trunks.add(vs[v], fine);
          }
        }
      }
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
        if (vkind[v] != kLeaf) {
          vs[v].enh_sway = (u16)std::lround(h * 32767.f);
          continue;
        }
        // a leaf: its height (dm) and its reach, from the trunk or, with none near, from the ground
        const float h_m = h * kEnhTieSwayMaxM;
        // (with no trunk: a bush, or a planter's or a wall's hanging leaves, whose foot is their
        // lowest tip: at most kBushReachM, so they don't swing like palm fronds)
        constexpr float kBushReachM = 1.5f;
        const float trunk = trunks.nearest(vs[v]);
        const float reach = std::isfinite(trunk) ? std::min(trunk, h_m) : std::min(h_m, kBushReachM);
        const int hd = std::clamp((int)std::lround(h_m * 10.f), 0, 127);
        const int rq = std::clamp((int)std::lround(reach / kEnhTieReachStepM), 0, 255);
        reach_sum += reach;
        vs[v].enh_sway = (u16)(0x8000 | (hd << 8) | rq);
      }
  };
  for (auto& geo : level.tie_trees) {
    for (auto& tree : geo) {
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
        const auto& m0 = tree.wind_instance_info[0].matrix[3];
        lg::info("[enhanced] tie wind {}: game wind {} instances (first at {:.0f} {:.0f} {:.0f}), {} draws: {}",
                 name, tree.wind_instance_info.size(), m0.x() / kMeter, m0.y() / kMeter,
                 m0.z() / kMeter, tree.instanced_wind_draws.size(), all);
      }
      weigh(tree.unpacked.vertices, tree.unpacked.indices, tree.static_draws, "tie");
    }
  }
  for (auto& geo : level.tfrag_trees) {
    for (auto& tree : geo) {
      weigh(tree.unpacked.vertices, tree.unpacked.indices, tree.draws, "tfrag");
    }
  }
  // the wind ties' origins over the same foot field (blended as the static vertices' bases), so a
  // crown sitting on a static trunk (Spargus' palms) gets that trunk top's lean (Tie3.cpp)
  {
    auto foot_at = [&](int cx, int cz) {
      float f = INFINITY;
      for (int dx = -kFootReach; dx <= kFootReach; dx++) {
        for (int dz = -kFootReach; dz <= kFootReach; dz++) {
          auto l = level_low.find(cell_key(cx + dx, cz + dz));
          if (l != level_low.end()) {
            f = std::min(f, l->second);
          }
        }
      }
      return f;
    };
    int raised = 0, all_inst = 0;
    std::unordered_map<const tfrag3::TieWindInstance*, float> foot_y;  // (for the spots' file)
    double h_sum = 0.;
    for (auto& geo : level.tie_trees) {
      for (auto& tree : geo) {
        for (auto& wi : tree.wind_instance_info) {
          all_inst++;
          wi.enh_lean_h = 0.f;
          const float x = wi.matrix[3].x(), y = wi.matrix[3].y(), z = wi.matrix[3].z();
          const float fx = x / kCell - 0.5f, fz = z / kCell - 0.5f;
          const int x0 = (int)std::floor(fx), z0 = (int)std::floor(fz);
          const float tx = fx - x0, tz = fz - z0;
          float f00 = foot_at(x0, z0), f10 = foot_at(x0 + 1, z0);
          float f01 = foot_at(x0, z0 + 1), f11 = foot_at(x0 + 1, z0 + 1);
          const float any_f = std::min(std::min(f00, f10), std::min(f01, f11));
          if (!std::isfinite(any_f)) {
            continue;
          }
          for (float* f : {&f00, &f10, &f01, &f11}) {
            if (!std::isfinite(*f)) {
              *f = any_f;
            }
          }
          const float base =
              (f00 * (1 - tx) + f10 * tx) * (1 - tz) + (f01 * (1 - tx) + f11 * tx) * tz;
          wi.enh_lean_h = std::clamp((y - base) / kMeter, 0.f, kEnhTieSwayMaxM);
          foot_y[&wi] = base / kMeter;
          if (wi.enh_lean_h > 0.5f) {
            raised++;
            h_sum += wi.enh_lean_h;
          }
        }
      }
    }
    if (log_names) {
      // (spots to go and look: the lowest few origins of each wind tree, with their height)
      for (const auto& geo : level.tie_trees) {
        for (const auto& tree : geo) {
          std::vector<const tfrag3::TieWindInstance*> by_y;
          for (const auto& wi : tree.wind_instance_info) {
            by_y.push_back(&wi);
          }
          std::sort(by_y.begin(), by_y.end(), [](auto a, auto b) {
            return a->matrix[3].y() < b->matrix[3].y();
          });
          std::string spots;
          for (size_t i = 0; i < by_y.size() && i < 10; i++) {
            const auto& m = by_y[i]->matrix[3];
            spots += fmt::format("{:.0f},{:.0f},{:.0f} h{:.1f}; ", m.x() / kMeter, m.y() / kMeter,
                                 m.z() / kMeter, by_y[i]->enh_lean_h);
          }
          if (!spots.empty()) {
            lg::info("[enhanced] tie wind {}: wind-tie spots: {}", name, spots);
          }
          // (and every origin to log/tie_wind_<level>.csv: x, y, z, height over the foot, the
          // foot's y, in m)
          if (!by_y.empty() && &geo == &level.tie_trees[0]) {
            if (FILE* f = std::fopen(fmt::format("log/tie_wind_{}.csv", name).c_str(), "a")) {
              for (const auto* wi : by_y) {
                const auto& m = wi->matrix[3];
                auto fy = foot_y.find(wi);
                std::fprintf(f, "%.1f,%.1f,%.1f,%.1f,%.1f\n", m.x() / kMeter, m.y() / kMeter,
                             m.z() / kMeter, wi->enh_lean_h,
                             fy != foot_y.end() ? fy->second : -999.f);
              }
              std::fclose(f);
            }
          }
        }
      }
    }
    if (log_names && all_inst > 0) {
      lg::info("[enhanced] tie wind {}: {} of {} wind-tie instances sit {:.1f} m (mean) over a "
               "static plant's foot (their origin leans with it)",
               name, raised, all_inst, raised ? h_sum / raised : 0.);
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
    lg::info(
        "[enhanced] tie wind {}: {} leaf draws ({} verts, mean reach {:.2f} m), {} trunk draws ({} "
        "verts), {} of them tfrag trees, {:.0f} ms",
        name, draws[kLeaf], verts[kLeaf], verts[kLeaf] ? reach_sum / verts[kLeaf] : 0.,
        draws[kTrunk], verts[kTrunk], tfrag_trees,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
}
