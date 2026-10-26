#include "EnhancedRayTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/Timer.h"

bool g_enhanced_rt_build = [] {
  const char* env = std::getenv("JAK_ENHANCED_RT");
  return !(env && std::strcmp(env, "0") == 0);
}();

namespace {

std::mutex g_rt_mutex;
std::unordered_map<std::string, std::unique_ptr<EnhancedRtLevel>> g_rt_levels;

constexpr int kLeafTris = 4;
constexpr int kBins = 12;

struct Box {
  float lo[3] = {1e30f, 1e30f, 1e30f};
  float hi[3] = {-1e30f, -1e30f, -1e30f};
  void grow(const float* p) {
    for (int i = 0; i < 3; i++) {
      lo[i] = std::min(lo[i], p[i]);
      hi[i] = std::max(hi[i], p[i]);
    }
  }
  void grow(const Box& b) {
    for (int i = 0; i < 3; i++) {
      lo[i] = std::min(lo[i], b.lo[i]);
      hi[i] = std::max(hi[i], b.hi[i]);
    }
  }
  float area() const {
    float d[3];
    for (int i = 0; i < 3; i++) {
      d[i] = std::max(0.f, hi[i] - lo[i]);
    }
    return d[0] * d[1] + d[1] * d[2] + d[2] * d[0];
  }
};

struct Prim {
  Box box;
  float c[3];
  u32 tri;
};

struct Builder {
  std::vector<Prim> prims;
  std::vector<EnhancedRtLevel::Node> nodes;

  // nodes in depth-first order: an inner node's left child is the next node
  void build(u32 first, u32 count) {
    u32 me = nodes.size();
    nodes.emplace_back();
    Box box, cbox;
    for (u32 i = first; i < first + count; i++) {
      box.grow(prims[i].box);
      cbox.grow(prims[i].c);
    }
    auto leaf = [&]() {
      auto& n = nodes[me];
      std::memcpy(n.min, box.lo, sizeof(n.min));
      std::memcpy(n.max, box.hi, sizeof(n.max));
      n.a = first;
      n.b = count;
    };
    if (count <= kLeafTris) {
      leaf();
      return;
    }
    // binned surface area heuristic on the widest axis of the centroids
    int axis = 0;
    for (int i = 1; i < 3; i++) {
      if (cbox.hi[i] - cbox.lo[i] > cbox.hi[axis] - cbox.lo[axis]) {
        axis = i;
      }
    }
    float extent = cbox.hi[axis] - cbox.lo[axis];
    u32 mid = first + count / 2;
    if (extent > 1e-3f) {
      Box bins[kBins];
      u32 bin_count[kBins] = {};
      float scale = kBins / extent;
      auto bin_of = [&](const Prim& p) {
        return std::min(kBins - 1, (int)((p.c[axis] - cbox.lo[axis]) * scale));
      };
      for (u32 i = first; i < first + count; i++) {
        int b = bin_of(prims[i]);
        bins[b].grow(prims[i].box);
        bin_count[b]++;
      }
      float best = 1e30f;
      int best_split = -1;
      for (int s = 1; s < kBins; s++) {
        Box l, r;
        u32 lc = 0, rc = 0;
        for (int b = 0; b < s; b++) {
          if (bin_count[b]) {
            l.grow(bins[b]);
            lc += bin_count[b];
          }
        }
        for (int b = s; b < kBins; b++) {
          if (bin_count[b]) {
            r.grow(bins[b]);
            rc += bin_count[b];
          }
        }
        if (!lc || !rc) {
          continue;
        }
        float cost = l.area() * lc + r.area() * rc;
        if (cost < best) {
          best = cost;
          best_split = s;
        }
      }
      if (best_split > 0) {
        auto it = std::partition(prims.begin() + first, prims.begin() + first + count,
                                 [&](const Prim& p) { return bin_of(p) < best_split; });
        mid = it - prims.begin();
      }
    }
    if (mid == first || mid == first + count) {
      // all in one bin (or on one spot): split in the middle along the axis
      std::nth_element(prims.begin() + first, prims.begin() + first + count / 2,
                       prims.begin() + first + count,
                       [&](const Prim& a, const Prim& b) { return a.c[axis] < b.c[axis]; });
      mid = first + count / 2;
    }
    build(first, mid - first);
    u32 right = nodes.size();
    build(mid, first + count - mid);
    auto& n = nodes[me];
    std::memcpy(n.min, box.lo, sizeof(n.min));
    std::memcpy(n.max, box.hi, sizeof(n.max));
    n.a = right;
    n.b = 0;
  }
};

// a texture's mean color (its texels that aren't see-through)
u32 mean_color(const tfrag3::Texture& tex) {
  u64 sum[3] = {0, 0, 0};
  u64 n = 0;
  for (u32 px : tex.data) {
    if ((px >> 24) < 64) {
      continue;
    }
    sum[0] += px & 0xff;
    sum[1] += (px >> 8) & 0xff;
    sum[2] += (px >> 16) & 0xff;
    n++;
  }
  if (!n) {
    return 0xff808080;
  }
  return (u32)(sum[0] / n) | ((u32)(sum[1] / n) << 8) | ((u32)(sum[2] / n) << 16) | 0xff000000;
}

void add_palettes(const tfrag3::PackedTimeOfDay& colors, std::vector<u32>* out) {
  for (u32 c = 0; c < colors.color_count; c++) {
    for (int p = 0; p < 8; p++) {
      u32 v = 0;
      for (int ch = 0; ch < 4; ch++) {
        v |= (u32)colors.read(c, p, ch) << (8 * ch);
      }
      out->push_back(v);
    }
  }
}

}  // namespace

