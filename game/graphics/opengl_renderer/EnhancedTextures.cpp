#include "EnhancedTextures.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "common/log/log.h"
#include "common/versions/versions.h"

#include "game/runtime.h"

#include "third-party/stb_image/stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

struct P3 {
  float x, y, z;
};
P3 sub(P3 a, P3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float dot3(P3 a, P3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

// the squared distance from p to the triangle abc (Ericson, Real-Time Collision Detection 5.1.5)
float dist2_tri(P3 p, P3 a, P3 b, P3 c) {
  const P3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
  auto at = [&](float v, float w) {
    const P3 q = {a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w};
    const P3 d = sub(p, q);
    return dot3(d, d);
  };
  const float d1 = dot3(ab, ap), d2 = dot3(ac, ap);
  if (d1 <= 0 && d2 <= 0) {
    return dot3(ap, ap);
  }
  const P3 bp = sub(p, b);
  const float d3 = dot3(ab, bp), d4 = dot3(ac, bp);
  if (d3 >= 0 && d4 <= d3) {
    return dot3(bp, bp);
  }
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    return at(d1 / (d1 - d3), 0);
  }
  const P3 cp = sub(p, c);
  const float d5 = dot3(ab, cp), d6 = dot3(ac, cp);
  if (d6 >= 0 && d5 <= d6) {
    return dot3(cp, cp);
  }
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    return at(0, d2 / (d2 - d6));
  }
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return at(1 - w, w);
  }
  const float denom = 1.f / (va + vb + vc);
  return at(vb * denom, vc * denom);
}

// each triangle of indices [first, end): strips (restarting at UINT32_MAX) or a list; f(a, b, c, i)
// with i the index of its last vertex in the stream
template <typename F>
void each_tri(const std::vector<u32>& idx, size_t first, size_t end, bool strips, F&& f) {
  end = std::min(end, idx.size());
  if (strips) {
    size_t start = first;
    for (size_t i = first; i < end; i++) {
      if (idx[i] == UINT32_MAX) {
        start = i + 1;
      } else if (i >= start + 2) {
        f(idx[i - 2], idx[i - 1], idx[i], i);
      }
    }
  } else {
    for (size_t i = first; i + 2 < end; i += 3) {
      f(idx[i], idx[i + 1], idx[i + 2], i + 2);
    }
  }
}

// the nearest triangle so far: its kind (0 background, 1 prop, 2 prop in the wind, 3 shrub), level,
// tree, texture, the proto (or wind instance) it's of
struct Hit {
  float d2 = 1e30f;
  int kind = -1;
  size_t level = 0, tree = 0;
  s32 tex = -1;
  u32 owner = 0;
};

// is p in the box of these three, grown by m
bool near_box(P3 p, P3 a, P3 b, P3 c, float m) {
  return p.x >= std::min({a.x, b.x, c.x}) - m && p.x <= std::max({a.x, b.x, c.x}) + m &&
         p.y >= std::min({a.y, b.y, c.y}) - m && p.y <= std::max({a.y, b.y, c.y}) + m &&
         p.z >= std::min({a.z, b.z, c.z}) - m && p.z <= std::max({a.z, b.z, c.z}) + m;
}

P3 vert(const tfrag3::PreloadedVertex& v) {
  return {v.x, v.y, v.z};
}

bool plain_alpha_of(const tfrag3::Texture& tex) {
  // (the PS2's alpha: 128 is opaque; none above it)
  for (u32 px : tex.data) {
    if ((px >> 24) > 0x80) {
      return true;
    }
  }
  return false;
}

std::mutex g_edits_mutex;
bool g_edits_read = false;
std::set<std::string> g_edits;  // "tpage/name"

fs::path edits_dir() {
  return file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] /
         "texture_edits";
}

