#include "EnhancedVegetation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "common/log/log.h"

int g_enhanced_vegetation = [] {
  const char* env = std::getenv("JAK_ENHANCED_VEGETATION");
  return env ? std::clamp(std::atoi(env), 0, 2) : 2;
}();

namespace {

constexpr float kMeter = 4096.f;
constexpr u32 kNoTex = UINT32_MAX;

using Vec3 = math::Vector3f;

// the height field (Jak 3's desert floor, an hfrag): 512 x 512 heights 8 m apart from the world
// origin, kept from the level that has it for the levels loaded after it (their stones stand on it)
constexpr int kHfragEdge = 512;
constexpr float kHfragStep = 8.f * kMeter;
std::mutex g_hfrag_mutex;
std::shared_ptr<const std::vector<float>> g_hfrag;

void keep_hfrag(const tfrag3::Level& level) {
  const auto& verts = level.hfrag.vertices;
  if (verts.size() < kHfragEdge * kHfragEdge / 2) {
    return;
  }
  auto h = std::make_shared<std::vector<float>>(kHfragEdge * kHfragEdge, NAN);
  for (const auto& v : verts) {
    if (v.vi < h->size()) {
      (*h)[v.vi] = v.height;
    }
  }
  std::lock_guard<std::mutex> lock(g_hfrag_mutex);
  g_hfrag = h;
}

std::shared_ptr<const std::vector<float>> current_hfrag() {
  std::lock_guard<std::mutex> lock(g_hfrag_mutex);
  return g_hfrag;
}

// the height field's height at (x, z), between its four nearest heights
bool hfrag_height(const std::vector<float>& h, float x, float z, float* out) {
  float fx = x / kHfragStep, fz = z / kHfragStep;
  int x0 = (int)std::floor(fx), z0 = (int)std::floor(fz);
  if (x0 < 0 || z0 < 0 || x0 + 1 >= kHfragEdge || z0 + 1 >= kHfragEdge) {
    return false;
  }
  float tx = fx - x0, tz = fz - z0;
  float a = h[z0 * kHfragEdge + x0], b = h[z0 * kHfragEdge + x0 + 1];
  float c = h[(z0 + 1) * kHfragEdge + x0], d = h[(z0 + 1) * kHfragEdge + x0 + 1];
  if (std::isnan(a) || std::isnan(b) || std::isnan(c) || std::isnan(d)) {
    return false;
  }
  *out = (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz;
  return true;
}

/*!
 * The level's floor: its upward-facing triangles (tfrag and tie, most detailed), bucketed on a 2D
 * grid, for "how high is the ground here, and what texture is it".
 */
class Ground {
 public:
  void add(const Vec3& a, const Vec3& b, const Vec3& c, u32 tex) {
    Vec3 n = (b - a).cross(c - a);
    float len = n.length();
    if (!(len > 1.f) || std::abs(n.y()) / len < 0.7f) {
      return;  // degenerate, or a wall / steep slope
    }
    int x0 = cell(std::min({a.x(), b.x(), c.x()})), x1 = cell(std::max({a.x(), b.x(), c.x()}));
    int z0 = cell(std::min({a.z(), b.z(), c.z()})), z1 = cell(std::max({a.z(), b.z(), c.z()}));
    if ((x1 - x0 + 1) * (z1 - z0 + 1) > 400) {
      return;  // huge triangle (far terrain): not a floor plants stand on
    }
    u32 id = m_tex.size();
    m_tris.push_back(a);
    m_tris.push_back(b);
    m_tris.push_back(c);
    m_tex.push_back(tex);
    for (int x = x0; x <= x1; x++) {
      for (int z = z0; z <= z1; z++) {
        m_cells[key(x, z)].push_back(id);
      }
    }
  }

  // triangles from an index range (strips with restarts, or a triangle list)
  template <typename Verts>
  void add_mesh(const Verts& verts,
                const std::vector<u32>& indices,
                size_t first,
                size_t end,
                bool strips,
                u32 tex) {
    auto pos = [&](u32 i) { return Vec3(verts[i].x, verts[i].y, verts[i].z); };
    auto tri = [&](u32 a, u32 b, u32 c) {
      if (a < verts.size() && b < verts.size() && c < verts.size() && a != b && b != c && a != c) {
        add(pos(a), pos(b), pos(c), tex);
      }
    };
    if (strips) {
      size_t start = first;
      for (size_t i = first; i < end; i++) {
        if (indices[i] == UINT32_MAX) {
          start = i + 1;
        } else if (i >= start + 2) {
          tri(indices[i - 2], indices[i - 1], indices[i]);
        }
      }
    } else {
      for (size_t i = first; i + 2 < end; i += 3) {
        tri(indices[i], indices[i + 1], indices[i + 2]);
      }
    }
  }

  // the floor height at (x, z) closest to y_ref within [y_ref - below, y_ref + above], and the
  // texture of that floor (kNoTex for ties)
  bool height(float x,
              float z,
              float y_ref,
              float below,
              float above,
              float* out,
              u32* tex = nullptr) const {
    auto it = m_cells.find(key(cell(x), cell(z)));
    bool found = false;
    float best = 0;
    // (the height field under it all, where there's one)
    float hy;
    if (hfrag && hfrag_height(*hfrag, x, z, &hy) && hy >= y_ref - below && hy <= y_ref + above) {
      best = hy;
      found = true;
      if (tex) {
        *tex = kNoTex;
      }
    }
    if (it == m_cells.end()) {
      if (found) {
        *out = best;
      }
      return found;
    }
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
        if (tex) {
          *tex = m_tex[id];
        }
      }
    }
    if (found) {
      *out = best;
    }
    return found;
  }