void enhanced_build_rt(const tfrag3::Level& level, const std::string& name) {
  if (!g_enhanced_rt_build) {
    return;
  }
  Timer timer;
  auto rt = std::make_unique<EnhancedRtLevel>();
  rt->name = name;
  std::unordered_map<s32, u32> tex_color;
  auto color_of = [&](s32 tex) {
    if (tex < 0 || (size_t)tex >= level.textures.size()) {
      return 0xff808080u;
    }
    auto it = tex_color.find(tex);
    if (it == tex_color.end()) {
      it = tex_color.emplace(tex, mean_color(level.textures[tex])).first;
    }
    return it->second;
  };

  // the triangles of one draw's index range (strips restart at UINT32_MAX)
  auto add_draws = [&](const std::vector<tfrag3::PreloadedVertex>& verts,
                       const std::vector<u32>& idx, const std::vector<tfrag3::StripDraw>& draws,
                       bool strips, u32 tod_base) {
    for (size_t d = 0; d < draws.size(); d++) {
      size_t first = draws[d].unpacked.idx_of_first_idx_in_full_buffer;
      size_t end = d + 1 < draws.size() ? draws[d + 1].unpacked.idx_of_first_idx_in_full_buffer
                                        : idx.size();
      u32 color = color_of(draws[d].tree_tex_id);
      auto tri = [&](u32 a, u32 b, u32 c) {
        if (a >= verts.size() || b >= verts.size() || c >= verts.size() || a == b || b == c ||
            a == c) {
          return;
        }
        const auto &va = verts[a], &vb = verts[b], &vc = verts[c];
        EnhancedRtLevel::Tri t;
        t.v0[0] = va.x;
        t.v0[1] = va.y;
        t.v0[2] = va.z;
        t.e1[0] = vb.x - va.x;
        t.e1[1] = vb.y - va.y;
        t.e1[2] = vb.z - va.z;
        t.e2[0] = vc.x - va.x;
        t.e2[1] = vc.y - va.y;
        t.e2[2] = vc.z - va.z;
        float cx = t.e1[1] * t.e2[2] - t.e1[2] * t.e2[1];
        float cy = t.e1[2] * t.e2[0] - t.e1[0] * t.e2[2];
        float cz = t.e1[0] * t.e2[1] - t.e1[1] * t.e2[0];
        if (cx * cx + cy * cy + cz * cz < 1.f) {
          return;  // degenerate
        }
        t.color = color;
        t.tod = tod_base + va.color_index;
        t.pad = 0;
        rt->tris.push_back(t);
      };
      if (strips) {
        size_t start = first;
        for (size_t i = first; i < end; i++) {
          if (idx[i] == UINT32_MAX) {
            start = i + 1;
          } else if (i >= start + 2) {
            tri(idx[i - 2], idx[i - 1], idx[i]);
          }
        }
      } else {
        for (size_t i = first; i + 2 < end; i += 3) {
          tri(idx[i], idx[i + 1], idx[i + 2]);
        }
      }
    }
  };

  for (const auto& tree : level.tfrag_trees[0]) {
    if (tree.kind != tfrag3::TFragmentTreeKind::NORMAL &&
        tree.kind != tfrag3::TFragmentTreeKind::DIRT &&
        tree.kind != tfrag3::TFragmentTreeKind::ICE) {
      continue;
    }
    u32 base = rt->palettes.size() / 8;
    add_palettes(tree.colors, &rt->palettes);
    add_draws(tree.unpacked.vertices, tree.unpacked.indices, tree.draws, tree.use_strips, base);
  }
  for (const auto& tree : level.tie_trees[0]) {
    u32 base = rt->palettes.size() / 8;
    add_palettes(tree.colors, &rt->palettes);
    add_draws(tree.unpacked.vertices, tree.unpacked.indices, tree.static_draws, tree.use_strips,
              base);
  }
  if (rt->tris.empty()) {
    return;
  }

  Builder b;
  b.prims.resize(rt->tris.size());
  for (u32 i = 0; i < rt->tris.size(); i++) {
    const auto& t = rt->tris[i];
    auto& p = b.prims[i];
    p.tri = i;
    float v[3][3];
    for (int k = 0; k < 3; k++) {
      v[0][k] = t.v0[k];
      v[1][k] = t.v0[k] + t.e1[k];
      v[2][k] = t.v0[k] + t.e2[k];
    }
    for (auto& q : v) {
      p.box.grow(q);
    }
    for (int k = 0; k < 3; k++) {
      p.c[k] = (v[0][k] + v[1][k] + v[2][k]) / 3.f;
    }
  }
  b.nodes.reserve(rt->tris.size() / 2);
  b.build(0, b.prims.size());
  // the triangles in the leaves' order
  std::vector<EnhancedRtLevel::Tri> ordered(rt->tris.size());
  for (u32 i = 0; i < b.prims.size(); i++) {
    ordered[i] = rt->tris[b.prims[i].tri];
  }
  rt->tris = std::move(ordered);
  rt->nodes = std::move(b.nodes);
  lg::info("[enhanced] ray tracing {}: {} triangles, {} nodes, {} colors, {:.0f} MB in {:.2f} s",
           name, rt->tris.size(), rt->nodes.size(), rt->palettes.size() / 8,
           (rt->tris.size() * sizeof(EnhancedRtLevel::Tri) +
            rt->nodes.size() * sizeof(EnhancedRtLevel::Node) + rt->palettes.size() * 4) /
               1e6,
           timer.getSeconds());
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  g_rt_levels[name] = std::move(rt);
}

std::unique_ptr<EnhancedRtLevel> enhanced_take_rt(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_rt_mutex);
  auto it = g_rt_levels.find(name);
  if (it == g_rt_levels.end()) {
    return nullptr;
  }
  auto rt = std::move(it->second);
  g_rt_levels.erase(it);
  return rt;
}
