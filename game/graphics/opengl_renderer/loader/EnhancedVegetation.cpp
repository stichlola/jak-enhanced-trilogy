#include "EnhancedVegetation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "common/log/log.h"

int g_enhanced_vegetation = [] {
  const char* env = std::getenv("JAK_ENHANCED_VEGETATION");
  return env ? std::clamp(std::atoi(env), 0, 2) : 2;
}();

namespace {

constexpr float kMeter = 4096.f;

using Vec3 = math::Vector3f;

/*!
 * The level's floor: its upward-facing triangles (tfrag and tie, most detailed), bucketed on a 2D
 * grid, for "how high is the ground here".
 */
class Ground {
 public:
  void add(const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 n = (b - a).cross(c - a);
    float len = n.length();
    if (!(len > 1.f) || std::abs(n.y()) / len < 0.7f) {
      return;  // degenerate, or a wall / steep slope
    }
    u32 id = m_tris.size() / 3;
    m_tris.push_back(a);
    m_tris.push_back(b);
    m_tris.push_back(c);
    int x0 = cell(std::min({a.x(), b.x(), c.x()})), x1 = cell(std::max({a.x(), b.x(), c.x()}));
    int z0 = cell(std::min({a.z(), b.z(), c.z()})), z1 = cell(std::max({a.z(), b.z(), c.z()}));
    if ((x1 - x0) * (z1 - z0) > 400) {
      return;  // huge triangle (sky, far terrain): not a floor plants stand on
    }
    for (int x = x0; x <= x1; x++) {
      for (int z = z0; z <= z1; z++) {
        m_cells[key(x, z)].push_back(id);
      }
    }
  }

  template <typename Verts, typename GetPos>
  void add_mesh(const Verts& verts, const std::vector<u32>& indices, bool strips, GetPos pos) {
    auto tri = [&](u32 a, u32 b, u32 c) {
      if (a < verts.size() && b < verts.size() && c < verts.size() && a != b && b != c && a != c) {
        add(pos(verts[a]), pos(verts[b]), pos(verts[c]));
      }
    };
    if (strips) {
      size_t start = 0;
      for (size_t i = 0; i < indices.size(); i++) {
        if (indices[i] == UINT32_MAX) {
          start = i + 1;
        } else if (i >= start + 2) {
          tri(indices[i - 2], indices[i - 1], indices[i]);
        }
      }
    } else {
      for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        tri(indices[i], indices[i + 1], indices[i + 2]);
      }
    }
  }

  // the floor height at (x, z) closest to y_ref within [y_ref - below, y_ref + above]
  bool height(float x, float z, float y_ref, float below, float above, float* out) const {
    auto it = m_cells.find(key(cell(x), cell(z)));
    if (it == m_cells.end()) {
      return false;
    }
    bool found = false;
    float best = 0;
    for (u32 id : it->second) {
      const Vec3& a = m_tris[id * 3];
      const Vec3& b = m_tris[id * 3 + 1];
      const Vec3& c = m_tris[id * 3 + 2];
      // barycentric in the xz plane
      float d = (b.z() - c.z()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.z() - c.z());
      if (std::abs(d) < 1e-3f) {
        continue;
      }
      float l0 = ((b.z() - c.z()) * (x - c.x()) + (c.x() - b.x()) * (z - c.z())) / d;
      float l1 = ((c.z() - a.z()) * (x - c.x()) + (a.x() - c.x()) * (z - c.z())) / d;
      float l2 = 1.f - l0 - l1;
      if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) {
        continue;
      }
      float y = l0 * a.y() + l1 * b.y() + l2 * c.y();
      if (y < y_ref - below || y > y_ref + above) {
        continue;
      }
      if (!found || std::abs(y - y_ref) < std::abs(best - y_ref)) {
        best = y;
        found = true;
      }
    }
    if (found) {
      *out = best;
    }
    return found;
  }

  size_t size() const { return m_tris.size() / 3; }

 private:
  static constexpr float kCell = 4.f * kMeter;
  static int cell(float v) { return (int)std::floor(v / kCell); }
  static u64 key(int x, int z) { return ((u64)(u32)x << 32) | (u32)z; }
  std::vector<Vec3> m_tris;
  std::unordered_map<u64, std::vector<u32>> m_cells;
};

// deterministic random in [0, 1)
float rnd(u32 a, u32 b, u32 c) {
  u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return (h >> 8) * (1.f / 16777216.f);
}