void read_edits() {
  if (g_edits_read) {
    return;
  }
  g_edits_read = true;
  std::error_code ec;
  if (!fs::exists(edits_dir(), ec)) {
    return;
  }
  for (const auto& page : fs::directory_iterator(edits_dir(), ec)) {
    if (!page.is_directory()) {
      continue;
    }
    for (const auto& f : fs::directory_iterator(page.path(), ec)) {
      if (f.path().extension() == ".png") {
        g_edits.insert(page.path().filename().string() + "/" + f.path().stem().string());
      }
    }
  }
  lg::info("[enhanced] texture edits: {}", g_edits.size());
}

const tfrag3::Texture* find_texture(const std::vector<LevelData*>& levels,
                                    const std::string& tpage,
                                    const std::string& name,
                                    std::vector<GLuint>* gls) {
  const tfrag3::Texture* found = nullptr;
  for (const auto* lev : levels) {
    if (!lev || !lev->level) {
      continue;
    }
    const auto& texs = lev->level->textures;
    for (size_t i = 0; i < texs.size() && i < lev->textures.size(); i++) {
      if (texs[i].debug_name == name && texs[i].debug_tpage_name == tpage) {
        if (!found) {
          found = &texs[i];
        }
        if (gls) {
          gls->push_back(lev->textures[i]);
        }
      }
    }
  }
  return found;
}

}  // namespace