  size_t size() const { return m_tex.size(); }
  std::shared_ptr<const std::vector<float>> hfrag;
  const Vec3* tri(u32 id) const { return &m_tris[id * 3]; }
  u32 tex(u32 id) const { return m_tex[id]; }

 private:
  static constexpr float kCell = 4.f * kMeter;
  static int cell(float v) { return (int)std::floor(v / kCell); }
  static u64 key(int x, int z) { return ((u64)(u32)x << 32) | (u32)z; }
  std::vector<Vec3> m_tris;
  std::vector<u32> m_tex;
  std::unordered_map<u64, std::vector<u32>> m_cells;
};

/*!
 * The level's walls: its upright triangles (tfrag and tie), for more of the level's wall vines.
 */
struct Walls {
  struct Tri {
    Vec3 a, b, c;
    Vec3 n;      // horizontal, unit (either side)
    float area;  // m2
    float lo, hi;
  };
  std::vector<Tri> tris;

  void add(const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 n = (b - a).cross(c - a);
    float len = n.length();
    if (!(len > 1.f) || std::abs(n.y()) / len > 0.25f) {
      return;  // degenerate, or not upright
    }
    float area = 0.5f * len / (kMeter * kMeter);
    if (area < 0.5f || area > 400.f) {
      return;
    }
    Vec3 h(n.x(), 0.f, n.z());
    u32 id = tris.size();
    tris.push_back({a, b, c, h / h.length(), area, std::min({a.y(), b.y(), c.y()}),
                    std::max({a.y(), b.y(), c.y()})});
    int x0 = cell(std::min({a.x(), b.x(), c.x()})), x1 = cell(std::max({a.x(), b.x(), c.x()}));
    int z0 = cell(std::min({a.z(), b.z(), c.z()})), z1 = cell(std::max({a.z(), b.z(), c.z()}));
    for (int x = x0; x <= x1; x++) {
      for (int z = z0; z <= z1; z++) {
        cells[key(x, z)].push_back(id);
      }
    }
  }

  // how far the wall through `at` (facing n) goes down and up from [lo, hi]: the triangles in
  // its plane over that spot, chained
  void extent(const Vec3& at, const Vec3& n, float* lo, float* hi) const {
    auto it = cells.find(key(cell(at.x()), cell(at.z())));
    if (it == cells.end()) {
      return;
    }
    for (bool changed = true; changed;) {
      changed = false;
      for (u32 id : it->second) {
        const auto& t = tris[id];
        if (std::abs(t.n.dot(n)) < 0.9f || t.lo >= *hi + 0.2f * kMeter || t.hi <= *lo - 0.2f * kMeter ||
            (t.lo >= *lo && t.hi <= *hi)) {
          continue;  // another plane, not touching, or nothing new
        }
        // in the plane and across this spot
        Vec3 along(-n.z(), 0.f, n.x());
        float s = (at - t.a).dot(along);
        float s1 = (t.b - t.a).dot(along), s2 = (t.c - t.a).dot(along);
        if (std::abs((at - t.a).dot(n)) > 0.3f * kMeter ||
            s < std::min({0.f, s1, s2}) - 0.2f * kMeter || s > std::max({0.f, s1, s2}) + 0.2f * kMeter) {
          continue;
        }
        *lo = std::min(*lo, t.lo);
        *hi = std::max(*hi, t.hi);
        changed = true;
      }
    }
  }