// 10:10:10 signed normal (pack_to_gl_normal), turned around the vertical axis
u32 rotate_normal(u32 nor, float c, float s) {
  auto get = [&](int shift) {
    int v = (nor >> shift) & 0x3ff;
    return v >= 512 ? v - 1024 : v;
  };
  float x = get(0), y = get(10), z = get(20);
  float rx = c * x + s * z;
  float rz = -s * x + c * z;
  auto put = [](float v) { return (u32)(std::clamp((int)std::lround(v), -511, 511) & 0x3ff); };
  return put(rx) | (put(y) << 10) | (put(rz) << 20);
}

struct Plant {
  u32 first_vert = 0, end_vert = 0;
  float cx = 0, cz = 0;  // middle
  float base = 0;        // floor height under it
  float radius = 0;      // horizontal
  bool ok = true;        // only in cutout draws
  bool seen = false;
};

void add_to_tree(tfrag3::ShrubTree& tree, const Ground& ground, u32 tree_idx, int* plants_out,
                 int* copies_out) {
  auto& verts = tree.unpacked.vertices;
  const auto& groups = tree.packed_vertices.instance_groups;

  // which instance each unpacked vertex belongs to (unpack() goes through the groups in order)
  std::vector<Plant> plants(groups.size());
  std::vector<u32> plant_of(verts.size(), UINT32_MAX);
  {
    u32 i = 0;
    for (u32 g = 0; g < groups.size(); g++) {
      u32 n = groups[g].end_vert - groups[g].start_vert;
      plants[g].first_vert = i;
      plants[g].end_vert = i + n;
      for (u32 k = 0; k < n && i < verts.size(); k++) {
        plant_of[i++] = g;
      }
    }
  }

  // a plant shows up only in cutout (alpha tested) draws
  for (const auto& draw : tree.static_draws) {
    bool cutout = draw.mode.get_at_enable();
    for (u32 k = draw.first_index_index; k < draw.first_index_index + draw.num_indices; k++) {
      u32 v = tree.indices[k];
      if (v != UINT32_MAX && v < verts.size() && plant_of[v] != UINT32_MAX) {
        auto& p = plants[plant_of[v]];
        p.seen = true;
        if (!cutout) {
          p.ok = false;
        }
      }
    }
  }

  // small, standing on the floor, made of crossed cards (not a flat fence or sign)
  for (auto& p : plants) {
    if (!p.seen || !p.ok || p.end_vert - p.first_vert < 4) {
      p.ok = false;
      continue;
    }
    float min_y = 1e30f, max_y = -1e30f, sx = 0, sz = 0;
    for (u32 v = p.first_vert; v < p.end_vert; v++) {
      min_y = std::min(min_y, verts[v].y);
      max_y = std::max(max_y, verts[v].y);
      sx += verts[v].x;
      sz += verts[v].z;
    }
    float n = p.end_vert - p.first_vert;
    p.cx = sx / n;
    p.cz = sz / n;
    float r = 0;
    for (u32 v = p.first_vert; v < p.end_vert; v++) {
      r = std::max(r, std::hypot(verts[v].x - p.cx, verts[v].z - p.cz));
    }
    p.radius = r;
    bool crossed = false;
    u32 n0 = verts[p.first_vert].pad0;
    auto unit = [](u32 nor) {
      auto get = [&](int shift) {
        int v = (nor >> shift) & 0x3ff;
        return (float)(v >= 512 ? v - 1024 : v);
      };
      Vec3 u(get(0), get(10), get(20));
      float l = u.length();
      return l > 0 ? u / l : u;
    };
    Vec3 first = unit(n0);
    for (u32 v = p.first_vert; v < p.end_vert && !crossed; v++) {
      if (std::abs(unit(verts[v].pad0).dot(first)) < 0.7f) {
        crossed = true;
      }
    }
    float height = max_y - min_y;
    p.ok = crossed && height < 1.6f * kMeter && height > 0.05f * kMeter && r < 2.f * kMeter &&
           r > 0.08f * kMeter &&
           ground.height(p.cx, p.cz, min_y, 0.4f * kMeter, 0.4f * kMeter, &p.base);
  }

  // copies: tier 1 one per plant, tier 2 two more
  struct Copy {
    u32 plant;
    u32 vert_offset;  // where its vertices start in the appended block
  };
  std::vector<std::vector<Copy>> copies_per_tier(3);
  int plant_count = 0;
  for (u32 g = 0; g < plants.size(); g++) {
    const auto& p = plants[g];
    if (!p.ok) {
      continue;
    }
    plant_count++;
    for (u32 k = 0; k < 3; k++) {
      int tier = k == 0 ? 1 : 2;
      float ang = 6.2831853f * (rnd(tree_idx, g, k * 7 + 1) + k / 3.f);
      float dist = p.radius * (1.1f + 1.2f * rnd(tree_idx, g, k * 7 + 2)) + 0.25f * kMeter;
      float x = p.cx + std::cos(ang) * dist;
      float z = p.cz + std::sin(ang) * dist;
      float y, y_mid;
      if (!ground.height(x, z, p.base, 0.6f * kMeter, 0.6f * kMeter, &y) ||
          !ground.height((x + p.cx) * 0.5f, (z + p.cz) * 0.5f, p.base, 0.4f * kMeter,
                         0.4f * kMeter, &y_mid)) {
        continue;  // no floor there, or not the same floor (an edge, water, a step, a wall)
      }
      float yaw = 6.2831853f * rnd(tree_idx, g, k * 7 + 3);
      float scale = 0.75f + 0.45f * rnd(tree_idx, g, k * 7 + 4);
      float c = std::cos(yaw), s = std::sin(yaw);
      u32 offset = verts.size();
      for (u32 v = p.first_vert; v < p.end_vert; v++) {
        auto out = verts[v];
        float dx = (verts[v].x - p.cx) * scale, dz = (verts[v].z - p.cz) * scale;
        out.x = x + c * dx + s * dz;
        out.z = z - s * dx + c * dz;
        // sunk a little, so a copy on a gentle slope doesn't float
        out.y = y + (verts[v].y - p.base) * scale - 0.03f * kMeter;
        out.pad0 = rotate_normal(verts[v].pad0, c, s);
        verts.push_back(out);
      }
      copies_per_tier[tier].push_back({g, offset});
    }
  }
  *plants_out += plant_count;

  // the copies' strips, one new draw per original cutout draw and tier
  const size_t original_draws = tree.static_draws.size();
  for (size_t d = 0; d < original_draws; d++) {
    const auto draw = tree.static_draws[d];
    if (!draw.mode.get_at_enable()) {
      continue;
    }
    // this draw's strips, per plant
    std::unordered_map<u32, std::vector<std::pair<u32, u32>>> strips;  // plant -> [start, end)
    u32 end = draw.first_index_index + draw.num_indices;
    u32 start = draw.first_index_index;
    for (u32 k = draw.first_index_index; k <= end; k++) {
      if (k == end || tree.indices[k] == UINT32_MAX) {
        if (k > start) {
          u32 v = tree.indices[start];
          if (v < plant_of.size() && plant_of[v] != UINT32_MAX && plants[plant_of[v]].ok) {
            strips[plant_of[v]].emplace_back(start, k);
          }
        }
        start = k + 1;
      }
    }
    if (strips.empty()) {
      continue;
    }
    for (int tier = 1; tier <= 2; tier++) {
      tfrag3::ShrubDraw extra = draw;
      extra.first_index_index = tree.indices.size();
      extra.num_triangles = 0;
      extra.enh_tier = tier;
      for (const auto& copy : copies_per_tier[tier]) {
        auto it = strips.find(copy.plant);
        if (it == strips.end()) {
          continue;
        }
        const u32 first = plants[copy.plant].first_vert;
        for (auto [a, b] : it->second) {
          for (u32 k = a; k < b; k++) {
            tree.indices.push_back(tree.indices[k] - first + copy.vert_offset);
          }
          tree.indices.push_back(UINT32_MAX);
          extra.num_triangles += b - a >= 2 ? b - a - 2 : 0;
          (*copies_out)++;
        }
      }
      extra.num_indices = tree.indices.size() - extra.first_index_index;
      if (extra.num_indices) {
        tree.static_draws.push_back(extra);
      }
    }
  }
}
}  // namespace

void enhanced_add_vegetation(tfrag3::Level& level, const std::string& name) {
  if (level.shrub_trees.empty()) {
    return;
  }
  Ground ground;
  auto pos = [](const tfrag3::PreloadedVertex& v) { return Vec3(v.x, v.y, v.z); };
  for (const auto& tree : level.tfrag_trees[0]) {
    if (tree.kind == tfrag3::TFragmentTreeKind::NORMAL ||
        tree.kind == tfrag3::TFragmentTreeKind::DIRT) {
      ground.add_mesh(tree.unpacked.vertices, tree.unpacked.indices, tree.use_strips, pos);
    }
  }
  for (const auto& tree : level.tie_trees[0]) {
    ground.add_mesh(tree.unpacked.vertices, tree.unpacked.indices, true, pos);
  }
  int plants = 0, strips = 0;
  for (u32 t = 0; t < level.shrub_trees.size(); t++) {
    add_to_tree(level.shrub_trees[t], ground, t, &plants, &strips);
  }
  lg::info("[enhanced] vegetation {}: {} floor triangles, {} plants copied ({} strips)", name,
           ground.size(), plants, strips);
}