bool enhanced_pick_world_textures(const std::vector<LevelData*>& levels,
                                  const float pos[3],
                                  float margin,
                                  EnhancedTexPick* out) {
  const P3 p = {pos[0], pos[1], pos[2]};
  Hit best;
  for (size_t li = 0; li < levels.size(); li++) {
    const auto* lev = levels[li];
    if (!lev || !lev->level) {
      continue;
    }
    const auto& level = *lev->level;
    // the background
    for (size_t ti = 0; ti < level.tfrag_trees[0].size(); ti++) {
      const auto& tree = level.tfrag_trees[0][ti];
      if (tree.kind == tfrag3::TFragmentTreeKind::WATER || tree.kind == tfrag3::TFragmentTreeKind::INVALID) {
        continue;
      }
      const auto& vs = tree.unpacked.vertices;
      for (size_t d = 0; d < tree.draws.size(); d++) {
        const size_t first = tree.draws[d].unpacked.idx_of_first_idx_in_full_buffer;
        const size_t end = d + 1 < tree.draws.size() ? tree.draws[d + 1].unpacked.idx_of_first_idx_in_full_buffer
                                                     : tree.unpacked.indices.size();
        each_tri(tree.unpacked.indices, first, end, tree.use_strips, [&](u32 a, u32 b, u32 c, size_t) {
          if (a >= vs.size() || b >= vs.size() || c >= vs.size()) {
            return;
          }
          const P3 A = vert(vs[a]), B = vert(vs[b]), C = vert(vs[c]);
          if (!near_box(p, A, B, C, margin)) {
            return;
          }
          const float d2 = dist2_tri(p, A, B, C);
          if (d2 < best.d2) {
            best = {d2, 0, li, ti, tree.draws[d].tree_tex_id, 0};
          }
        });
      }
    }
    // the props (tie): their protos, from the draws' vis groups
    for (size_t ti = 0; ti < level.tie_trees[0].size(); ti++) {
      const auto& tree = level.tie_trees[0][ti];
      const auto& vs = tree.unpacked.vertices;
      for (size_t d = 0; d < tree.static_draws.size(); d++) {
        const auto& draw = tree.static_draws[d];
        const size_t first = draw.unpacked.idx_of_first_idx_in_full_buffer;
        const size_t end = d + 1 < tree.static_draws.size()
                               ? tree.static_draws[d + 1].unpacked.idx_of_first_idx_in_full_buffer
                               : tree.unpacked.indices.size();
        size_t group = 0, group_end = first + (draw.vis_groups.empty() ? 0 : draw.vis_groups[0].num_inds);
        each_tri(tree.unpacked.indices, first, end, tree.use_strips, [&](u32 a, u32 b, u32 c, size_t i) {
          if (a >= vs.size() || b >= vs.size() || c >= vs.size()) {
            return;
          }
          while (group + 1 < draw.vis_groups.size() && i >= group_end) {
            group++;
            group_end += draw.vis_groups[group].num_inds;
          }
          const P3 A = vert(vs[a]), B = vert(vs[b]), C = vert(vs[c]);
          if (!near_box(p, A, B, C, margin)) {
            return;
          }
          const float d2 = dist2_tri(p, A, B, C);
          if (d2 < best.d2) {
            best = {d2, 1, li, ti, draw.tree_tex_id,
                    draw.vis_groups.empty() ? 0u : (u32)draw.vis_groups[group].tie_proto_idx};
          }
        });
      }
      // (in the wind: the proto's vertices, each instance's matrix)
      for (const auto& draw : tree.instanced_wind_draws) {
        size_t at = 0;
        for (const auto& g : draw.instance_groups) {
          const size_t first = at;
          at += g.num;
          if (g.instance_idx >= tree.wind_instance_info.size()) {
            continue;
          }
          const auto& m = tree.wind_instance_info[g.instance_idx].matrix;
          auto world = [&](u32 v) {
            const auto& q = vs[v];
            return P3{m[0].x() * q.x + m[1].x() * q.y + m[2].x() * q.z + m[3].x(),
                      m[0].y() * q.x + m[1].y() * q.y + m[2].y() * q.z + m[3].y(),
                      m[0].z() * q.x + m[1].z() * q.y + m[2].z() * q.z + m[3].z()};
          };
          // (far from its origin: none of it is near)
          const P3 o = {m[3].x(), m[3].y(), m[3].z()};
          const P3 off = sub(p, o);
          if (dot3(off, off) > 60.f * 4096.f * 60.f * 4096.f) {
            continue;
          }
          each_tri(draw.vertex_index_stream, first, at, true, [&](u32 a, u32 b, u32 c, size_t) {
            if (a >= vs.size() || b >= vs.size() || c >= vs.size()) {
              return;
            }
            const P3 A = world(a), B = world(b), C = world(c);
            if (!near_box(p, A, B, C, margin)) {
              return;
            }
            const float d2 = dist2_tri(p, A, B, C);
            if (d2 < best.d2) {
              best = {d2, 2, li, ti, draw.tree_tex_id, g.instance_idx};
            }
          });
        }
      }
    }
    // the shrubs
    for (size_t ti = 0; ti < level.shrub_trees.size(); ti++) {
      const auto& tree = level.shrub_trees[ti];
      const auto& vs = tree.unpacked.vertices;
      for (const auto& draw : tree.static_draws) {
        if (draw.enh_tier) {
          continue;
        }
        each_tri(tree.indices, draw.first_index_index, (size_t)draw.first_index_index + draw.num_indices, true,
                 [&](u32 a, u32 b, u32 c, size_t) {
                   if (a >= vs.size() || b >= vs.size() || c >= vs.size()) {
                     return;
                   }
                   const P3 A = {vs[a].x, vs[a].y, vs[a].z}, B = {vs[b].x, vs[b].y, vs[b].z},
                            C = {vs[c].x, vs[c].y, vs[c].z};
                   if (!near_box(p, A, B, C, margin)) {
                     return;
                   }
                   const float d2 = dist2_tri(p, A, B, C);
                   if (d2 < best.d2) {
                     best = {d2, 3, li, ti, (s32)draw.tree_tex_id, draw.proto_idx};
                   }
                 });
      }
    }
  }
  if (best.kind < 0 || best.d2 > margin * margin) {
    return false;
  }

  // what it belongs to: its textures, the one hit first
  const auto* lev = levels[best.level];
  const auto& level = *lev->level;
  std::vector<s32> ids = {best.tex};
  auto add = [&](s32 t) {
    if (std::find(ids.begin(), ids.end(), t) == ids.end()) {
      ids.push_back(t);
    }
  };
  out->what.clear();
  switch (best.kind) {
    case 0: {
      // (the background has no parts: what's around the point, nearest first)
      out->what = "Background";
      const float r = 3.f * 4096.f;
      std::vector<std::pair<float, s32>> close_by;
      const auto& tree = level.tfrag_trees[0][best.tree];
      const auto& vs = tree.unpacked.vertices;
      for (size_t d = 0; d < tree.draws.size(); d++) {
        const size_t first = tree.draws[d].unpacked.idx_of_first_idx_in_full_buffer;
        const size_t end = d + 1 < tree.draws.size() ? tree.draws[d + 1].unpacked.idx_of_first_idx_in_full_buffer
                                                     : tree.unpacked.indices.size();
        float dmin = 1e30f;
        each_tri(tree.unpacked.indices, first, end, tree.use_strips, [&](u32 a, u32 b, u32 c, size_t) {
          if (a >= vs.size() || b >= vs.size() || c >= vs.size()) {
            return;
          }
          const P3 A = vert(vs[a]), B = vert(vs[b]), C = vert(vs[c]);
          if (near_box(p, A, B, C, r)) {
            dmin = std::min(dmin, dist2_tri(p, A, B, C));
          }
        });
        if (dmin < r * r) {
          close_by.push_back({dmin, tree.draws[d].tree_tex_id});
        }
      }
      std::sort(close_by.begin(), close_by.end());
      for (const auto& [d2, t] : close_by) {
        if (ids.size() < 16) {
          add(t);
        }
      }
      break;
    }
    case 1: {
      const auto& tree = level.tie_trees[0][best.tree];
      out->what = best.owner < tree.proto_names.size() ? "Prop: " + tree.proto_names[best.owner] : "Prop";
      for (const auto& draw : tree.static_draws) {
        for (const auto& g : draw.vis_groups) {
          if (g.tie_proto_idx == best.owner) {
            add(draw.tree_tex_id);
            break;
          }
        }
      }
      break;
    }
    case 2: {
      out->what = "Prop (in the wind)";
      const auto& tree = level.tie_trees[0][best.tree];
      for (const auto& draw : tree.instanced_wind_draws) {
        for (const auto& g : draw.instance_groups) {
          if (g.instance_idx == best.owner) {
            add(draw.tree_tex_id);
            break;
          }
        }
      }
      break;
    }
    case 3: {
      const auto& tree = level.shrub_trees[best.tree];
      out->what = best.owner < tree.proto_names.size() ? "Plant: " + tree.proto_names[best.owner] : "Plant";
      for (const auto& draw : tree.static_draws) {
        if (draw.proto_idx == best.owner && !draw.enh_tier) {
          add((s32)draw.tree_tex_id);
        }
      }
      break;
    }
  }
  out->textures.clear();
  for (s32 t : ids) {
    if (t < 0 || (size_t)t >= level.textures.size() || (size_t)t >= lev->textures.size()) {
      continue;
    }
    const auto& tex = level.textures[t];
    if (std::any_of(out->textures.begin(), out->textures.end(), [&](const EnhancedTexRef& r) {
          return r.tpage == tex.debug_tpage_name && r.name == tex.debug_name;
        })) {
      continue;
    }
    out->textures.push_back({tex.debug_tpage_name, tex.debug_name, lev->textures[t], tex.w, tex.h});
  }
  out->what += fmt::format("  ({})", lev->level->level_name);
  return !out->textures.empty();
}