  template <typename Verts>
  void add_mesh(const Verts& verts,
                const std::vector<u32>& indices,
                size_t first,
                size_t end,
                bool strips) {
    auto pos = [&](u32 i) { return Vec3(verts[i].x, verts[i].y, verts[i].z); };
    auto tri = [&](u32 a, u32 b, u32 c) {
      if (a < verts.size() && b < verts.size() && c < verts.size() && a != b && b != c && a != c) {
        add(pos(a), pos(b), pos(c));
      }
    };
    if (strips) {
      size_t start = first;
      for (size_t i = first; i < end; i++) {
        if (indices[i] == UINT32_MAX) {
          start = i + 1;
        } else if (i >= start + 2) {
          tri(indices[i - 2], indices[i - 1], indices[i]);
        }
      }
    } else {
      for (size_t i = first; i + 2 < end; i += 3) {
        tri(indices[i], indices[i + 1], indices[i + 2]);
      }
    }
  }

 private:
  static constexpr float kCell = 4.f * kMeter;
  static int cell(float v) { return (int)std::floor(v / kCell); }
  static u64 key(int x, int z) { return ((u64)(u32)x << 32) | (u32)z; }
  std::unordered_map<u64, std::vector<u32>> cells;
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

Vec3 unpack_normal(u32 nor) {
  auto get = [&](int shift) {
    int v = (nor >> shift) & 0x3ff;
    return (float)(v >= 512 ? v - 1024 : v);
  };
  Vec3 u(get(0), get(10), get(20));
  float l = u.length();
  return l > 0 ? u / l : u;
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
  float height = 0;
  bool seen = false;
  bool cutout = true;    // only in cutout draws
  bool grounded = false;
  bool crossed = false;  // made of crossed cards (a plant, not a fence or litter)
  bool copy = false;     // gets copies around it
  bool stone = false;    // a pebble or a little rock (solid): copies scattered wider
  int proto = -1;
  u32 floor_tex = kNoTex;
};

// a strip of the tree's index buffer
struct Strip {
  u32 start, end;  // in tree.indices
};

// A "brush" card: one vertical quad of a grass plant, scattered as tufts on its kind of floor
struct Card {
  u32 draw;  // the draw it's in (texture, mode)
  Strip strip;
  float cx, cz, base;  // its foot
};

// copies of a group of vertices: turned by yaw around (cx, cz), scaled, the foot (base) set at
// (x, y, z). Returns where they start.
u32 place(std::vector<tfrag3::ShrubGpuVertex>& verts,
          u32 first,
          u32 end,
          float cx,
          float cz,
          float base,
          float x,
          float y,
          float z,
          float yaw,
          float scale) {
  float c = std::cos(yaw), s = std::sin(yaw);
  u32 offset = verts.size();
  for (u32 v = first; v < end; v++) {
    auto out = verts[v];
    float dx = (verts[v].x - cx) * scale, dz = (verts[v].z - cz) * scale;
    out.x = x + c * dx + s * dz;
    out.z = z - s * dx + c * dz;
    // sunk a little, so a copy on a gentle slope doesn't float
    out.y = y + (verts[v].y - base) * scale - 0.03f * kMeter;
    out.pad0 = rotate_normal(verts[v].pad0, c, s);
    verts.push_back(out);
  }
  return offset;
}

struct Stats {
  int plants = 0, copies = 0, tufts = 0, habitats = 0, vine_sources = 0, vines = 0;
};

void add_to_tree(tfrag3::ShrubTree& tree,
                 const std::vector<tfrag3::Texture>& textures,
                 const Ground& ground,
                 const Walls& walls,
                 float max_vines,
                 u32 tree_idx,
                 float max_tufts,
                 Stats* stats) {
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

  // every draw's strips, and which plants show up in non-cutout draws
  std::vector<std::vector<Strip>> draw_strips(tree.static_draws.size());
  for (size_t d = 0; d < tree.static_draws.size(); d++) {
    const auto& draw = tree.static_draws[d];
    bool cutout = draw.mode.get_at_enable();
    u32 end = draw.first_index_index + draw.num_indices;
    u32 start = draw.first_index_index;
    for (u32 k = draw.first_index_index; k <= end; k++) {
      if (k == end || tree.indices[k] == UINT32_MAX) {
        if (k > start) {
          draw_strips[d].push_back({start, k});
        }
        start = k + 1;
      } else {
        u32 v = tree.indices[k];
        if (v < verts.size() && plant_of[v] != UINT32_MAX) {
          auto& p = plants[plant_of[v]];
          p.seen = true;
          p.proto = draw.proto_idx;
          if (!cutout) {
            p.cutout = false;
          }
        }
      }
    }
  }

  // small cutout plants standing on the floor; the ones made of crossed cards (not a flat fence
  // or sign) get copies around them
  for (auto& p : plants) {
    if (p.seen && p.proto >= 0 && p.proto < (int)tree.proto_names.size()) {
      // (the desert's pebbles and little rock clusters: not crossed cards, but strewn about like
      // the plants)
      const auto& name = tree.proto_names[p.proto];
      p.stone = name.find("pebble") != std::string::npos ||
                name.find("rock-cluster") != std::string::npos;
    }
    if (!p.seen || (!p.cutout && !p.stone) || p.end_vert - p.first_vert < 4) {
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
    p.height = max_y - min_y;
    // 30 cm and up: pebbles and litter are crossed cards too
    p.grounded = p.height < 1.6f * kMeter && p.height > (p.stone ? 0.03f : 0.3f) * kMeter &&
                 ground.height(p.cx, p.cz, min_y, 0.4f * kMeter, 0.4f * kMeter, &p.base,
                               &p.floor_tex);
    bool crossed = false;
    Vec3 first = unpack_normal(verts[p.first_vert].pad0);
    for (u32 v = p.first_vert; v < p.end_vert && !crossed; v++) {
      if (std::abs(unpack_normal(verts[v].pad0).dot(first)) < 0.7f) {
        crossed = true;
      }
    }
    p.crossed = crossed;
    p.copy = p.grounded && (crossed || p.stone) && r < (p.stone ? 6.f : 2.f) * kMeter &&
             r > 0.08f * kMeter;
  }

  // the new strips: per original draw and tier
  std::vector<std::array<std::vector<u32>, 3>> extra(tree.static_draws.size());
  auto emit = [&](u32 d, int tier, const Strip& s, u32 first_vert, u32 offset) {
    auto& out = extra[d][tier];
    for (u32 k = s.start; k < s.end; k++) {
      out.push_back(tree.indices[k] - first_vert + offset);
    }
    out.push_back(UINT32_MAX);
  };
  // per plant, its strips in each draw
  std::unordered_map<u32, std::vector<std::pair<u32, Strip>>> plant_strips;
  for (u32 d = 0; d < draw_strips.size(); d++) {
    for (const auto& s : draw_strips[d]) {
      u32 v = tree.indices[s.start];
      if (v < plant_of.size() && plant_of[v] != UINT32_MAX && plants[plant_of[v]].copy) {
        plant_strips[plant_of[v]].emplace_back(d, s);
      }
    }
  }

  // 1: copies around the plants (tier 1: one per plant, tier 2: two more)
  for (u32 g = 0; g < plants.size(); g++) {
    const auto& p = plants[g];
    if (!p.copy) {
      continue;
    }
    stats->plants++;
    // (stones: more of them and wider, a strewn field instead of a ring)
    const u32 n_copies = p.stone ? 14 : 3;
    for (u32 k = 0; k < n_copies; k++) {
      int tier = k < n_copies / 3 ? 1 : 2;
      float ang = 6.2831853f * (rnd(tree_idx, g, k * 7 + 1) + k / (float)n_copies);
      float dist = p.stone ? p.radius * 1.2f + (0.8f + 7.f * rnd(tree_idx, g, k * 7 + 2)) * kMeter
                           : p.radius * (1.1f + 1.2f * rnd(tree_idx, g, k * 7 + 2)) + 0.25f * kMeter;
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
      if (p.stone && p.radius > 1.5f * kMeter) {
        // (a wide cluster's copies smaller: little heaps of it, each sitting on its own spot)
        scale = (0.6f + 0.5f * rnd(tree_idx, g, k * 7 + 4)) * kMeter / p.radius;
      }
      u32 offset = place(verts, p.first_vert, p.end_vert, p.cx, p.cz, p.base, x, y, z, yaw, scale);
      for (const auto& [d, s] : plant_strips[g]) {
        emit(d, tier, s, p.first_vert, offset);
      }
      // (so a detailed model can stand in for the copy too)
      tree.enh_copies.push_back({p.first_vert, p.end_vert, offset, (u8)tier, p.cx, p.cz, p.base,
                                 x, y - 0.03f * kMeter, z, yaw, scale});
      stats->copies++;
    }
  }

  // 2: climbing plants: the level's own wall vines (shrubs with a "vine" texture, flat against a
  // wall) copied onto the other walls around them, turned to face out of the wall, as high above
  // the floor as the original (tier 2, a third of them tier 1)
  struct Vine {
    u32 plant;
    Vec3 n;  // out of its wall
    float cx, cz, min_y, height, width, rise;  // rise: its bottom above the floor
  };
  std::vector<Vine> vines;
  std::unordered_map<u32, std::vector<std::pair<u32, Strip>>> vine_strips;
  for (u32 d = 0; d < draw_strips.size(); d++) {
    u32 tex = tree.static_draws[d].tree_tex_id;
    if (tex >= textures.size() || textures[tex].debug_name.find("vine") == std::string::npos ||
        textures[tex].debug_name.find("wriggler") != std::string::npos) {
      continue;
    }
    for (const auto& s : draw_strips[d]) {
      u32 v = tree.indices[s.start];
      if (v < plant_of.size() && plant_of[v] != UINT32_MAX) {
        vine_strips[plant_of[v]].emplace_back(d, s);
      }
    }
  }
  for (const auto& [g, strips] : vine_strips) {
    const auto& p = plants[g];
    if (p.end_vert - p.first_vert < 3) {
      continue;
    }
    Vine vine{g, Vec3(0.f, 0.f, 0.f), 0, 0, 1e30f, 0, 0, 0};
    float max_y = -1e30f;
    for (u32 v = p.first_vert; v < p.end_vert; v++) {
      vine.n = vine.n + unpack_normal(verts[v].pad0);
      vine.cx += verts[v].x / (p.end_vert - p.first_vert);
      vine.cz += verts[v].z / (p.end_vert - p.first_vert);
      vine.min_y = std::min(vine.min_y, verts[v].y);
      max_y = std::max(max_y, verts[v].y);
    }
    Vec3 h(vine.n.x(), 0.f, vine.n.z());
    if (h.length() < 0.5f * vine.n.length() || h.length() < 1e-3f) {
      continue;  // not flat against a wall
    }
    vine.n = h / h.length();
    for (u32 v = p.first_vert; v < p.end_vert; v++) {
      vine.width = std::max(vine.width, 2.f * std::hypot(verts[v].x - vine.cx, verts[v].z - vine.cz));
    }
    vine.height = max_y - vine.min_y;
    if (vine.height < 0.3f * kMeter || vine.width > 8.f * kMeter || vine.height > 10.f * kMeter) {
      continue;
    }
    // the wall's open side: the one with a floor below the vine
    bool found = false;
    for (int side = 0; side < 2 && !found; side++) {
      Vec3 n = side == 0 ? vine.n : vine.n * -1.f;
      float floor_y;
      if (ground.height(vine.cx + n.x() * 0.6f * kMeter, vine.cz + n.z() * 0.6f * kMeter,
                        vine.min_y, 6.f * kMeter, 0.5f * kMeter, &floor_y)) {
        vine.n = n;
        vine.rise = std::max(0.f, vine.min_y - floor_y);
        found = true;
      }
    }
    if (found) {
      vines.push_back(vine);
    }
  }
  stats->vine_sources += vines.size();
  if (const char* env = std::getenv("JAK_ENHANCED_VINES"); env && std::atoi(env) == 0) {
    vines.clear();  // A/B tests
  }
  constexpr float kVineNear = 30.f * kMeter;
  float vine_thin = 1.f;
  std::unordered_set<u64> vine_taken;  // one vine per 2 x 2 x 3 m cell
  for (int pass = 0; pass < 2 && !vines.empty(); pass++) {
    int candidates = 0;
    for (u32 id = 0; id < walls.tris.size(); id++) {
      const auto& w = walls.tris[id];
      float want = w.area * 0.3f;
      int count = (int)want + (rnd(tree_idx, id, 4441) < want - (int)want ? 1 : 0);
      for (int k = 0; k < count; k++) {
        float u = rnd(tree_idx, id, k * 9 + 11), v = rnd(tree_idx, id, k * 9 + 12);
        if (u + v > 1.f) {
          u = 1.f - u;
          v = 1.f - v;
        }
        Vec3 at = w.a + (w.b - w.a) * u + (w.c - w.a) * v;
        // the nearest vine (not right next to one already there)
        const Vine* src = nullptr;
        float best = kVineNear;
        for (const auto& vine : vines) {
          float d = std::hypot(vine.cx - at.x(), vine.cz - at.z());
          if (d < vine.width) {
            src = nullptr;
            break;
          }
          if (d < best) {
            best = d;
            src = &vine;
          }
        }
        if (!src) {
          continue;
        }
        // the floor in front of the wall (either side), and the wall tall enough for the vine
        float lo = w.lo, hi = w.hi;
        walls.extent(at, w.n, &lo, &hi);
        Vec3 n = w.n;
        float floor_y = 0;
        bool found = false;
        for (int side = 0; side < 2 && !found; side++) {
          n = side == 0 ? w.n : w.n * -1.f;
          found = ground.height(at.x() + n.x() * 0.6f * kMeter, at.z() + n.z() * 0.6f * kMeter,
                                at.y(), 6.f * kMeter, 0.5f * kMeter, &floor_y) &&
                  floor_y + src->rise > lo - 0.1f * kMeter &&
                  floor_y + src->rise + src->height < hi + 0.3f * kMeter;
        }
        // thinner further from the vines already there
        if (!found || rnd(tree_idx, id, k * 9 + 13) > 1.f - best / kVineNear) {
          continue;
        }
        if (pass == 0) {
          candidates++;
          continue;
        }
        if (rnd(tree_idx, id, k * 9 + 14) > vine_thin) {
          continue;
        }
        u64 cell = ((u64)(u32)(int)std::floor(at.x() / (2.f * kMeter)) << 40) ^
                   ((u64)(u32)(int)std::floor(at.z() / (2.f * kMeter)) << 16) ^
                   (u64)(u32)(int)std::floor(floor_y / (3.f * kMeter));
        if (!vine_taken.insert(cell).second) {
          continue;
        }
        int tier = rnd(tree_idx, id, k * 9 + 15) < 0.33f ? 1 : 2;
        // turn the vine's normal onto this wall's
        float c = src->n.x() * n.x() + src->n.z() * n.z();
        float s = src->n.z() * n.x() - src->n.x() * n.z();
        float scale = 0.85f + 0.3f * rnd(tree_idx, id, k * 9 + 16);
        const auto& p = plants[src->plant];
        // a few cm off the wall, so it doesn't flicker into it
        u32 offset = place(verts, p.first_vert, p.end_vert, src->cx, src->cz, src->min_y,
                           at.x() + n.x() * 0.05f * kMeter, floor_y + src->rise + 0.03f * kMeter,
                           at.z() + n.z() * 0.05f * kMeter, std::atan2(s, c), scale);
        for (const auto& [d, strip] : vine_strips[src->plant]) {
          emit(d, tier, strip, p.first_vert, offset);
        }
        stats->vines++;
      }
    }
    if (pass == 0) {
      vine_thin = candidates > max_vines ? max_vines / candidates : 1.f;
    }
  }

  // 3: tufts on the floors the game already grows grass on. A habitat is a floor texture under
  // at least 3 small plants; the brushes are their upright quads (single cards).
  std::unordered_map<u32, int> habitat_count;
  for (const auto& p : plants) {
    if (p.grounded && p.floor_tex != kNoTex) {
      habitat_count[p.floor_tex]++;
    }
  }
  std::unordered_map<u32, std::vector<Card>> brushes;
  std::unordered_map<u32, std::vector<Card>> plant_cards;  // plant -> its own cards
  for (u32 d = 0; d < draw_strips.size(); d++) {
    if (!tree.static_draws[d].mode.get_at_enable()) {
      continue;
    }
    for (const auto& s : draw_strips[d]) {
      if (s.end - s.start < 4 || s.end - s.start > 8) {
        continue;  // one to three quads
      }
      u32 v0 = tree.indices[s.start];
      if (v0 >= plant_of.size() || plant_of[v0] == UINT32_MAX) {
        continue;
      }
      const auto& p = plants[plant_of[v0]];
      if (!p.grounded || !p.crossed || p.floor_tex == kNoTex || habitat_count[p.floor_tex] < 3) {
        continue;  // only cards of real plants (crossed cards), not pebbles or litter
      }
      float min_y = 1e30f, max_y = -1e30f, cx = 0, cz = 0;
      bool bad = false;
      for (u32 k = s.start; k < s.end; k++) {
        u32 v = tree.indices[k];
        if (v >= verts.size()) {
          bad = true;
          break;
        }
        min_y = std::min(min_y, verts[v].y);
        max_y = std::max(max_y, verts[v].y);
        cx += verts[v].x / (s.end - s.start);
        cz += verts[v].z / (s.end - s.start);
      }
      float h = max_y - min_y;
      float width = 0;
      for (u32 k = s.start; k < s.end; k++) {
        u32 v = tree.indices[k];
        width = std::max(width, 2.f * std::hypot(verts[v].x - cx, verts[v].z - cz));
      }
      if (bad || h < 0.2f * kMeter || h > 1.2f * kMeter || min_y > p.base + 0.3f * kMeter ||
          h < 0.5f * width || std::abs(unpack_normal(verts[tree.indices[s.start]].pad0).y()) > 0.5f) {
        continue;  // too small or big, lying down, wider than tall, or not on the floor
      }
      auto& list = brushes[p.floor_tex];
      if (list.size() < 48) {
        list.push_back({d, s, cx, cz, min_y});
      }
      auto& own = plant_cards[plant_of[v0]];
      if (own.size() < 16) {
        own.push_back({d, s, cx, cz, min_y});
      }
    }
  }
  stats->habitats += brushes.size();
  if (brushes.empty()) {
    return;
  }

  // tufts only grow near plants already there (a patch gets fuller, a desert stays a desert)
  constexpr float kNear = 4.5f * kMeter;
  std::unordered_map<u64, std::vector<u32>> near_cells;  // kNear cells -> plants
  auto near_key = [&](float x, float z) {
    return ((u64)(u32)(int)std::floor(x / kNear) << 32) | (u32)(int)std::floor(z / kNear);
  };
  for (u32 g = 0; g < plants.size(); g++) {
    // only plants with grass cards of their own seed tufts (not a trail of pebbles)
    if (plants[g].grounded && plant_cards.count(g) && brushes.count(plants[g].floor_tex)) {
      near_cells[near_key(plants[g].cx, plants[g].cz)].push_back(g);
    }
  }
  // the nearest plant on the same floor within kNear (UINT32_MAX if none), and how far
  auto near_plant = [&](float x, float z, u32 tex, float* dist) {
    int cx = (int)std::floor(x / kNear), cz = (int)std::floor(z / kNear);
    u32 best = UINT32_MAX;
    *dist = kNear;
    for (int dx = -1; dx <= 1; dx++) {
      for (int dz = -1; dz <= 1; dz++) {
        auto it = near_cells.find(((u64)(u32)(cx + dx) << 32) | (u32)(cz + dz));
        if (it == near_cells.end()) {
          continue;
        }
        for (u32 g : it->second) {
          const auto& p = plants[g];
          // from the plant's edge, not its middle (long grass bands)
          float d = std::max(0.f, std::hypot(p.cx - x, p.cz - z) - p.radius);
          if (p.floor_tex == tex && d < *dist) {
            *dist = d;
            best = g;
          }
        }
      }
    }
    return best;
  };

  // lush: 6 tufts / m2 next to the plants, thinning out to none at kNear (a third of them in
  // moderate). Two passes: count, then thin out evenly to the budget (not just the first floors).
  const float density = 6.f;
  float thin = 1.f;
  for (int pass = 0; pass < 2; pass++) {
    int candidates = 0;
    for (u32 id = 0; id < ground.size(); id++) {
      auto it = brushes.find(ground.tex(id));
      if (it == brushes.end()) {
        continue;
      }
      const Vec3* t = ground.tri(id);
      float a = 0.5f * std::abs((t[1] - t[0]).cross(t[2] - t[0]).y()) / (kMeter * kMeter);
      float want = a * density;
      int count = (int)want + (rnd(tree_idx, id, 991) < want - (int)want ? 1 : 0);
      for (int k = 0; k < count; k++) {
        float u = rnd(tree_idx, id, k * 5 + 1), w = rnd(tree_idx, id, k * 5 + 2);
        if (u + w > 1.f) {
          u = 1.f - u;
          w = 1.f - w;
        }
        Vec3 at = t[0] + (t[1] - t[0]) * u + (t[2] - t[0]) * w;
        float dist;
        u32 nearest = near_plant(at.x(), at.z(), ground.tex(id), &dist);
        // thinning out away from the plants: patches, not litter
        float keep = 1.f - dist / kNear;
        if (nearest == UINT32_MAX || rnd(tree_idx, id, k * 5 + 7) > keep) {
          continue;
        }
        if (pass == 0) {
          candidates++;
          continue;
        }
        if (rnd(tree_idx, id, k * 5 + 8) > thin) {
          continue;
        }
        const auto& cards = plant_cards.at(nearest);
        const auto& card =
            cards[(u32)(rnd(tree_idx, id, k * 5 + 3) * cards.size()) % cards.size()];
        int tier = rnd(tree_idx, id, k * 5 + 4) < 0.35f ? 1 : 2;
        float yaw = 6.2831853f * rnd(tree_idx, id, k * 5 + 5);
        float scale = 0.7f + 0.6f * rnd(tree_idx, id, k * 5 + 6);
        // two crossed cards
        for (int c = 0; c < 2; c++) {
          // the card's 4 vertices, copied as a block (they may not be contiguous)
          u32 offset = verts.size();
          float cs = std::cos(yaw + c * 1.5707963f), sn = std::sin(yaw + c * 1.5707963f);
          for (u32 q = card.strip.start; q < card.strip.end; q++) {
            u32 v = tree.indices[q];
            auto out = verts[v];
            float dx = (verts[v].x - card.cx) * scale, dz = (verts[v].z - card.cz) * scale;
            out.x = at.x() + cs * dx + sn * dz;
            out.z = at.z() - sn * dx + cs * dz;
            out.y = at.y() + (verts[v].y - card.base) * scale - 0.03f * kMeter;
            out.pad0 = rotate_normal(verts[v].pad0, cs, sn);
            verts.push_back(out);
          }
          auto& out = extra[card.draw][tier];
          for (u32 q = 0; q < card.strip.end - card.strip.start; q++) {
            out.push_back(offset + q);
          }
          out.push_back(UINT32_MAX);
        }
        stats->tufts++;
      }
    }
    if (pass == 0) {
      thin = candidates > max_tufts ? max_tufts / candidates : 1.f;
    }
  }

  // the extra draws
  const size_t original_draws = tree.static_draws.size();
  for (size_t d = 0; d < original_draws; d++) {
    for (int tier = 1; tier <= 2; tier++) {
      auto& inds = extra[d][tier];
      if (inds.empty()) {
        continue;
      }
      tfrag3::ShrubDraw draw = tree.static_draws[d];
      draw.first_index_index = tree.indices.size();
      draw.num_indices = inds.size();
      draw.num_triangles = inds.size();  // about (debug counting only)
      draw.enh_tier = tier;
      tree.indices.insert(tree.indices.end(), inds.begin(), inds.end());
      tree.static_draws.push_back(draw);
    }
  }
}
}  // namespace

void enhanced_add_vegetation(tfrag3::Level& level, const std::string& name) {
  keep_hfrag(level);
  if (level.shrub_trees.empty()) {
    return;
  }
  Ground ground;
  ground.hfrag = current_hfrag();
  Walls walls;
  for (const auto& tree : level.tfrag_trees[0]) {
    if (tree.kind != tfrag3::TFragmentTreeKind::NORMAL &&
        tree.kind != tfrag3::TFragmentTreeKind::DIRT) {
      continue;
    }
    const auto& idx = tree.unpacked.indices;
    for (size_t d = 0; d < tree.draws.size(); d++) {
      size_t first = tree.draws[d].unpacked.idx_of_first_idx_in_full_buffer;
      size_t end = d + 1 < tree.draws.size()
                       ? tree.draws[d + 1].unpacked.idx_of_first_idx_in_full_buffer
                       : idx.size();
      ground.add_mesh(tree.unpacked.vertices, idx, first, end, tree.use_strips,
                      tree.draws[d].tree_tex_id);
      walls.add_mesh(tree.unpacked.vertices, idx, first, end, tree.use_strips);
    }
  }
  for (const auto& tree : level.tie_trees[0]) {
    ground.add_mesh(tree.unpacked.vertices, tree.unpacked.indices, 0, tree.unpacked.indices.size(),
                    true, kNoTex);
    walls.add_mesh(tree.unpacked.vertices, tree.unpacked.indices, 0, tree.unpacked.indices.size(),
                   true);
  }
  Stats stats;
  for (u32 t = 0; t < level.shrub_trees.size(); t++) {
    add_to_tree(level.shrub_trees[t], level.textures, ground, walls,
                4000.f / level.shrub_trees.size(), t, 150000.f / level.shrub_trees.size(), &stats);
  }
  lg::info("[enhanced] vegetation {}: {} floor triangles, {} plants with {} copies, {} tufts on {} "
           "grassy floors, {} wall vines with {} copies{}",
           name, ground.size(), stats.plants, stats.copies, stats.tufts, stats.habitats,
           stats.vine_sources, stats.vines, ground.hfrag ? " (and the height field)" : "");
}
