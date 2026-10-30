#include "EnhancedSmoothMerc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/runtime.h"

int g_enhanced_smooth_chars = [] {
  const char* env = std::getenv("JAK_ENHANCED_SMOOTH_CHARS");
  return env ? std::atoi(env) : 0;
}();

namespace {

constexpr u32 kRestart = UINT32_MAX;
// a vertex's copies (fragments repeat them, with rounding) weld within this share of the model's size
constexpr float kWeldGrid = 1.f / 50000.f;
// edges shorter than this share of the model's size aren't worth more triangles
constexpr float kMinEdge = 1.f / 250.f;
// two copies of a vertex with normals further apart than this make a hard edge (about 11 degrees)
constexpr float kSameNormal = 0.98f;
// an edge whose two normals turn this far (about 70 degrees) is a crease of the shape, not a curve
constexpr float kCrease = 0.34f;
// the bulge, as a share of the edge's length (the formula alone reaches 1/4 at silly normals)
constexpr float kMaxBulge = 0.18f;
constexpr size_t kMaxTriangles = 150000;

// lod0 only: past that the game shows lower models too small to matter. No crowds: they're many, and
// far less looked at.
constexpr const char* kCharacters[] = {
    // Jak and Daxter
    "jakb", "jakb-c", "jakb-normal", "jakb-old", "jak-highres", "jakc-highres", "jakc-feet",
    "jakc-scarf", "darkjak-highres", "jak-one", "daxter", "daxter-highres", "sidekick-human",
    // allies
    "ashelin", "ashelin-highres", "baron-highres", "brutter-highres", "crocadog-highres", "errol",
    "errol-highres", "errol-lowres", "jinx", "jinx-highres", "keira-highres", "keira-simple", "kid",
    "kid-highres", "king-highres", "kleever-highres", "kor-highres", "onin-highres", "onin-simple",
    "ottsel-daxpants", "ottsel-leader", "ottsel-surfer", "ottsel-tess", "ottsel-veger",
    "pecker-highres", "pecker-ingame", "samos-highres", "seem-highres", "sig-highres", "tess-highres",
    "torn-highres", "torn-simple", "veger-highres", "youngsamos-highres",
    // common enemies
    "crimson-guard", "crimson-guard-highres", "red-crimson-guard-highres", "kg-grunt", "grunt",
    "city-grunt", "flitter", "city-flitter", "marauder-male", "wlander-male", "wlander-female",
    "roboguard", "roboguard-city", "spydroid", "neo-grenadier", "neo-juicer", "dp-bipedal",
    "kg-hopper", "mantis", "predator"};

bool is_character(const std::string& model_name) {
  const size_t len = model_name.size();
  if (len <= 5 || model_name.compare(len - 5, 5, "-lod0") != 0) {
    return false;
  }
  for (const char* c : kCharacters) {
    if (model_name.compare(0, len - 5, c) == 0) {
      return true;
    }
  }
  return false;
}

using Vec = std::array<float, 3>;
using Tri = std::array<u32, 3>;

Vec pos_of(const tfrag3::MercVertex& v) {
  return {v.pos[0], v.pos[1], v.pos[2]};
}
Vec nrm_of(const tfrag3::MercVertex& v) {
  return {v.normal[0], v.normal[1], v.normal[2]};
}
float dot(const Vec& a, const Vec& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
// unit length, or all zeros if there's nothing to go on
Vec unit(const Vec& a) {
  const float l = std::sqrt(dot(a, a));
  if (!(l > 1e-6f)) {
    return {0, 0, 0};
  }
  return {a[0] / l, a[1] / l, a[2] / l};
}

/*!
 * The triangles of a draw, in the winding GL gives them in its strips (every other one flipped).
 * False if it doesn't look like a normal draw on the level's vertices.
 */
bool read_tris(const tfrag3::MercModelGroup& merc,
               const tfrag3::MercDraw& draw,
               std::vector<Tri>* out) {
  if (draw.no_strip || (size_t)draw.first_index + draw.index_count > merc.indices.size()) {
    return false;  // (no_strip: custom models, already as detailed as their authors wanted)
  }
  u32 n = 0, a = 0, b = 0;
  for (u32 i = 0; i < draw.index_count; i++) {
    const u32 c = merc.indices[draw.first_index + i];
    if (c == kRestart) {
      n = 0;
      continue;
    }
    if (c >= merc.vertices.size()) {
      return false;
    }
    if (n >= 2 && a != b && b != c && a != c) {
      if (n & 1) {
        out->push_back({b, a, c});
      } else {
        out->push_back({a, b, c});
      }
    }
    a = b;
    b = c;
    n++;
  }
  return true;
}

Tri sorted(Tri t) {
  std::sort(t.begin(), t.end());
  return t;
}

/*!
 * An edge of the model as a surface: between two welded vertices (same place, same bones), whatever
 * their UVs and normals in each triangle. There's one midpoint per edge, whichever triangles use it,
 * so the sides of a UV seam or a hard edge stay together.
 */
struct Edge {
  u32 a = 0, b = 0;     // the first triangle's vertices, in the order of their welded ids
  bool hard = false;    // its sides have different normals: straight, in the middle
  bool locked = false;  // not split at all
  // the midpoint
  Vec mid = {0, 0, 0};
  float weights[3] = {0, 0, 0};
  u8 mats[3] = {0, 0, 0};
};

class Smoother {
 public:
  explicit Smoother(tfrag3::MercModelGroup& merc) : m_merc(merc) {}

  // false: nothing was changed
  bool run(tfrag3::MercModel& model, size_t* tris_in, size_t* tris_out);

 private:
  u32 welded(u32 vertex);
  u64 edge_key(u32 a, u32 b) {
    const u64 ga = welded(a), gb = welded(b);
    return ga < gb ? (ga << 32) | gb : (gb << 32) | ga;
  }
  bool is_degenerate(const Tri& t) {
    return welded(t[0]) == welded(t[1]) || welded(t[1]) == welded(t[2]) ||
           welded(t[0]) == welded(t[2]);
  }
  void add_edge(u32 a, u32 b, bool lock);
  void finish_edge(Edge* e);
  u32 midpoint(u32 a, u32 b);
  void put(u32 a, u32 b, u32 c);
  void split(const Tri& t);
  void rewrite(tfrag3::MercDraw* draw, size_t* tris_in, size_t* tris_out);

  tfrag3::MercModelGroup& m_merc;
  float m_size = 0.f;
  std::map<std::array<s32, 9>, u32> m_welded_ids;
  std::unordered_map<u32, u32> m_welded;
  std::unordered_map<u64, Edge> m_edges;
  std::unordered_map<u64, u32> m_midpoints;
  std::vector<u32> m_new_indices;
  u32 m_new_tris = 0;
};

u32 Smoother::welded(u32 vertex) {
  const auto it = m_welded.find(vertex);
  if (it != m_welded.end()) {
    return it->second;
  }
  const auto& v = m_merc.vertices[vertex];
  // in the same place on different bones is two surfaces touching (lips, a sleeve on an arm), which
  // part when they move: they don't weld
  std::array<s32, 9> key;
  const float grid = std::max(m_size * kWeldGrid, 1e-6f);
  for (int i = 0; i < 3; i++) {
    key[i] = (s32)std::lround(v.pos[i] / grid);
  }
  std::array<std::pair<s32, s32>, 3> skin;
  for (int i = 0; i < 3; i++) {
    const bool used = v.weights[i] > 0.f;
    skin[i] = {used ? (s32)v.mats[i] : -1, used ? (s32)std::lround(v.weights[i] * 1024.f) : 0};
  }
  std::sort(skin.begin(), skin.end());
  for (int i = 0; i < 3; i++) {
    key[3 + i * 2] = skin[i].first;
    key[4 + i * 2] = skin[i].second;
  }
  const u32 id = m_welded_ids.emplace(key, (u32)m_welded_ids.size()).first->second;
  m_welded[vertex] = id;
  return id;
}

void Smoother::add_edge(u32 a, u32 b, bool lock) {
  if (welded(a) > welded(b)) {
    std::swap(a, b);
  }
  auto [it, is_new] = m_edges.try_emplace(edge_key(a, b));
  auto& e = it->second;
  if (is_new) {
    e.a = a;
    e.b = b;
  } else {
    const auto& v = m_merc.vertices;
    if (dot(unit(nrm_of(v[a])), unit(nrm_of(v[e.a]))) < kSameNormal ||
        dot(unit(nrm_of(v[b])), unit(nrm_of(v[e.b]))) < kSameNormal) {
      e.hard = true;
    }
  }
  e.locked = e.locked || lock;
}

void Smoother::finish_edge(Edge* e) {
  const auto& a = m_merc.vertices[e->a];
  const auto& b = m_merc.vertices[e->b];
  const Vec pa = pos_of(a), pb = pos_of(b);
  const Vec d = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
  const float len = std::sqrt(dot(d, d));
  if (len < m_size * kMinEdge) {
    e->locked = true;
  }
  if (e->locked) {
    return;
  }

  // the middle of the PN-triangle cubic along this edge, which depends on this edge's two ends and
  // normals alone: (P1 + P2) / 2 - (w12 N1 + w21 N2) / 8, w12 = (P2 - P1).N1, w21 = (P1 - P2).N2.
  // The surface still goes through the game's own vertices, so nothing shrinks or moves away from
  // what the animators saw. (Flipping a normal changes nothing.)
  const Vec n0 = unit(nrm_of(a)), n1 = unit(nrm_of(b));
  Vec bulge = {0, 0, 0};
  const bool no_normal = dot(n0, n0) == 0.f || dot(n1, n1) == 0.f;
  if (!e->hard && !no_normal && dot(n0, n1) >= kCrease) {
    const float w0 = dot(d, n0), w1 = dot(d, n1);
    for (int i = 0; i < 3; i++) {
      bulge[i] = (w1 * n1[i] - w0 * n0[i]) / 8.f;
    }
    const float l = std::sqrt(dot(bulge, bulge));
    if (l > len * kMaxBulge) {
      const float s = len * kMaxBulge / l;
      bulge = {bulge[0] * s, bulge[1] * s, bulge[2] * s};
    }
  }
  for (int i = 0; i < 3; i++) {
    e->mid[i] = (pa[i] + pb[i]) * 0.5f + bulge[i];
  }

  // the bones of both ends, half each, down to the three a vertex can have
  std::array<std::pair<float, int>, 6> bones;
  int count = 0;
  float total = 0.f;
  for (const auto* v : {&a, &b}) {
    for (int i = 0; i < 3; i++) {
      if (!(v->weights[i] > 0.f)) {
        continue;  // (the bone slots of unused weights hold anything)
      }
      total += v->weights[i] * 0.5f;
      int j = 0;
      while (j < count && bones[j].second != v->mats[i]) {
        j++;
      }
      if (j == count) {
        bones[count++] = {0.f, (int)v->mats[i]};
      }
      bones[j].first += v->weights[i] * 0.5f;
    }
  }
  std::sort(bones.begin(), bones.begin() + count, [](const auto& x, const auto& y) {
    return x.first != y.first ? x.first > y.first : x.second < y.second;
  });
  float kept = 0.f;
  for (int i = 0; i < std::min(count, 3); i++) {
    kept += bones[i].first;
  }
  for (int i = 0; i < 3; i++) {
    if (i < count && kept > 0.f) {
      e->weights[i] = bones[i].first * total / kept;
      e->mats[i] = (u8)bones[i].second;
    } else {
      e->weights[i] = 0.f;
      e->mats[i] = count ? (u8)bones[0].second : a.mats[0];
    }
  }
}

/*!
 * The vertex between these two: UVs, colors and normal of this pair, place and bones of their edge.
 * kRestart if the edge isn't split.
 */
u32 Smoother::midpoint(u32 a, u32 b) {
  const auto& e = m_edges.at(edge_key(a, b));
  if (e.locked) {
    return kRestart;
  }
  const u64 key = a < b ? ((u64)a << 32) | b : ((u64)b << 32) | a;
  const auto found = m_midpoints.find(key);
  if (found != m_midpoints.end()) {
    return found->second;
  }
  const tfrag3::MercVertex va = m_merc.vertices[a];  // (copies: the vector grows below)
  const tfrag3::MercVertex vb = m_merc.vertices[b];
  tfrag3::MercVertex m = va;
  for (int i = 0; i < 3; i++) {
    m.pos[i] = e.mid[i];
    m.weights[i] = e.weights[i];
    m.mats[i] = e.mats[i];
  }
  // the normal the two ends would have interpolated to anyway, so the shading stays what it was.
  // At the length of the ends: the merc shader normalizes after the bones
  const Vec na = nrm_of(va), nb = nrm_of(vb);
  Vec sum = {na[0] + nb[0], na[1] + nb[1], na[2] + nb[2]};
  sum = unit(dot(sum, sum) > 1e-6f ? sum : na);
  const float len = (std::sqrt(dot(na, na)) + std::sqrt(dot(nb, nb))) * 0.5f;
  for (int i = 0; i < 3; i++) {
    m.normal[i] = sum[i] * len;
  }
  m.st[0] = (va.st[0] + vb.st[0]) * 0.5f;
  m.st[1] = (va.st[1] + vb.st[1]) * 0.5f;
  for (int i = 0; i < 4; i++) {
    m.rgba[i] = (u8)(((u32)va.rgba[i] + vb.rgba[i] + 1) / 2);
  }
  const u32 idx = (u32)m_merc.vertices.size();
  m_merc.vertices.push_back(m);
  m_midpoints[key] = idx;
  return idx;
}

// a triangle, as its own little strip: the draws stay strips (no_strip means "a custom model" to
// Merc2), for one index more per triangle
void Smoother::put(u32 a, u32 b, u32 c) {
  m_new_indices.insert(m_new_indices.end(), {a, b, c, kRestart});
  m_new_tris++;
}

void Smoother::split(const Tri& tri) {
  Tri t = tri;
  std::array<u32, 3> m = {midpoint(t[0], t[1]), midpoint(t[1], t[2]), midpoint(t[2], t[0])};
  const int splits = (m[0] != kRestart) + (m[1] != kRestart) + (m[2] != kRestart);
  const auto turn = [&] {
    std::rotate(t.begin(), t.begin() + 1, t.end());
    std::rotate(m.begin(), m.begin() + 1, m.end());
  };
  switch (splits) {
    case 0:
      put(t[0], t[1], t[2]);
      break;
    case 1:
      // only the edge from t[0] to t[1] is split
      while (m[0] == kRestart) {
        turn();
      }
      put(t[0], m[0], t[2]);
      put(m[0], t[1], t[2]);
      break;
    case 2:
      // only the edge from t[2] to t[0] isn't
      while (m[2] != kRestart) {
        turn();
      }
      put(m[0], t[1], m[1]);
      put(t[0], m[0], m[1]);
      put(t[0], m[1], t[2]);
      break;
    default:
      put(t[0], m[0], m[2]);
      put(m[0], t[1], m[1]);
      put(m[2], m[1], t[2]);
      put(m[0], m[1], m[2]);
      break;
  }
}

void Smoother::rewrite(tfrag3::MercDraw* draw, size_t* tris_in, size_t* tris_out) {
  std::vector<Tri> tris;
  read_tris(m_merc, *draw, &tris);  // (run checked it)
  m_new_indices.clear();
  m_new_tris = 0;
  u32 kept = 0;
  for (const auto& t : tris) {
    if (!is_degenerate(t)) {
      split(t);
      kept++;
    }
  }
  *tris_in += draw->num_triangles;
  if (m_new_tris == kept) {
    *tris_out += draw->num_triangles;  // nothing was split: the game's own strips will do
    return;
  }
  draw->first_index = (u32)m_merc.indices.size();
  draw->index_count = (u32)m_new_indices.size();
  draw->num_triangles = m_new_tris;
  m_merc.indices.insert(m_merc.indices.end(), m_new_indices.begin(), m_new_indices.end());
  *tris_out += m_new_tris;
}

bool Smoother::run(tfrag3::MercModel& model, size_t* tris_in, size_t* tris_out) {
  // Read and check everything first: nothing changes unless the whole model can be done.
  // Blend shapes and ripple move some vertices in a buffer of the effect's own, with indices of
  // their own (mod_draw). The game switches to it and back at will (a blend shape running or not),
  // so all_draws must look the same as fix_draw + mod_draw: the triangles in fix_draw get split in
  // both, the others in neither.
  std::vector<std::pair<Tri, bool>> tris;  // and if its edges are to stay as they are
  std::vector<Tri> draw_tris;
  for (const auto& effect : model.effects) {
    std::set<Tri> fixed;
    for (const auto& draw : effect.mod.fix_draw) {
      draw_tris.clear();
      if (!read_tris(m_merc, draw, &draw_tris)) {
        return false;
      }
      const bool eye = draw.eye_id != 0xff;
      for (const auto& t : draw_tris) {
        if (!eye) {
          fixed.insert(sorted(t));
        }
        tris.emplace_back(t, eye);
      }
    }
    for (const auto& draw : effect.all_draws) {
      draw_tris.clear();
      if (!read_tris(m_merc, draw, &draw_tris)) {
        return false;
      }
      const bool eye = draw.eye_id != 0xff;
      for (const auto& t : draw_tris) {
        tris.emplace_back(t, eye || (effect.has_mod_draw && !fixed.count(sorted(t))));
      }
    }
  }
  if (tris.empty() || tris.size() > kMaxTriangles) {
    return false;
  }

  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& [t, lock] : tris) {
    for (u32 vi : t) {
      for (int k = 0; k < 3; k++) {
        lo[k] = std::min(lo[k], m_merc.vertices[vi].pos[k]);
        hi[k] = std::max(hi[k], m_merc.vertices[vi].pos[k]);
      }
    }
  }
  const Vec span = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  m_size = std::sqrt(dot(span, span));

  // the edges of the whole model (seams run between effects too), then their midpoints
  for (const auto& [t, lock] : tris) {
    if (!is_degenerate(t)) {
      add_edge(t[0], t[1], lock);
      add_edge(t[1], t[2], lock);
      add_edge(t[2], t[0], lock);
    }
  }
  for (auto& [key, edge] : m_edges) {
    finish_edge(&edge);
  }

  // Merc2 draws all_draws, or fix_draw + mod_draw (which stays as it is). A draw and its copy in
  // fix_draw share their midpoints: those are per pair of vertices.
  for (auto& effect : model.effects) {
    for (auto& draw : effect.all_draws) {
      if (draw.eye_id == 0xff) {
        rewrite(&draw, tris_in, tris_out);
      }
    }
    size_t ignored_in = 0, ignored_out = 0;
    for (auto& draw : effect.mod.fix_draw) {
      if (draw.eye_id == 0xff) {
        rewrite(&draw, &ignored_in, &ignored_out);
      }
    }
  }
  return true;
}

}  // namespace

void enhanced_smooth_characters(tfrag3::Level& level, const std::string& name) {
  if (g_enhanced_smooth_chars <= 0 || g_game_version != GameVersion::Jak3) {
    return;
  }
  for (auto& model : level.merc_data.models) {
    if (!is_character(model.name)) {
      continue;
    }
    // (a replacement: the only strips in it are the game's eyes and face it kept, which stay as is)
    if (!model.effects.empty() && !model.effects[0].all_draws.empty() &&
        model.effects[0].all_draws[0].no_strip) {
      continue;
    }
    size_t tris_in = 0, tris_out = 0;
    Smoother smoother(level.merc_data);
    if (smoother.run(model, &tris_in, &tris_out)) {
      lg::info("[enhanced] smooth: {}: {} {} -> {} triangles", name, model.name, tris_in, tris_out);
    } else {
      lg::info("[enhanced] smooth: {}: {} left as it is", name, model.name);
    }
  }
}