bool enhanced_world_texture_original(const std::vector<LevelData*>& levels,
                                     const std::string& tpage,
                                     const std::string& name,
                                     std::vector<u8>* rgba,
                                     int* w,
                                     int* h) {
  const auto* tex = find_texture(levels, tpage, name, nullptr);
  if (!tex || tex->data.size() < (size_t)tex->w * tex->h) {
    return false;
  }
  *w = tex->w;
  *h = tex->h;
  const bool plain = plain_alpha_of(*tex);
  rgba->resize((size_t)tex->w * tex->h * 4);
  for (size_t i = 0; i < (size_t)tex->w * tex->h; i++) {
    const u32 px = tex->data[i];
    (*rgba)[i * 4 + 0] = px & 0xff;
    (*rgba)[i * 4 + 1] = (px >> 8) & 0xff;
    (*rgba)[i * 4 + 2] = (px >> 16) & 0xff;
    const u32 a = px >> 24;
    (*rgba)[i * 4 + 3] = plain ? a : std::min(255u, a * 2);
  }
  return true;
}

void enhanced_world_texture_upload(const std::vector<LevelData*>& levels,
                                   const std::string& tpage,
                                   const std::string& name,
                                   const u8* rgba,
                                   int w,
                                   int h) {
  std::vector<GLuint> gls;
  const auto* tex = find_texture(levels, tpage, name, &gls);
  if (!tex) {
    return;
  }
  std::vector<u32> px;
  if (rgba) {
    const bool plain = plain_alpha_of(*tex);
    px.resize((size_t)w * h);
    for (size_t i = 0; i < px.size(); i++) {
      const u32 a = plain ? rgba[i * 4 + 3] : (rgba[i * 4 + 3] + 1u) / 2u;
      px[i] = rgba[i * 4] | (u32)rgba[i * 4 + 1] << 8 | (u32)rgba[i * 4 + 2] << 16 | a << 24;
    }
  } else {
    px = tex->data;
    w = tex->w;
    h = tex->h;
  }
  for (GLuint gl : gls) {
    glBindTexture(GL_TEXTURE_2D, gl);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, px.data());
    glGenerateMipmap(GL_TEXTURE_2D);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

fs::path enhanced_texture_edit_path(const std::string& tpage, const std::string& name) {
  return edits_dir() / tpage / (name + ".png");
}

bool enhanced_has_texture_edit(const std::string& tpage, const std::string& name) {
  std::lock_guard<std::mutex> lock(g_edits_mutex);
  read_edits();
  return g_edits.count(tpage + "/" + name) > 0;
}

void enhanced_note_texture_edit(const std::string& tpage, const std::string& name, bool exists) {
  std::lock_guard<std::mutex> lock(g_edits_mutex);
  read_edits();
  if (exists) {
    g_edits.insert(tpage + "/" + name);
  } else {
    g_edits.erase(tpage + "/" + name);
  }
}

bool enhanced_texture_edit(const tfrag3::Texture& tex, std::vector<u32>& out, int& w, int& h) {
  if (!enhanced_has_texture_edit(tex.debug_tpage_name, tex.debug_name)) {
    return false;
  }
  int n = 0;
  u8* px = stbi_load(enhanced_texture_edit_path(tex.debug_tpage_name, tex.debug_name).string().c_str(), &w,
                     &h, &n, 4);
  if (!px) {
    return false;
  }
  const bool plain = plain_alpha_of(tex);
  out.resize((size_t)w * h);
  for (size_t i = 0; i < out.size(); i++) {
    const u32 a = plain ? px[i * 4 + 3] : (px[i * 4 + 3] + 1u) / 2u;
    out[i] = px[i * 4] | (u32)px[i * 4 + 1] << 8 | (u32)px[i * 4 + 2] << 16 | a << 24;
  }
  stbi_image_free(px);
  return true;
}

namespace {
std::mutex g_remade_mutex;
bool g_remade_read = false;
bool g_remade_relief = false;
std::map<std::string, fs::path> g_remade;          // "tpage/name" (or "_all/name")
std::map<std::string, fs::path> g_remade_by_name;  // "name", from whichever folder
std::map<std::string, GLuint> g_remade_gl;          // pictures put on the GPU, by file

void read_remade() {
  std::lock_guard<std::mutex> lock(g_remade_mutex);
  if (g_remade_read) {
    return;
  }
  g_remade_read = true;
  const auto dir = file_util::get_jak_project_dir() / "custom_assets" /
                   game_version_names[g_game_version] / "texture_remade";
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return;
  }
  for (const auto& page : fs::directory_iterator(dir, ec)) {
    if (!page.is_directory()) {
      continue;
    }
    for (const auto& f : fs::directory_iterator(page.path(), ec)) {
      if (f.path().extension() == ".png") {
        const std::string stem = f.path().stem().string();
        g_remade[page.path().filename().string() + "/" + stem] = f.path();
        g_remade_by_name[stem] = f.path();
        if (stem.size() > 2 && (stem.ends_with("_n") || stem.ends_with("_h"))) {
          g_remade_relief = true;
        }
      }
    }
  }
  lg::info("[enhanced] remade textures: {}", g_remade.size());
}

fs::path remade_path(const std::string& tpage, const std::string& name) {
  read_remade();
  if (tpage.empty()) {
    auto it = g_remade_by_name.find(name);
    return it == g_remade_by_name.end() ? fs::path() : it->second;
  }
  auto it = g_remade.find(tpage + "/" + name);
  if (it == g_remade.end()) {
    it = g_remade.find("_all/" + name);
  }
  return it == g_remade.end() ? fs::path() : it->second;
}

// a PNG as the game's texels. The game's alpha is the PS2's (128 opaque): a picture saved with plain
// 0..255 alpha (any above 128), for a texture in the PS2's, is halved
bool load_remade(const fs::path& path, bool plain_level, std::vector<u32>& out, int& w, int& h) {
  int n = 0;
  u8* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
  if (!px) {
    lg::warn("[enhanced] remade texture {}: can't read it", path.string());
    return false;
  }
  out.resize((size_t)w * h);
  std::memcpy(out.data(), px, out.size() * 4);
  stbi_image_free(px);
  bool over = false;
  for (u32 c : out) {
    over |= (c >> 24) > 0x80;
  }
  if (over && !plain_level) {
    for (u32& c : out) {
      c = (c & 0xffffff) | (((c >> 24) + 1) / 2) << 24;
    }
  }
  return true;
}

GLuint remade_gl(const fs::path& path, bool wrap, bool as_is) {
  const auto it = g_remade_gl.find(path.string());
  if (it != g_remade_gl.end()) {
    return it->second;
  }
  std::vector<u32> px;
  int w = 0, h = 0;
  GLuint gl = 0;
  // (as_is: a relief map's alpha means nothing and its values aren't colors)
  if (load_remade(path, as_is, px, w, h)) {
    glGenTextures(1, &gl);
    glBindTexture(GL_TEXTURE_2D, gl);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, px.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  g_remade_gl[path.string()] = gl;
  return gl;
}
}  // namespace

bool enhanced_remade_any() {
  read_remade();
  return !g_remade.empty();
}

bool enhanced_remade_has_relief() {
  read_remade();
  return g_remade_relief;
}

bool enhanced_remade_texture(const tfrag3::Texture& tex, std::vector<u32>& out, int& w, int& h) {
  const auto path = remade_path(tex.debug_tpage_name, tex.debug_name);
  if (path.empty() || !load_remade(path, plain_alpha_of(tex), out, w, h)) {
    return false;
  }
  // (the same UVs on another shape would be a different picture altogether)
  if ((s64)w * tex.h != (s64)h * tex.w) {
    lg::warn("[enhanced] remade texture {}: {}x{} isn't the shape of the game's {}x{}, not used",
             path.string(), w, h, tex.w, tex.h);
    return false;
  }
  return true;
}

GLuint enhanced_remade_by_name(const std::string& name) {
  const auto path = remade_path("", name);
  return path.empty() ? 0 : remade_gl(path, false, false);
}

std::pair<GLuint, u8> enhanced_remade_relief(const std::string& tpage, const std::string& name) {
  for (u8 mode = 1; mode <= 2; mode++) {
    const auto path = remade_path(tpage, name + (mode == 1 ? "_n" : "_h"));
    if (!path.empty()) {
      glActiveTexture(GL_TEXTURE1);
      const GLuint gl = remade_gl(path, true, true);
      glActiveTexture(GL_TEXTURE0);
      if (gl) {
        return {gl, mode};
      }
    }
  }
  return {0, 0};
}

void enhanced_open_file(const fs::path& path) {
#ifdef _WIN32
  ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
  std::system(fmt::format("open \"{}\" &", path.string()).c_str());
#else
  std::system(fmt::format("xdg-open \"{}\" &", path.string()).c_str());
#endif
}
