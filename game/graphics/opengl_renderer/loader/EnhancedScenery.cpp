#include "EnhancedScenery.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/gltf_util.h"
#include "common/util/json_util.h"
#include "common/versions/versions.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/opengl_renderer/loader/EnhancedVegetation.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/runtime.h"

#include "third-party/meshoptimizer/meshoptimizer.h"
#include "third-party/stb_image/stb_image.h"

float g_enhanced_scenery_dist = [] {
  const char* env = std::getenv("JAK_ENHANCED_SCENERY");
  return env ? std::clamp((float)std::atof(env), 0.f, 200.f) : 60.f;
}();
float g_enhanced_scenery_scale = 1.f;

namespace {

constexpr float kMeter = 4096.f;
constexpr int kFarLods = 1;  // far versions per model, each drawn over one more detailed distance
constexpr float kImpReach = 500.f;  // m: the impostors (past the far versions) reach this far
constexpr float kThinKeep = 0.5f;  // the share of impostors left at kImpReach (thinned from half way)
constexpr int kImpGrid = 8;  // impostor atlas: views per side (tools/blender/grass_variants.py)
constexpr float kFarReach = 1.f + kFarLods;  // so they reach this many times the detailed distance

using Vec3 = math::Vector3f;

u32 pack_normal(const Vec3& n) {
  auto c = [](float f) { return (u32)(std::lround(std::clamp(f, -1.f, 1.f) * 511.f) & 0x3ff); };
  return c(n.x()) | (c(n.y()) << 10) | (c(n.z()) << 20);
}

// a detailed model: CPU side from the loader thread, GL side made on first draw
struct Mesh {
  struct Vertex {
    float x, y, z;  // game units
    float s, t;
    u32 normal;  // GL_INT_2_10_10_10_REV
  };
  std::string file;
  bool ok = false;
  std::vector<Vertex> verts;
  std::vector<u32> indices;
  int tex_w = 0, tex_h = 0;
  std::vector<u8> rgba;
  Vec3 lo, hi;  // bounds, game units
  Vec3 avg;      // its texture's average color (opaque texels), 0..1
  float foot_y = 0;  // where it meets the ground: the bottom of its middle, not of drooping tips
  u64 tex_key = 0;   // the texture's content (models sharing it share one GL texture)
  bool cutout = false;  // its texture has holes (blades, leaves): swayed by the wind, pushed aside
  int far_lod = -1;      // its far version (<file>_lod1.glb, fewer faces), drawn past the detailed distance
  // its impostor (<file>_imp.png): pictures of it from kImpGrid^2 directions, drawn past them all
  int imp_w = 0, imp_h = 0;
  std::vector<u8> imp_rgba, imp_nrm;  // the pictures, their normals (<file>_nrm.png, may be missing)
  GLuint imp_tex = 0, imp_nrm_tex = 0;
  GLuint vao = 0, vbo = 0, ibo = 0, tex = 0;
};

// 64 bytes per copy: the model-to-world matrix's columns, the time of day color in col0.w
struct GpuInstance {
  float col[4][4];
};

// all the copies of one proto drawn with one of its models
struct Batch {
  int mesh = -1;
  DrawMode mode;
  bool foliage = false;
  bool stone = false;  // a stone's model: tagged rock for the post pass (its detail and light)
  float base_rgb[3] = {0.5f, 0.5f, 0.5f};
  float tint[3] = {1, 1, 1};  // the model's texture brought to the proto's colors
  std::vector<GpuInstance> instances;
  std::vector<math::Vector4f> spheres;  // culling: center, radius (game units)
  std::vector<u8> tiers;                // the vegetation density they show at (0 = always)
};

struct SceneryTree {
  std::vector<Batch> batches;
  bool impostors = false;  // every model has one: the game's protos only past kImpReach
};

// the crush map (update_crush)
struct Crush {
  static constexpr int kCells = 128;
  static constexpr float kCell = 0.5f;  // m: so 64 m across
  std::vector<float> rgb;
  int ox = 0, oz = 0;  // its corner, in cells
  bool on = false;
  double last = 0;
  GLuint tex = 0;
};

struct Library {
  std::mutex mutex;
  bool loaded_config = false;
  std::unordered_map<std::string, std::vector<int>> protos;  // proto name -> meshes
  // protos whose models get their own colors: the average they're brought to (rgb)
  std::unordered_map<std::string, std::array<float, 3>> own_colors;
  std::vector<Mesh> meshes;
  std::unordered_map<u64, GLuint> textures;  // by the texture's content
  GLuint instance_vbo = 0;
  // the copies drawn, per batch, per version (the model, its far versions, its impostor)
  std::vector<std::array<std::vector<GpuInstance>, kFarLods + 2>> copies;
  GLuint imp_vao = 0, imp_vbo = 0;  // the impostors' quad
  Crush crush;
};

Library& library() {
  static Library lib;
  return lib;
}

fs::path scenery_dir() {
  return file_util::get_jak_project_dir() / "custom_assets" /
         game_version_names[g_game_version] / "scenery";
}

// every triangle of every node, in the glb's own meters turned into game units
bool load_mesh(Mesh& mesh) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  auto path = (scenery_dir() / mesh.file).string();
  if (!loader.LoadBinaryFromFile(&model, &err, &warn, path)) {
    lg::warn("[enhanced] scenery: can't read {}: {}", path, err);
    return false;
  }
  for (const auto& n : gltf_util::flatten_nodes_from_all_scenes(model)) {
    const auto& node = model.nodes[n.node_idx];
    if (node.mesh < 0) {
      continue;
    }
    for (const auto& prim : model.meshes[node.mesh].primitives) {
      if (prim.mode != TINYGLTF_MODE_TRIANGLES || prim.indices < 0) {
        continue;
      }
      u32 base = mesh.verts.size();
      auto ex = gltf_util::gltf_vertices(model, prim.attributes, n.w_T_node, false, true, "");
      for (size_t i = 0; i < ex.vtx.size(); i++) {
        const auto& v = ex.vtx[i];
        Vec3 nrm = i < ex.normals.size() ? ex.normals[i] : Vec3(0, 1, 0);
        float len = nrm.length();
        mesh.verts.push_back({v.x, v.y, v.z, v.s, v.t, pack_normal(len > 0 ? nrm / len : nrm)});
      }
      auto inds = gltf_util::gltf_index_buffer(model, prim.indices, base);
      mesh.indices.insert(mesh.indices.end(), inds.begin(), inds.end());
      // the base color texture of the first material that has one
      if (mesh.rgba.empty() && prim.material >= 0) {
        const auto& mat = model.materials[prim.material];
        int ti = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (ti >= 0 && model.textures[ti].source >= 0) {
          const auto& img = model.images[model.textures[ti].source];
          if (img.bits == 8 && (img.component == 3 || img.component == 4) && !img.image.empty()) {
            mesh.tex_w = img.width;
            mesh.tex_h = img.height;
            mesh.rgba.resize((size_t)img.width * img.height * 4);
            for (size_t p = 0; p < (size_t)img.width * img.height; p++) {
              for (int c = 0; c < 4; c++) {
                mesh.rgba[p * 4 + c] = c < img.component ? img.image[p * img.component + c] : 255;
              }
            }
          }
        }
      }
    }
  }
  if (mesh.verts.empty() || mesh.indices.empty()) {
    lg::warn("[enhanced] scenery: {} has no triangles", path);
    return false;
  }
  if (mesh.rgba.empty()) {
    mesh.tex_w = mesh.tex_h = 1;
    mesh.rgba = {200, 200, 200, 255};
  }
  // halved down to 2048 at most (a tuft is never that big on screen; Tripo's are 4096)
  while (std::max(mesh.tex_w, mesh.tex_h) > 2048 && mesh.tex_w % 2 == 0 && mesh.tex_h % 2 == 0) {
    int w = mesh.tex_w / 2, h = mesh.tex_h / 2;
    std::vector<u8> half((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        for (int c = 0; c < 4; c++) {
          auto at = [&](int dx, int dy) {
            return (u32)mesh.rgba[((size_t)(2 * y + dy) * mesh.tex_w + 2 * x + dx) * 4 + c];
          };
          half[((size_t)y * w + x) * 4 + c] = (u8)((at(0, 0) + at(1, 0) + at(0, 1) + at(1, 1) + 2) / 4);
        }
      }
    }
    mesh.rgba = std::move(half);
    mesh.tex_w = w;
    mesh.tex_h = h;
  }
  // the variants of one tuft carry the same texture: a key to share one GL texture among them
  {
    u64 h = 1469598103934665603ull ^ ((u64)mesh.tex_w << 32 | (u32)mesh.tex_h);
    for (size_t p = 0; p < mesh.rgba.size(); p += 61) {
      h = (h ^ mesh.rgba[p]) * 1099511628211ull;
    }
    mesh.tex_key = h;
  }
  for (size_t p = 3; p < mesh.rgba.size() && !mesh.cutout; p += 4) {
    mesh.cutout = mesh.rgba[p] < 128;
  }
  {
    double sum[3] = {0, 0, 0}, n = 0;
    for (size_t p = 0; p < mesh.rgba.size(); p += 4) {
      if (mesh.rgba[p + 3] > 128) {
        for (int c = 0; c < 3; c++) {
          sum[c] += mesh.rgba[p + c];
        }
        n++;
      }
    }
    n = std::max(n, 1.0);
    mesh.avg = Vec3(sum[0] / n / 255.0, sum[1] / n / 255.0, sum[2] / n / 255.0);
  }
  mesh.lo = Vec3(1e30f, 1e30f, 1e30f);
  mesh.hi = Vec3(-1e30f, -1e30f, -1e30f);
  for (const auto& v : mesh.verts) {
    mesh.lo = Vec3(std::min(mesh.lo.x(), v.x), std::min(mesh.lo.y(), v.y), std::min(mesh.lo.z(), v.z));
    mesh.hi = Vec3(std::max(mesh.hi.x(), v.x), std::max(mesh.hi.y(), v.y), std::max(mesh.hi.z(), v.z));
  }
  {
    // the lowest point near its axis (the inner third of its width); a tuft's blades bend down at
    // the sides below where it stands
    float cx = (mesh.lo.x() + mesh.hi.x()) * 0.5f, cz = (mesh.lo.z() + mesh.hi.z()) * 0.5f;
    float r = std::max(mesh.hi.x() - mesh.lo.x(), mesh.hi.z() - mesh.lo.z()) * 0.5f / 3.f;
    mesh.foot_y = 1e30f;
    for (const auto& v : mesh.verts) {
      if (std::hypot(v.x - cx, v.z - cz) < r) {
        mesh.foot_y = std::min(mesh.foot_y, v.y);
      }
    }
    if (mesh.foot_y > mesh.hi.y()) {
      mesh.foot_y = mesh.lo.y();
    }
  }
  lg::info("[enhanced] scenery: {} ({} triangles, {}x{} texture)", mesh.file,
           mesh.indices.size() / 3, mesh.tex_w, mesh.tex_h);
  return true;
}

// scenery.json, once (the loader thread holds the library's lock)
void load_config(Library& lib) {
  if (lib.loaded_config) {
    return;
  }
  lib.loaded_config = true;
  auto path = scenery_dir() / "scenery.json";
  if (!fs::exists(path)) {
    return;
  }
  try {
    auto j = parse_commented_json(file_util::read_text_file(path), path.string());
    std::unordered_map<std::string, int> by_file;
    for (auto& [proto, files] : j.items()) {
      if (proto == "own_colors") {
        for (const auto& [p, rgb] : files.items()) {
          lib.own_colors[p] = {rgb[0].get<float>(), rgb[1].get<float>(), rgb[2].get<float>()};
        }
        continue;
      }
      for (const auto& f : files) {
        auto file = f.get<std::string>();
        auto it = by_file.find(file);
        if (it == by_file.end()) {
          Mesh mesh;
          mesh.file = file;
          mesh.ok = load_mesh(mesh);
          it = by_file.emplace(file, (int)lib.meshes.size()).first;
          lib.meshes.push_back(std::move(mesh));
          // its impostor
          auto imp_path = scenery_dir() / (fs::path(file).stem().string() + "_imp.png");
          if (lib.meshes[it->second].ok && fs::exists(imp_path)) {
            int w = 0, h = 0;
            u8* px = stbi_load(imp_path.string().c_str(), &w, &h, nullptr, 4);
            if (px) {
              auto& mesh = lib.meshes[it->second];
              mesh.imp_w = w;
              mesh.imp_h = h;
              mesh.imp_rgba.assign(px, px + (size_t)w * h * 4);
              stbi_image_free(px);
              auto nrm_path = scenery_dir() / (fs::path(file).stem().string() + "_nrm.png");
              int nw = 0, nh = 0;
              u8* npx = fs::exists(nrm_path)
                            ? stbi_load(nrm_path.string().c_str(), &nw, &nh, nullptr, 4)
                            : nullptr;
              if (npx && nw == w && nh == h) {
                mesh.imp_nrm.assign(npx, npx + (size_t)w * h * 4);
              }
              if (npx) {
                stbi_image_free(npx);
              }
            }
          }
          // its far versions, a chain: <file>_lod1.glb, _lod2.glb ...
          int prev = it->second;
          for (int k = 1; k <= kFarLods && lib.meshes[prev].ok; k++) {
            auto far_file = fmt::format("{}_lod{}.glb", fs::path(file).stem().string(), k);
            if (!fs::exists(scenery_dir() / far_file)) {
              break;
            }
            Mesh lod;
            lod.file = far_file;
            lod.ok = load_mesh(lod);
            if (!lod.ok) {
              break;
            }
            lib.meshes[prev].far_lod = (int)lib.meshes.size();
            prev = (int)lib.meshes.size();
            lib.meshes.push_back(std::move(lod));
          }
        }
        if (lib.meshes[it->second].ok) {
          lib.protos[proto].push_back(it->second);
        }
      }
    }
  } catch (const std::exception& e) {
    lg::warn("[enhanced] scenery: bad {}: {}", path.string(), e.what());
  }
}

// a proto's name without the ".mb" the levels give it
std::string proto_key(const std::string& proto) {
  std::string name = proto;
  if (name.size() > 3 && name.compare(name.size() - 3, 3, ".mb") == 0) {
    name.resize(name.size() - 3);
  }
  return name;
}

// the models listed for a proto
const std::vector<int>* models_for(const Library& lib, const std::string& proto) {
  auto it = lib.protos.find(proto_key(proto));
  return it == lib.protos.end() ? nullptr : &it->second;
}

float hash01(u32 a, u32 b, u32 c) {
  u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return (h & 0xffffff) / float(0x1000000);
}

// 3x3 matrix by columns, plus a translation
struct Affine {
  Vec3 c[3];
  Vec3 t;
  Vec3 apply(const Vec3& v) const { return c[0] * v.x() + c[1] * v.y() + c[2] * v.z() + t; }
  Vec3 turn(const Vec3& v) const { return c[0] * v.x() + c[1] * v.y() + c[2] * v.z(); }
  // this after o (this * o)
  Affine after(const Affine& o) const {
    return {{turn(o.c[0]), turn(o.c[1]), turn(o.c[2])}, apply(o.t)};
  }
};

// scale s, turn yaw about +y (as EnhancedVegetation's place()), then move by t
Affine yaw_scale(float yaw, float s, const Vec3& t) {
  float c = std::cos(yaw), n = std::sin(yaw);
  // place(): x' = c x + n z, z' = -n x + c z
  return {{Vec3(c, 0, -n) * s, Vec3(0, 1, 0) * s, Vec3(n, 0, c) * s}, t};
}

// the average color of a level texture where it isn't cut out (alpha up to 128 on these)
Vec3 texture_average(const tfrag3::Texture& tex) {
  double sum[3] = {0, 0, 0}, n = 0;
  for (u32 px : tex.data) {
    if ((px >> 24) > 48) {
      for (int c = 0; c < 3; c++) {
        sum[c] += (px >> (8 * c)) & 0xff;
      }
      n++;
    }
  }
  n = std::max(n, 1.0);
  return Vec3(sum[0] / n / 255.0, sum[1] / n / 255.0, sum[2] / n / 255.0);
}

void prepare_tree(tfrag3::ShrubTree& tree,
                  const std::vector<tfrag3::Texture>& textures,
                  u32 tree_idx,
                  Library& lib,
                  int* replaced) {
  const auto& groups = tree.packed_vertices.instance_groups;
  auto& verts = tree.unpacked.vertices;

  // which listed proto each draw is (by its name), and which groups (instances) they touch
  std::vector<int> group_proto(groups.size(), -1);
  std::vector<u32> group_first(groups.size() + 1, 0);
  for (u32 g = 0; g < groups.size(); g++) {
    group_first[g + 1] = group_first[g] + groups[g].end_vert - groups[g].start_vert;
  }
  const u32 original_verts = group_first.back();
  std::unordered_map<u32, size_t> first_draw_of_proto;
  for (size_t d = 0; d < tree.static_draws.size(); d++) {
    const auto& draw = tree.static_draws[d];
    if (draw.enh_tier != 0 || draw.proto_idx >= tree.proto_names.size() ||
        !models_for(lib, tree.proto_names[draw.proto_idx])) {
      continue;
    }
    first_draw_of_proto.emplace(draw.proto_idx, d);
    for (u32 k = draw.first_index_index; k < draw.first_index_index + draw.num_indices; k++) {
      u32 v = tree.indices[k];
      if (v == UINT32_MAX || v >= original_verts) {
        continue;
      }
      u32 g = std::upper_bound(group_first.begin(), group_first.end(), v) - group_first.begin() - 1;
      group_proto[g] = draw.proto_idx;
    }
  }
  if (first_draw_of_proto.empty()) {
    return;
  }

  auto data = std::make_shared<SceneryTree>();
  std::unordered_map<u64, size_t> batch_of;  // (proto, mesh) -> batch
  // the copy's model fitted into its proto (proto's local space): the foot on the foot, as tall
  auto bounds = [&](u32 g, Vec3& lo, Vec3& hi) {
    const auto& grp = groups[g];
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (u32 v = grp.start_vert; v < grp.end_vert; v++) {
      const auto& pv = tree.packed_vertices.vertices[v];
      lo = Vec3(std::min(lo.x(), pv.x), std::min(lo.y(), pv.y), std::min(lo.z(), pv.z));
      hi = Vec3(std::max(hi.x(), pv.x), std::max(hi.y(), pv.y), std::max(hi.z(), pv.z));
    }
  };
  // width over height: a proto's, a model's (above its foot)
  auto proto_aspect = [&](u32 g) {
    Vec3 lo, hi;
    bounds(g, lo, hi);
    return std::max(std::max(hi.x() - lo.x(), hi.z() - lo.z()), 1.f) / std::max(hi.y() - lo.y(), 1.f);
  };
  auto mesh_aspect = [](const Mesh& mesh) {
    return std::max(std::max(mesh.hi.x() - mesh.lo.x(), mesh.hi.z() - mesh.lo.z()), 1.f) /
           std::max(mesh.hi.y() - mesh.foot_y, 1.f);
  };
  auto fit = [&](u32 g, const Mesh& mesh, float yaw) {
    Vec3 lo, hi;
    bounds(g, lo, hi);
    Vec3 msize = mesh.hi - mesh.lo;
    msize = Vec3(msize.x(), mesh.hi.y() - mesh.foot_y, msize.z());
    float h = std::max(hi.y() - lo.y(), 1.f);
    float w = std::max(std::max(hi.x() - lo.x(), hi.z() - lo.z()), 1.f);
    float s = h / std::max(msize.y(), 1.f);
    // (not much wider than the proto: a flat model fitted by height would sprawl)
    s = std::min(s, 1.6f * w / std::max(std::max(msize.x(), msize.z()), 1.f));
    // (sunk a tenth of its height, as the game sinks its own: no gap on a slope or a bump)
    Vec3 mfoot((mesh.lo.x() + mesh.hi.x()) * 0.5f, mesh.foot_y + 0.1f * msize.y(),
               (mesh.lo.z() + mesh.hi.z()) * 0.5f);
    Vec3 pfoot((lo.x() + hi.x()) * 0.5f, lo.y(), (lo.z() + hi.z()) * 0.5f);
    Affine f = yaw_scale(yaw, s, Vec3(0, 0, 0));
    f.t = pfoot - f.turn(mfoot);
    return f;
  };
  auto add = [&](u32 g, const Affine& place, u8 tier, u32 seed) {
    u32 proto = group_proto[g];
    const auto& meshes = *models_for(lib, tree.proto_names[proto]);
    // a model about as wide for its height as the proto (a bush of tufts where the proto spreads,
    // one tuft where it's narrow: no swelling when it takes over), at random among the close ones
    const float pa = proto_aspect(g);
    float wsum = 0;
    std::vector<float> wts(meshes.size());
    for (size_t i = 0; i < meshes.size(); i++) {
      float d = std::log(mesh_aspect(lib.meshes[meshes[i]]) / pa);
      wts[i] = std::exp(-d * d / 0.08f) + 1e-4f;
      wsum += wts[i];
    }
    float pick = hash01(tree_idx, seed, 1) * wsum;
    int m = meshes.back();
    for (size_t i = 0; i < meshes.size(); i++) {
      pick -= wts[i];
      if (pick <= 0) {
        m = meshes[i];
        break;
      }
    }
    const auto& mesh = lib.meshes[m];
    Affine full = place.after(fit(g, mesh, 6.2831853f * hash01(tree_idx, seed, 2)));
    u64 key = ((u64)proto << 32) | (u32)m;
    auto it = batch_of.find(key);
    if (it == batch_of.end()) {
      Batch b;
      b.mesh = m;
      const auto& draw = tree.static_draws[first_draw_of_proto.at(proto)];
      b.mode = draw.mode;
      // (a solid model, a stone, stays put even standing in for an alpha-tested proto)
      b.foliage = draw.mode.get_at_enable() && mesh.cutout;
      {
        const auto name = proto_key(tree.proto_names[proto]);
        b.stone = !mesh.cutout && (name.find("pebble") != std::string::npos ||
                                   name.find("rock") != std::string::npos ||
                                   name.find("stone") != std::string::npos);
      }
      // the proto's own vertex color (baked light), averaged
      const auto& grp = groups[g];
      float sum[3] = {0, 0, 0};
      for (u32 v = grp.start_vert; v < grp.end_vert; v++) {
        for (int c = 0; c < 3; c++) {
          sum[c] += tree.packed_vertices.vertices[v].rgba[c];
        }
      }
      for (int c = 0; c < 3; c++) {
        b.base_rgb[c] = sum[c] / std::max(1u, grp.end_vert - grp.start_vert) / 255.f;
      }
      // the model's colors toward the proto's texture (a straw-pale grass stays straw-pale),
      // unless it keeps its own (stones: the game's are dull grey-green)
      auto own = lib.own_colors.find(proto_key(tree.proto_names[proto]));
      if (own != lib.own_colors.end()) {
        for (int c = 0; c < 3; c++) {
          b.tint[c] = std::clamp(own->second[c] / std::max(mesh.avg[c], 0.02f), 0.2f, 2.5f);
        }
        // (and the baked light's brightness only: its grey-green hue turned them grey)
        const float lum = 0.3f * b.base_rgb[0] + 0.59f * b.base_rgb[1] + 0.11f * b.base_rgb[2];
        for (int c = 0; c < 3; c++) {
          b.base_rgb[c] = lum;
        }
      } else if (draw.tree_tex_id < textures.size()) {
        Vec3 want = texture_average(textures[draw.tree_tex_id]);
        for (int c = 0; c < 3; c++) {
          b.tint[c] = std::clamp(want[c] / std::max(mesh.avg[c], 0.02f), 0.4f, 2.5f);
        }
      }
      it = batch_of.emplace(key, data->batches.size()).first;
      data->batches.push_back(std::move(b));
    }
    auto& b = data->batches[it->second];
    GpuInstance inst;
    for (int c = 0; c < 3; c++) {
      inst.col[c][0] = full.c[c].x();
      inst.col[c][1] = full.c[c].y();
      inst.col[c][2] = full.c[c].z();
      inst.col[c][3] = 0;
    }
    inst.col[0][3] = groups[g].color_index;
    inst.col[3][0] = full.t.x();
    inst.col[3][1] = full.t.y();
    inst.col[3][2] = full.t.z();
    inst.col[3][3] = 1;
    b.instances.push_back(inst);
    Vec3 center = full.apply((mesh.lo + mesh.hi) * 0.5f);
    float scale = std::max({full.c[0].length(), full.c[1].length(), full.c[2].length()});
    b.spheres.emplace_back(center.x(), center.y(), center.z(),
                           scale * (mesh.hi - mesh.lo).length() * 0.5f);
    b.tiers.push_back(tier);
    (*replaced)++;
  };

  // the level's own copies
  for (u32 g = 0; g < groups.size(); g++) {
    if (group_proto[g] < 0) {
      continue;
    }
    const auto& m = tree.packed_vertices.matrices[groups[g].matrix_idx];
    Affine place{{Vec3(m[0].x(), m[0].y(), m[0].z()), Vec3(m[1].x(), m[1].y(), m[1].z()),
                  Vec3(m[2].x(), m[2].y(), m[2].z())},
                 Vec3(m[3].x(), m[3].y(), m[3].z())};
    add(g, place, 0, g);
    for (u32 v = group_first[g]; v < group_first[g + 1]; v++) {
      verts[v].pad2 = 1;
    }
  }
  // and the vegetation's copies of them: turned and scaled around the source's foot, set down
  for (size_t k = 0; k < tree.enh_copies.size(); k++) {
    const auto& cp = tree.enh_copies[k];
    if (cp.src_first >= original_verts) {
      continue;
    }
    u32 g = std::upper_bound(group_first.begin(), group_first.end(), cp.src_first) -
            group_first.begin() - 1;
    if (group_proto[g] < 0 || group_first[g] != cp.src_first) {
      continue;
    }
    const auto& m = tree.packed_vertices.matrices[groups[g].matrix_idx];
    Affine src{{Vec3(m[0].x(), m[0].y(), m[0].z()), Vec3(m[1].x(), m[1].y(), m[1].z()),
                Vec3(m[2].x(), m[2].y(), m[2].z())},
               Vec3(m[3].x(), m[3].y(), m[3].z())};
    Affine move = yaw_scale(cp.yaw, cp.scale, Vec3(cp.x, cp.y, cp.z));
    move.t = move.t - move.turn(Vec3(cp.cx, cp.base, cp.cz));
    add(g, move.after(src), cp.tier, groups.size() + k);
    for (u32 v = cp.offset; v < cp.offset + (cp.src_end - cp.src_first) && v < verts.size(); v++) {
      verts[v].pad2 = 1;
    }
  }
  data->impostors = !data->batches.empty();
  for (const auto& b : data->batches) {
    data->impostors = data->impostors && !lib.meshes[b.mesh].imp_rgba.empty();
  }
  tree.enh_scenery = data;
}

void upload_cutout(const std::vector<u8>& rgba, int w, int h);

void make_gl(Mesh& mesh, Library& lib) {
  if (!lib.instance_vbo) {
    glGenBuffers(1, &lib.instance_vbo);
  }
  glGenVertexArrays(1, &mesh.vao);
  glBindVertexArray(mesh.vao);
  glGenBuffers(1, &mesh.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
  glBufferData(GL_ARRAY_BUFFER, mesh.verts.size() * sizeof(Mesh::Vertex), mesh.verts.data(),
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Mesh::Vertex),
                        (void*)offsetof(Mesh::Vertex, x));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Mesh::Vertex),
                        (void*)offsetof(Mesh::Vertex, s));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_INT_2_10_10_10_REV, GL_TRUE, sizeof(Mesh::Vertex),
                        (void*)offsetof(Mesh::Vertex, normal));
  glBindBuffer(GL_ARRAY_BUFFER, lib.instance_vbo);
  for (int c = 0; c < 4; c++) {
    glEnableVertexAttribArray(3 + c);
    glVertexAttribPointer(3 + c, 4, GL_FLOAT, GL_FALSE, sizeof(GpuInstance),
                          (void*)(sizeof(float) * 4 * c));
    glVertexAttribDivisor(3 + c, 1);
  }
  glGenBuffers(1, &mesh.ibo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(u32), mesh.indices.data(),
               GL_STATIC_DRAW);
  glBindVertexArray(0);

  auto shared = lib.textures.find(mesh.tex_key);
  if (shared != lib.textures.end()) {
    mesh.tex = shared->second;
    mesh.rgba = {};
    return;
  }
  glGenTextures(1, &mesh.tex);
  lib.textures.emplace(mesh.tex_key, mesh.tex);
  glBindTexture(GL_TEXTURE_2D, mesh.tex);
  upload_cutout(mesh.rgba, mesh.tex_w, mesh.tex_h);
  mesh.rgba = {};  // (on the GPU now)
}

// A cutout texture with its mipmaps made here: each level's alpha scaled so as much of it passes
// the cutout as of the full size one. Plain averaging lets thin blades fade below the cutout a
// few levels down, so far grass thins out and vanishes (Castano, "Computing alpha mipmaps").
void upload_cutout(const std::vector<u8>& rgba, int w, int h) {
  constexpr float kCut = 0.5f;  // (about where the shrubs' alpha test cuts the texture's alpha)
  auto coverage = [&](const std::vector<u8>& px, float scale) {
    size_t n = 0;
    for (size_t p = 3; p < px.size(); p += 4) {
      n += px[p] * scale > kCut * 255.f;
    }
    return (float)n / std::max<size_t>(px.size() / 4, 1);
  };
  const float want = coverage(rgba, 1.f);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  std::vector<u8> level = rgba, next;
  int level_w = w, level_h = h;
  // the box filter runs on the unscaled levels; only what's uploaded gets its alpha scaled
  for (int mip = 1; level_w > 1 || level_h > 1; mip++) {
    int nw = std::max(level_w / 2, 1), nh = std::max(level_h / 2, 1);
    next.assign((size_t)nw * nh * 4, 0);
    for (int y = 0; y < nh; y++) {
      for (int x = 0; x < nw; x++) {
        for (int c = 0; c < 4; c++) {
          u32 sum = 0;
          for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
              int sx = std::min(2 * x + dx, level_w - 1), sy = std::min(2 * y + dy, level_h - 1);
              sum += level[((size_t)sy * level_w + sx) * 4 + c];
            }
          }
          next[((size_t)y * nw + x) * 4 + c] = (u8)((sum + 2) / 4);
        }
      }
    }
    level.swap(next);
    level_w = nw;
    level_h = nh;
    // the alpha scale that keeps the coverage (bisection: coverage grows with the scale)
    float lo = 0.f, hi = 8.f;
    for (int it = 0; it < 12; it++) {
      float mid = 0.5f * (lo + hi);
      (coverage(level, mid) < want ? lo : hi) = mid;
    }
    std::vector<u8> scaled = level;
    for (size_t p = 3; p < scaled.size(); p += 4) {
      scaled[p] = (u8)std::min(255.f, scaled[p] * hi + 0.5f);
    }
    glTexImage2D(GL_TEXTURE_2D, mip, GL_RGBA, level_w, level_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 scaled.data());
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

// The crush map: a square of ground around Jak where whoever walks or drives through the grass
// leaves it lying, bent away from their path, rising back over a few seconds. Each cell holds the
// bend (direction x amount) and the height it was made at (m; grass far above or below isn't
// touched). Follows Jak a cell at a time; updated once a frame.
void update_crush(Library& lib) {
  auto& c = lib.crush;
  constexpr int N = Crush::kCells;
  const float cell = Crush::kCell * kMeter;
  using clock = std::chrono::steady_clock;
  const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
  if (now - c.last < 0.004) {
    return;  // (once a frame: every shrub tree draws)
  }
  const float dt = c.last > 0 ? (float)std::min(now - c.last, 0.1) : 0.f;
  c.last = now;
  const auto& pl = g_enhanced_player;
  if (!pl.valid || !g_enhanced_wind.push) {
    c.on = false;
    return;
  }
  if (c.rgb.empty()) {
    c.rgb.assign(N * N * 3, 0.f);
  }
  // follow Jak: the corner snapped to whole cells, the grid moved along
  int ox = (int)std::floor(pl.pos[0] / cell) - N / 2, oz = (int)std::floor(pl.pos[2] / cell) - N / 2;
  if (!c.on || ox != c.ox || oz != c.oz) {
    std::vector<float> moved(N * N * 3, 0.f);
    if (c.on) {
      for (int z = 0; z < N; z++) {
        for (int x = 0; x < N; x++) {
          int sx = x + ox - c.ox, sz = z + oz - c.oz;
          if (sx >= 0 && sx < N && sz >= 0 && sz < N) {
            for (int k = 0; k < 3; k++) {
              moved[(z * N + x) * 3 + k] = c.rgb[(sz * N + sx) * 3 + k];
            }
          }
        }
      }
    }
    c.rgb.swap(moved);
    c.ox = ox;
    c.oz = oz;
    c.on = true;
  }
  // rising back
  const float keep = std::exp(-dt / 2.5f);
  for (int p = 0; p < N * N; p++) {
    c.rgb[p * 3] *= keep;
    c.rgb[p * 3 + 1] *= keep;
  }
  // pressed where someone is now: away from them, as hard as the live push
  auto stamp = [&](const float* pos, float radius) {
    int r = (int)std::ceil(radius / cell);
    int cx = (int)std::floor(pos[0] / cell) - ox, cz = (int)std::floor(pos[2] / cell) - oz;
    for (int z = std::max(cz - r, 0); z <= std::min(cz + r, N - 1); z++) {
      for (int x = std::max(cx - r, 0); x <= std::min(cx + r, N - 1); x++) {
        float dx = (ox + x + 0.5f) * cell - pos[0], dz = (oz + z + 0.5f) * cell - pos[2];
        float d = std::hypot(dx, dz);
        float t = std::clamp((d - radius * 0.25f) / (radius * 0.75f), 0.f, 1.f);
        float amount = 1.f - t * t * (3.f - 2.f * t);
        float* v = &c.rgb[(z * N + x) * 3];
        if (amount > std::hypot(v[0], v[1])) {
          v[0] = d > 1.f ? dx / d * amount : amount;
          v[1] = d > 1.f ? dz / d * amount : 0.f;
          v[2] = pos[1] / kMeter;
        }
      }
    }
  };
  stamp(pl.pos, g_enhanced_wind.push_radius * kMeter);
  const auto ps = enhanced_pushers_now();
  for (int i = 0; i < ps.count; i++) {
    stamp(ps.pos[i], ps.pos[i][3]);
  }
  if (!c.tex) {
    glGenTextures(1, &c.tex);
    glBindTexture(GL_TEXTURE_2D, c.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, c.tex);
  // (rows along z: the shader's v is z)
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, N, N, 0, GL_RGB, GL_FLOAT, c.rgb.data());
}

// the impostor's atlas, and the quad all impostors share (its corners; the copies' columns from
// the instance buffer, as the models)
void make_imp_gl(Mesh& mesh, Library& lib) {
  if (!lib.imp_vao) {
    if (!lib.instance_vbo) {
      glGenBuffers(1, &lib.instance_vbo);
    }
    const float corners[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &lib.imp_vao);
    glBindVertexArray(lib.imp_vao);
    glGenBuffers(1, &lib.imp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, lib.imp_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, lib.instance_vbo);
    for (int c = 0; c < 4; c++) {
      glEnableVertexAttribArray(3 + c);
      glVertexAttribPointer(3 + c, 4, GL_FLOAT, GL_FALSE, sizeof(GpuInstance),
                            (void*)(sizeof(float) * 4 * c));
      glVertexAttribDivisor(3 + c, 1);
    }
    glBindVertexArray(0);
  }
  glGenTextures(1, &mesh.imp_tex);
  glBindTexture(GL_TEXTURE_2D, mesh.imp_tex);
  upload_cutout(mesh.imp_rgba, mesh.imp_w, mesh.imp_h);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (!mesh.imp_nrm.empty()) {
    glGenTextures(1, &mesh.imp_nrm_tex);
    glBindTexture(GL_TEXTURE_2D, mesh.imp_nrm_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mesh.imp_w, mesh.imp_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 mesh.imp_nrm.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  mesh.imp_rgba = {};  // (on the GPU now)
  mesh.imp_nrm = {};
}

}  // namespace

void enhanced_prepare_scenery(tfrag3::Level& level, const std::string& name) {
  if (level.shrub_trees.empty()) {
    return;
  }
  // JAK_ENHANCED_SCENERY_LIST: the level's shrub protos and how many draws use each (to pick
  // which to replace in scenery.json)
  static const bool list = std::getenv("JAK_ENHANCED_SCENERY_LIST") != nullptr;
  if (list) {
    std::map<std::string, int> uses;
    for (const auto& tree : level.shrub_trees) {
      for (const auto& draw : tree.static_draws) {
        if (draw.proto_idx < tree.proto_names.size()) {
          uses[tree.proto_names[draw.proto_idx]]++;
        }
      }
    }
    for (const auto& [proto, n] : uses) {
      lg::info("[enhanced] scenery list: {}: {} ({} draws)", name, proto, n);
    }
  }
  auto& lib = library();
  std::lock_guard<std::mutex> lock(lib.mutex);
  load_config(lib);
  if (lib.protos.empty()) {
    return;
  }
  int replaced = 0;
  for (u32 t = 0; t < level.shrub_trees.size(); t++) {
    prepare_tree(level.shrub_trees[t], level.textures, t, lib, &replaced);
  }
  if (replaced) {
    lg::info("[enhanced] scenery: {}: {} copies get a detailed model", name, replaced);
  }
}

math::Vector2f enhanced_scenery_fade(const tfrag3::ShrubTree& tree) {
  if (!tree.enh_scenery || g_enhanced_scenery_dist <= 0.f) {
    return math::Vector2f(0, 0);
  }
  // the game's protos only where the models end: with their far pictures (impostors) out to
  // kImpReach, where the protos are a few pixels; else the far versions' reach (dithered: the two
  // differ, up close it showed)
  const auto& data = *static_cast<const SceneryTree*>(tree.enh_scenery.get());
  float end = data.impostors ? kImpReach * kMeter
                             : g_enhanced_scenery_dist * g_enhanced_scenery_scale * kFarReach * kMeter;
  return math::Vector2f(end * 0.85f, end);
}

void enhanced_draw_scenery(const tfrag3::ShrubTree& tree,
                           const TfragRenderSettings& settings,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  if (!tree.enh_scenery || g_enhanced_scenery_dist <= 0.f) {
    return;
  }
  auto& lib = library();
  std::unique_lock<std::mutex> lock(lib.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;  // (the loader is reading a model for a level coming in: the game's protos for now)
  }
  const auto& data = *static_cast<const SceneryTree*>(tree.enh_scenery.get());
  // each version reaches one detailed distance further (the impostor: to kImpReach); a copy near
  // the end of its version's reach swaps to the next over the last part of it (both drawn,
  // complementary pixels: shrub.frag's enh_xfade)
  const float step = g_enhanced_scenery_dist * g_enhanced_scenery_scale * kMeter;
  const float swap = step * 0.2f;
  const float far_end = data.impostors ? kImpReach * kMeter : step * kFarReach;
  const float thin_from = std::max(far_end * 0.5f, (kFarLods + 1.5f) * step);
  const auto& cam = settings.camera.trans;
  auto fade = enhanced_scenery_fade(tree);
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  const float now = std::chrono::duration<float>(clock::now() - start).count();

  // the copies to draw: per batch, per version (the far versions, then the impostor last)
  constexpr int kImp = kFarLods + 1;
  lib.copies.resize(data.batches.size());
  bool any = false;
  for (size_t bi = 0; bi < data.batches.size(); bi++) {
    const auto& b = data.batches[bi];
    const auto& mesh = lib.meshes[b.mesh];
    auto& out = lib.copies[bi];
    for (auto& c : out) {
      c.clear();
    }
    // the last version this model has: its impostor, else its last far version
    int last = mesh.imp_rgba.empty() && !mesh.imp_tex ? 0 : kImp;
    if (!last) {
      for (int k = 0, m = b.mesh; k < kFarLods && lib.meshes[m].far_lod >= 0; k++) {
        m = lib.meshes[m].far_lod;
        last = k + 1;
      }
    }
    for (size_t i = 0; i < b.instances.size(); i++) {
      const auto& s = b.spheres[i];
      float dx = s.x() - cam.x(), dy = s.y() - cam.y(), dz = s.z() - cam.z();
      float d2 = dx * dx + dy * dy + dz * dz;
      float reach = far_end + s.w();
      if (d2 > reach * reach || b.tiers[i] > g_enhanced_vegetation ||
          !sphere_in_view_ref(s, settings.camera.planes)) {
        continue;
      }
      float d = std::sqrt(d2);
      int k = std::min((int)(d / step), last);
      GpuInstance inst = b.instances[i];
      float into = d - (k + 1) * step + swap;  // how far into the swap to the next version
      if (k < last && into > 0.f) {
        float f = std::min(into / swap, 1.f);
        inst.col[1][3] = f;
        inst.col[2][3] = -1.f;
        out[k].push_back(inst);
        inst.col[2][3] = 1.f;
        out[k + 1].push_back(inst);
      } else if (k == kImp && d > thin_from) {
        // far out fewer copies (the ones a stable hash ranks last go first, fading out over a
        // band): a few pixels each, most of them are just cost there
        float keep = 1.f - (1.f - kThinKeep) * std::min((d - thin_from) / (far_end - thin_from), 1.f);
        float shown = std::clamp((keep - hash01((u32)bi, (u32)i, 7)) / 0.1f, 0.f, 1.f);
        if (shown <= 0.f) {
          continue;
        }
        if (shown < 1.f) {
          inst.col[1][3] = 1.f - shown;
          inst.col[2][3] = -1.f;
        }
        out[k].push_back(inst);
      } else {
        out[k].push_back(inst);
      }
      any = true;
    }
  }
  if (!any) {
    return;
  }

  // the models and their far versions
  bool setup = false;
  auto& sh = render_state->shaders[ShaderId::SHRUB_INST];
  for (size_t bi = 0; bi < data.batches.size(); bi++) {
    const auto& b = data.batches[bi];
    auto& mesh = lib.meshes[b.mesh];
    const auto& out = lib.copies[bi];
    // the model and its far versions (the last one stands in for any missing)
    std::array<Mesh*, kFarLods + 1> lods;
    lods[0] = &mesh;
    bool some = !out[0].empty();
    for (int k = 1; k <= kFarLods; k++) {
      lods[k] = lods[k - 1]->far_lod >= 0 ? &lib.meshes[lods[k - 1]->far_lod] : lods[k - 1];
      some = some || !out[k].empty();
    }
    if (!some) {
      continue;
    }
    for (int k = 0; k <= kFarLods; k++) {
      if (!out[k].empty() && !lods[k]->vao) {
        make_gl(*lods[k], lib);
      }
    }
    if (!setup) {
      setup = true;
      first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB_INST);
      glUniform2f(glGetUniformLocation(sh.id(), "enh_lod"), fade.x(), fade.y());
      glUniform1f(glGetUniformLocation(sh.id(), "enh_wet"), g_enhanced_wind.wet);
      glUniform1i(glGetUniformLocation(sh.id(), "tex_T10"), 10);
      // wind and the push of Jak and whoever is near him, as the shrubs
      glUniform1f(glGetUniformLocation(sh.id(), "enh_time"), now);
      const auto& pl = g_enhanced_player;
      glUniform4f(glGetUniformLocation(sh.id(), "enh_player"), pl.pos[0], pl.pos[1], pl.pos[2],
                  pl.valid && g_enhanced_wind.push ? g_enhanced_wind.push_radius * kMeter : 0.f);
      const auto ps = enhanced_pushers_now();
      int pushers = g_enhanced_wind.push ? ps.count : 0;
      glUniform1i(glGetUniformLocation(sh.id(), "enh_pusher_count"), pushers);
      if (pushers > 0) {
        glUniform4fv(glGetUniformLocation(sh.id(), "enh_pushers"), pushers, &ps.pos[0][0]);
      }
      // where the grass still lies
      update_crush(lib);
      const auto& cr = lib.crush;
      const float cell = Crush::kCell * kMeter;
      glUniform4f(glGetUniformLocation(sh.id(), "enh_crush"), cr.ox * cell, cr.oz * cell,
                  Crush::kCells * cell, cr.on && cr.tex ? 1.f : 0.f);
      glUniform1i(glGetUniformLocation(sh.id(), "tex_T2"), 2);
      glActiveTexture(GL_TEXTURE2);
      glBindTexture(GL_TEXTURE_2D, cr.tex);
      glActiveTexture(GL_TEXTURE0);
    }
    glUniform3f(glGetUniformLocation(sh.id(), "enh_base_rgb"), b.base_rgb[0] * b.tint[0],
                b.base_rgb[1] * b.tint[1], b.base_rgb[2] * b.tint[2]);
    glUniform3f(glGetUniformLocation(sh.id(), "enh_model_avg"), mesh.avg.x(), mesh.avg.y(),
                mesh.avg.z());
    glUniform2f(glGetUniformLocation(sh.id(), "enh_tip"), mesh.lo.y(),
                std::max(mesh.hi.y() - mesh.lo.y(), 1.f));
    glUniform1f(glGetUniformLocation(sh.id(), "enh_foliage"), b.foliage ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(sh.id(), "enh_stone"), b.stone ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(sh.id(), "enh_wind"),
                b.foliage && g_enhanced_wind.enabled ? g_enhanced_wind.strength : 0.f);
    setup_tfrag_shader(render_state, b.mode, ShaderId::SHRUB_INST);

    glDisable(GL_PRIMITIVE_RESTART);
    glActiveTexture(GL_TEXTURE0);
    for (int k = 0; k <= kFarLods; k++) {
      const auto& m = *lods[k];
      const auto& copies = out[k];
      if (copies.empty()) {
        continue;
      }
      glBindVertexArray(m.vao);
      glBindBuffer(GL_ARRAY_BUFFER, lib.instance_vbo);
      glBufferData(GL_ARRAY_BUFFER, copies.size() * sizeof(GpuInstance), copies.data(),
                   GL_STREAM_DRAW);
      glBindTexture(GL_TEXTURE_2D, m.tex);
      glDrawElementsInstanced(GL_TRIANGLES, m.indices.size(), GL_UNSIGNED_INT, nullptr,
                              copies.size());
      prof.add_draw_call();
      prof.add_tri(m.indices.size() / 3 * copies.size());
    }
    glEnable(GL_PRIMITIVE_RESTART);
  }

  // the impostors: one quad a copy
  bool imp_setup = false;
  auto& ish = render_state->shaders[ShaderId::SHRUB_IMP];
  for (size_t bi = 0; bi < data.batches.size(); bi++) {
    const auto& b = data.batches[bi];
    auto& mesh = lib.meshes[b.mesh];
    const auto& copies = lib.copies[bi][kImp];
    if (copies.empty()) {
      continue;
    }
    if (!mesh.imp_tex) {
      make_imp_gl(mesh, lib);
    }
    if (!imp_setup) {
      imp_setup = true;
      first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB_IMP);
      glUniform2f(glGetUniformLocation(ish.id(), "enh_lod"), fade.x(), fade.y());
      glUniform1f(glGetUniformLocation(ish.id(), "enh_wet"), g_enhanced_wind.wet);
      glUniform1i(glGetUniformLocation(ish.id(), "tex_T10"), 10);
      glUniform1f(glGetUniformLocation(ish.id(), "enh_time"), now);
      glUniform1f(glGetUniformLocation(ish.id(), "enh_imp_grid"), (float)kImpGrid);
      glUniform1i(glGetUniformLocation(ish.id(), "tex_T1"), 1);
    }
    glUniform3f(glGetUniformLocation(ish.id(), "enh_base_rgb"), b.base_rgb[0] * b.tint[0],
                b.base_rgb[1] * b.tint[1], b.base_rgb[2] * b.tint[2]);
    Vec3 center = (mesh.lo + mesh.hi) * 0.5f;
    glUniform4f(glGetUniformLocation(ish.id(), "enh_imp"), center.x(), center.y(), center.z(),
                (mesh.hi - mesh.lo).length() * 0.5f);
    glUniform1f(glGetUniformLocation(ish.id(), "enh_foliage"), b.foliage ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(ish.id(), "enh_stone"), b.stone ? 1.f : 0.f);
    glUniform2f(glGetUniformLocation(ish.id(), "enh_tip"), mesh.lo.y(),
                std::max(mesh.hi.y() - mesh.lo.y(), 1.f));
    glUniform1f(glGetUniformLocation(ish.id(), "enh_wind"),
                b.foliage && g_enhanced_wind.enabled ? g_enhanced_wind.strength : 0.f);
    glUniform1i(glGetUniformLocation(ish.id(), "enh_imp_normals"), mesh.imp_nrm_tex ? 1 : 0);
    setup_tfrag_shader(render_state, b.mode, ShaderId::SHRUB_IMP);

    glBindVertexArray(lib.imp_vao);
    glBindBuffer(GL_ARRAY_BUFFER, lib.instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, copies.size() * sizeof(GpuInstance), copies.data(),
                 GL_STREAM_DRAW);
    if (mesh.imp_nrm_tex) {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, mesh.imp_nrm_tex);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mesh.imp_tex);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, copies.size());
    prof.add_draw_call();
    prof.add_tri(2 * copies.size());
  }
  glBindVertexArray(0);
}

GLuint enhanced_crush_map(float out[4]) {
  auto& lib = library();
  update_crush(lib);
  const auto& cr = lib.crush;
  const float cell = Crush::kCell * kMeter;
  out[0] = cr.ox * cell;
  out[1] = cr.oz * cell;
  out[2] = Crush::kCells * cell;
  out[3] = cr.on && cr.tex ? 1.f : 0.f;
  return cr.on ? cr.tex : 0;
}

namespace {
// the objects placed by hand (enhanced_set_placed), drawn by the first shrub tree near the camera
struct PlacedSet {
  std::vector<EnhancedPlaced> list;
  std::unordered_map<std::string, int> mesh_of;  // model -> its mesh in the library (-1: broken)
  std::unordered_map<std::string, std::shared_future<std::shared_ptr<Mesh>>> loading;
  u64 drawn_frame = 0;
  // each object's light: the nearest vertex of the tree drawing it (its time of day color, its
  // baked light), worked out again when the tree or the place changes
  struct Lit {
    const tfrag3::ShrubTree* tree = nullptr;
    float pos[3] = {0, 0, 0};
    u16 tod = 0;
    float base = 0.5f;
  };
  std::vector<Lit> lit;
  // the trees' bounds (x, z, game units): whether one is near the camera
  std::unordered_map<const tfrag3::ShrubTree*, std::array<float, 4>> tree_bounds;
  // per mesh, per look (tint and light): the copies
  struct Group {
    int mesh = -1;
    float rgb[3] = {0.5f, 0.5f, 0.5f};
    std::vector<GpuInstance> copies;
  };
  std::vector<Group> groups;
};
PlacedSet g_placed;
std::mutex g_placed_mutex;  // the list (set by the editor, read by the renderer)

// the library's mesh for a model, loaded on a worker the first time it's asked for (the library
// locked): -1 while loading or broken
int placed_mesh(Library& lib, const std::string& model) {
  auto& p = g_placed;
  auto found = p.mesh_of.find(model);
  if (found != p.mesh_of.end()) {
    return found->second;
  }
  auto& job = p.loading[model];
  if (!job.valid()) {
    job = std::async(std::launch::async,
                     [model] {
                       auto mesh = std::make_shared<Mesh>();
                       mesh->file = model;
                       mesh->ok = load_mesh(*mesh);
                       return mesh;
                     })
              .share();
  }
  if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return -1;
  }
  auto mesh = job.get();
  int idx = -1;
  if (mesh->ok) {
    idx = (int)lib.meshes.size();
    lib.meshes.push_back(std::move(*mesh));
  }
  p.mesh_of[model] = idx;
  p.loading.erase(model);
  return idx;
}
}  // namespace

std::vector<std::string> enhanced_scenery_models() {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(scenery_dir(), ec)) {
    const auto name = e.path().filename().string();
    if (e.path().extension() == ".glb" && name.find("_lod") == std::string::npos) {
      out.push_back(name);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

namespace {
u64 g_placed_version = 0;  // counts the lists set (the collision built again when it changes)

// the gameplay's clock, seconds
double play_time() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// a moving object's place at time t: its offset (game units) and turn about the vertical (radians);
// still while editing (false)
bool placed_motion(const EnhancedPlaced& o, double t, float off[3], float* yaw) {
  off[0] = off[1] = off[2] = 0.f;
  *yaw = 0.f;
  if (!o.motion || g_enhanced_menu.editing()) {
    return false;
  }
  constexpr double kTau = 6.283185307179586;
  const double ph = kTau * t / std::max(o.period, 0.1f);
  switch (o.motion) {
    case kMotionMove: {
      const float s = (float)(0.5 - 0.5 * std::cos(ph));
      for (int k = 0; k < 3; k++) {
        off[k] = o.move[k] * s;
      }
      break;
    }
    case kMotionBob: {
      const float s = (float)std::sin(ph);
      for (int k = 0; k < 3; k++) {
        off[k] = o.move[k] * s;
      }
      break;
    }
    case kMotionSpin:
      *yaw = (float)std::fmod(ph, kTau);
      break;
    default:
      return false;
  }
  return true;
}

// a point of an object at rest where it is when moved (yaw about its foot, then the offset)
void moved_point(const float pos[3], const float off[3], float yaw, const float in[3], float out[3]) {
  const float x = in[0] - pos[0], z = in[2] - pos[2];
  const float c = std::cos(yaw), s = std::sin(yaw);
  out[0] = pos[0] + c * x + s * z + off[0];
  out[1] = in[1] + off[1];
  out[2] = pos[2] - s * x + c * z + off[2];
}

// the gameplay's own state: what's taken (back when), cooldowns, the checkpoint
struct PlayState {
  u64 version = ~0ull;
  std::unordered_map<size_t, double> hidden_until, cool_until;
  bool was_dead = false;
  bool have_put = false;  // the editor's "Put Jak Here", done when back in the game
  float put[3] = {0, 0, 0};
  bool have_checkpoint = false;
  float checkpoint[3] = {0, 0, 0};
  double last_t = 0;
};
PlayState g_play;
}

void enhanced_set_placed(const std::vector<EnhancedPlaced>& placed) {
  std::lock_guard<std::mutex> lock(g_placed_mutex);
  g_placed.list = placed;
  g_placed_version++;
}

bool enhanced_model_bounds(const std::string& model, float lo[3], float hi[3]) {
  auto& lib = library();
  std::unique_lock<std::mutex> lock(lib.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return false;
  }
  std::lock_guard<std::mutex> plock(g_placed_mutex);
  int m = placed_mesh(lib, model);
  if (m < 0) {
    return false;
  }
  const auto& mesh = lib.meshes[m];
  // (around its foot: the middle of its bottom)
  const float cx = (mesh.lo.x() + mesh.hi.x()) * 0.5f, cz = (mesh.lo.z() + mesh.hi.z()) * 0.5f;
  lo[0] = mesh.lo.x() - cx;
  lo[1] = mesh.lo.y() - mesh.foot_y;
  lo[2] = mesh.lo.z() - cz;
  hi[0] = mesh.hi.x() - cx;
  hi[1] = mesh.hi.y() - mesh.foot_y;
  hi[2] = mesh.hi.z() - cz;
  return true;
}

namespace {
// ---- the placed objects' collision: each model's shape (its triangles, fewer), the world's (all
// the solid objects' triangles where they stand), built again when the list changes
constexpr int kCollideTris = 300;            // a model's shape at most
constexpr float kCollideMinSize = 0.5f;      // m: smaller than this, not solid on its own (pebbles)

struct CollideShape {
  bool ok = false;
  bool solid = false;  // on its own (collide -1): no see-through texture
  float size = 0.f;    // its biggest side, game units
  std::vector<Vec3> tris;  // around its foot, unscaled, three a triangle
};
std::unordered_map<int, CollideShape> g_shapes;  // by library mesh (the library locked)

struct CollideWorld {
  struct Obj {
    float lo[3], hi[3];  // (a moving one: all the places it goes)
    u32 first = 0, count = 0;
    EnhancedPlaced moving;  // its motion (motion 0: still)
  };
  std::vector<Obj> objs;
  std::vector<float> tris;   // 9 floats a triangle, game units
  std::vector<float> boxes;  // 6 a triangle: its bounds
  u64 version = ~0ull;
  bool pending = false;  // some models still loading: built again a little later
  std::chrono::steady_clock::time_point tried;
};
CollideWorld g_cworld;
std::mutex g_cworld_mutex;

const CollideShape& collide_shape(const Library& lib, int m) {
  auto& s = g_shapes[m];
  if (s.ok) {
    return s;
  }
  s.ok = true;
  const auto& mesh = lib.meshes[m];
  const Vec3 foot((mesh.lo.x() + mesh.hi.x()) * 0.5f, mesh.foot_y, (mesh.lo.z() + mesh.hi.z()) * 0.5f);
  const Vec3 ext = mesh.hi - mesh.lo;
  s.size = std::max({ext.x(), ext.y(), ext.z()});
  s.solid = !mesh.cutout;
  // (its positions only, the same ones joined: then fewer triangles, the shape kept)
  std::vector<float> pos(mesh.verts.size() * 3);
  for (size_t i = 0; i < mesh.verts.size(); i++) {
    pos[i * 3] = mesh.verts[i].x - foot.x();
    pos[i * 3 + 1] = mesh.verts[i].y - foot.y();
    pos[i * 3 + 2] = mesh.verts[i].z - foot.z();
  }
  std::vector<u32> remap(mesh.verts.size());
  const size_t nv = meshopt_generateVertexRemap(remap.data(), mesh.indices.data(), mesh.indices.size(),
                                                pos.data(), mesh.verts.size(), sizeof(float) * 3);
  std::vector<float> wpos(nv * 3);
  meshopt_remapVertexBuffer(wpos.data(), pos.data(), mesh.verts.size(), sizeof(float) * 3, remap.data());
  std::vector<u32> idx(mesh.indices.size());
  meshopt_remapIndexBuffer(idx.data(), mesh.indices.data(), mesh.indices.size(), remap.data());
  if (idx.size() > kCollideTris * 3) {
    std::vector<u32> out(idx.size());
    float err = 0.f;
    size_t n = meshopt_simplify(out.data(), idx.data(), idx.size(), wpos.data(), nv, sizeof(float) * 3,
                                kCollideTris * 3, 0.05f, meshopt_SimplifyPrune, &err);
    if (n > kCollideTris * 3 * 2 || n < 3) {
      n = meshopt_simplifySloppy(out.data(), idx.data(), idx.size(), wpos.data(), nv, sizeof(float) * 3,
                                 kCollideTris * 3, 0.05f, &err);
    }
    if (n >= 3) {
      out.resize(n);
      idx = std::move(out);
    }
  }
  auto at = [&](u32 i) { return Vec3(wpos[i * 3], wpos[i * 3 + 1], wpos[i * 3 + 2]); };
  // outward as the game's are, (v1 - v0) x (v2 - v0): the volume they close is positive that way
  double vol = 0;
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const Vec3 a = at(idx[t]), b = at(idx[t + 1]), c = at(idx[t + 2]);
    vol += (b - a).cross(c - a).dot(a);
  }
  const bool flip = vol < 0;
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    s.tris.push_back(at(idx[t]));
    s.tris.push_back(at(idx[flip ? t + 2 : t + 1]));
    s.tris.push_back(at(idx[flip ? t + 1 : t + 2]));
  }
  lg::info("[enhanced] collision: {} ({} triangles)", mesh.file, s.tris.size() / 3);
  return s;
}

// the world's collision built again from the list (both locks held)
void build_collide_world(Library& lib, CollideWorld& w) {
  w.objs.clear();
  w.tris.clear();
  w.boxes.clear();
  w.pending = false;
  for (const auto& o : g_placed.list) {
    if (o.collide == 0 || o.ghost) {
      continue;
    }
    const int m = placed_mesh(lib, o.model);
    if (m < 0) {
      w.pending |= g_placed.mesh_of.find(o.model) == g_placed.mesh_of.end();
      continue;
    }
    const auto& s = collide_shape(lib, m);
    const float scale = std::max({std::abs(o.scale[0]), std::abs(o.scale[1]), std::abs(o.scale[2])});
    if (o.collide < 0 && (!s.solid || s.size * scale < kCollideMinSize * kMeter)) {
      continue;
    }
    float cols[3][3];
    enhanced_placed_axes(o, cols);
    // (a mirroring scale turns the triangles inside out: back the right way)
    const float det = cols[0][0] * (cols[1][1] * cols[2][2] - cols[2][1] * cols[1][2]) -
                      cols[1][0] * (cols[0][1] * cols[2][2] - cols[2][1] * cols[0][2]) +
                      cols[2][0] * (cols[0][1] * cols[1][2] - cols[1][1] * cols[0][2]);
    CollideWorld::Obj obj;
    obj.first = (u32)(w.tris.size() / 9);
    for (int k = 0; k < 3; k++) {
      obj.lo[k] = 1e30f;
      obj.hi[k] = -1e30f;
    }
    for (size_t t = 0; t + 2 < s.tris.size(); t += 3) {
      float v[3][3];
      for (int j = 0; j < 3; j++) {
        const Vec3& l = s.tris[t + (det < 0 && j ? 3 - j : j)];
        for (int r = 0; r < 3; r++) {
          v[j][r] = o.pos[r] + cols[0][r] * l.x() + cols[1][r] * l.y() + cols[2][r] * l.z();
        }
      }
      float lo[3], hi[3];
      for (int r = 0; r < 3; r++) {
        lo[r] = std::min({v[0][r], v[1][r], v[2][r]});
        hi[r] = std::max({v[0][r], v[1][r], v[2][r]});
        obj.lo[r] = std::min(obj.lo[r], lo[r]);
        obj.hi[r] = std::max(obj.hi[r], hi[r]);
      }
      for (int j = 0; j < 3; j++) {
        w.tris.insert(w.tris.end(), v[j], v[j] + 3);
      }
      w.boxes.insert(w.boxes.end(), {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]});
    }
    obj.count = (u32)(w.tris.size() / 9) - obj.first;
    if (o.motion) {
      // (its box over all the places it goes)
      obj.moving.motion = o.motion;
      std::copy(o.move, o.move + 3, obj.moving.move);
      std::copy(o.pos, o.pos + 3, obj.moving.pos);
      obj.moving.period = o.period;
      float reach = 0.f;
      if (o.motion == kMotionSpin) {
        for (int c = 0; c < 4; c++) {
          const float x = (c & 1 ? obj.hi[0] : obj.lo[0]) - o.pos[0], z = (c & 2 ? obj.hi[2] : obj.lo[2]) - o.pos[2];
          reach = std::max(reach, std::sqrt(x * x + z * z));
        }
        obj.lo[0] = o.pos[0] - reach;
        obj.hi[0] = o.pos[0] + reach;
        obj.lo[2] = o.pos[2] - reach;
        obj.hi[2] = o.pos[2] + reach;
      } else {
        for (int k = 0; k < 3; k++) {
          const float a = o.motion == kMotionBob ? -std::abs(o.move[k]) : std::min(0.f, o.move[k]);
          const float b = o.motion == kMotionBob ? std::abs(o.move[k]) : std::max(0.f, o.move[k]);
          obj.lo[k] += a;
          obj.hi[k] += b;
        }
      }
    }
    if (obj.count) {
      w.objs.push_back(obj);
    }
  }
}

// does a line (from p, by d) come within r of a box?
bool line_near_box(const float* p, const float* d, float r, const float* lo, const float* hi) {
  float t0 = 0.f, t1 = 1.f;
  for (int k = 0; k < 3; k++) {
    const float a = lo[k] - r, b = hi[k] + r;
    if (std::abs(d[k]) < 1e-6f) {
      if (p[k] < a || p[k] > b) {
        return false;
      }
      continue;
    }
    float u0 = (a - p[k]) / d[k], u1 = (b - p[k]) / d[k];
    if (u0 > u1) {
      std::swap(u0, u1);
    }
    t0 = std::max(t0, u0);
    t1 = std::min(t1, u1);
    if (t0 > t1) {
      return false;
    }
  }
  return true;
}
}  // namespace

int enhanced_collide_tris(const float* box, const float* seg, float* out, int stride, int max) {
  auto& w = g_cworld;
  {
    // (built again when the list changed, or a little later while models still load; the library
    // busy: the old one this time)
    u64 version;
    {
      std::lock_guard<std::mutex> plock(g_placed_mutex);
      version = g_placed_version;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> clock(g_cworld_mutex);
    if (version != w.version || (w.pending && now - w.tried > std::chrono::milliseconds(250))) {
      auto& lib = library();
      std::unique_lock<std::mutex> lock(lib.mutex, std::try_to_lock);
      if (lock.owns_lock()) {
        std::lock_guard<std::mutex> plock(g_placed_mutex);
        w.version = g_placed_version;
        w.tried = now;
        build_collide_world(lib, w);
      }
    }
  }
  std::lock_guard<std::mutex> clock(g_cworld_mutex);
  const float* lo = box;
  const float* hi = box + 4;
  int n = 0;
  for (const auto& o : w.objs) {
    if (o.hi[0] < lo[0] || o.lo[0] > hi[0] || o.hi[1] < lo[1] || o.lo[1] > hi[1] || o.hi[2] < lo[2] ||
        o.lo[2] > hi[2]) {
      continue;
    }
    if (seg && !line_near_box(seg, seg + 4, seg[3], o.lo, o.hi)) {
      continue;
    }
    float off[3], yaw;
    if (o.moving.motion && placed_motion(o.moving, play_time(), off, &yaw)) {
      // (moving: each triangle where it is now)
      for (u32 t = o.first; t < o.first + o.count && n < max; t++) {
        float v[3][3], b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (int j = 0; j < 3; j++) {
          moved_point(o.moving.pos, off, yaw, &w.tris[t * 9 + j * 3], v[j]);
          for (int r = 0; r < 3; r++) {
            b[r] = std::min(b[r], v[j][r]);
            b[r + 3] = std::max(b[r + 3], v[j][r]);
          }
        }
        if (b[3] < lo[0] || b[0] > hi[0] || b[4] < lo[1] || b[1] > hi[1] || b[5] < lo[2] || b[2] > hi[2] ||
            (seg && !line_near_box(seg, seg + 4, seg[3], b, b + 3))) {
          continue;
        }
        float* dst = out + (size_t)n * stride;
        for (int j = 0; j < 3; j++) {
          dst[j * 4] = v[j][0];
          dst[j * 4 + 1] = v[j][1];
          dst[j * 4 + 2] = v[j][2];
          dst[j * 4 + 3] = 1.f;
        }
        n++;
      }
      continue;
    }
    for (u32 t = o.first; t < o.first + o.count; t++) {
      const float* b = &w.boxes[t * 6];
      if (b[3] < lo[0] || b[0] > hi[0] || b[4] < lo[1] || b[1] > hi[1] || b[5] < lo[2] || b[2] > hi[2]) {
        continue;
      }
      if (seg && !line_near_box(seg, seg + 4, seg[3], b, b + 3)) {
        continue;
      }
      if (n >= max) {
        return n;
      }
      float* dst = out + (size_t)n * stride;
      for (int j = 0; j < 3; j++) {
        dst[j * 4] = w.tris[t * 9 + j * 3];
        dst[j * 4 + 1] = w.tris[t * 9 + j * 3 + 1];
        dst[j * 4 + 2] = w.tris[t * 9 + j * 3 + 2];
        dst[j * 4 + 3] = 1.f;
      }
      n++;
    }
  }
  return n;
}

void enhanced_put_jak(const float* pos) {
  std::lock_guard<std::mutex> plock(g_placed_mutex);
  g_play.have_put = true;
  std::copy(pos, pos + 3, g_play.put);
}

int enhanced_gameplay(float* io) {
  float* carry = io + 4;
  float* put = io + 8;
  carry[0] = carry[1] = carry[2] = 0.f;
  if (g_enhanced_menu.editing()) {
    return 0;
  }
  const double now = play_time();
  const float jak[3] = {io[0], io[1], io[2]};
  const int state = (int)io[3];
  const bool ground = state & 1, dead = state & 2;
  int event = 0;
  {
    // (the platform under him: how far its point under his feet went since the last frame)
    std::lock_guard<std::mutex> clock(g_cworld_mutex);
    if (ground && g_play.last_t > 0) {
      for (const auto& o : g_cworld.objs) {
        if (!o.moving.motion || jak[0] < o.lo[0] || jak[0] > o.hi[0] || jak[2] < o.lo[2] || jak[2] > o.hi[2] ||
            jak[1] < o.lo[1] - 0.5f * kMeter || jak[1] > o.hi[1] + 0.5f * kMeter) {
          continue;
        }
        float off0[3], yaw0, off1[3], yaw1;
        placed_motion(o.moving, g_play.last_t, off0, &yaw0);
        placed_motion(o.moving, now, off1, &yaw1);
        // (where his foot is on it at rest: back from where it was)
        const float* c = o.moving.pos;
        const float x = jak[0] - c[0] - off0[0], z = jak[2] - c[2] - off0[2];
        const float rest[3] = {c[0] + std::cos(-yaw0) * x + std::sin(-yaw0) * z, jak[1] - off0[1],
                               c[2] - std::sin(-yaw0) * x + std::cos(-yaw0) * z};
        // (it must be on it now: over its triangles, near their top)
        float lo_y = 1e30f, hi_y = -1e30f;
        bool over = false;
        for (u32 t = o.first; t < o.first + o.count && !over; t++) {
          const float* b = &g_cworld.boxes[t * 6];
          over = rest[0] >= b[0] - 0.2f * kMeter && rest[0] <= b[3] + 0.2f * kMeter && rest[2] >= b[2] - 0.2f * kMeter &&
                 rest[2] <= b[5] + 0.2f * kMeter && rest[1] >= b[1] - 0.3f * kMeter && rest[1] <= b[4] + 0.5f * kMeter;
          lo_y = std::min(lo_y, b[1]);
          hi_y = std::max(hi_y, b[4]);
        }
        if (!over) {
          continue;
        }
        float now_p[3];
        moved_point(c, off1, yaw1, rest, now_p);
        for (int k = 0; k < 3; k++) {
          carry[k] = now_p[k] - jak[k];
        }
        break;
      }
    }
  }
  std::lock_guard<std::mutex> plock(g_placed_mutex);
  auto& ps = g_play;
  if (ps.version != g_placed_version) {
    ps.version = g_placed_version;
    ps.hidden_until.clear();
    ps.cool_until.clear();
  }
  ps.last_t = now;
  if (ps.have_put) {
    ps.have_put = false;
    std::copy(ps.put, ps.put + 3, put);
    return 6;
  }
  // (back from dying: at the last checkpoint)
  if (ps.was_dead && !dead && ps.have_checkpoint) {
    std::copy(ps.checkpoint, ps.checkpoint + 3, put);
    event = 6;
  }
  ps.was_dead = dead;
  if (dead || event) {
    return event;
  }
  const auto& list = g_placed.list;
  for (size_t i = 0; i < list.size(); i++) {
    const auto& o = list[i];
    if (!o.action) {
      continue;
    }
    float off[3], yaw;
    placed_motion(o, now, off, &yaw);
    const float at[3] = {o.pos[0] + off[0], o.pos[1] + off[1] + 0.5f * kMeter, o.pos[2] + off[2]};
    const float dx = jak[0] - at[0], dy = jak[1] + 0.5f * kMeter - at[1], dz = jak[2] - at[2];
    const float reach = (o.action == kActionTeleport || o.action == kActionVent ? 1.6f : 1.2f) * kMeter *
                        std::max({o.scale[0], o.scale[2], 1.f});
    if (dx * dx + dz * dz > reach * reach || std::abs(dy) > 2.f * kMeter) {
      continue;
    }
    auto cool = ps.cool_until.find(i);
    if (cool != ps.cool_until.end() && cool->second > now) {
      continue;
    }
    auto hid = ps.hidden_until.find(i);
    if (hid != ps.hidden_until.end() && hid->second > now) {
      continue;
    }
    switch (o.action) {
      case kActionHealth:
      case kActionDarkEco:
      case kActionLightEco:
      case kActionAmmo:
        // (taken: back in half a minute)
        ps.hidden_until[i] = now + 30.0;
        return o.action;
      case kActionVent:
        ps.cool_until[i] = now + 0.5;
        return kActionDarkEco;
      case kActionCheckpoint:
        if (!ps.have_checkpoint || ps.checkpoint[0] != at[0] || ps.checkpoint[2] != at[2]) {
          lg::info("[enhanced] checkpoint");
        }
        ps.have_checkpoint = true;
        std::copy(at, at + 3, ps.checkpoint);
        ps.cool_until[i] = now + 1.0;
        break;
      case kActionTeleport:
        for (size_t j = 0; j < list.size(); j++) {
          if (j != i && list[j].action == kActionTeleport && list[j].link == o.link) {
            float off2[3], yaw2;
            placed_motion(list[j], now, off2, &yaw2);
            put[0] = list[j].pos[0] + off2[0];
            put[1] = list[j].pos[1] + off2[1] + 0.5f * kMeter;
            put[2] = list[j].pos[2] + off2[2];
            // (not straight back: both rest a while)
            ps.cool_until[i] = ps.cool_until[j] = now + 2.5;
            return 6;
          }
        }
        break;
      default:
        break;
    }
  }
  return 0;
}

void enhanced_draw_placed(const tfrag3::ShrubTree& tree,
                          const TfragRenderSettings& settings,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& prof) {
  auto& p = g_placed;
  if (p.drawn_frame == render_state->frame_idx || tree.unpacked.vertices.empty() ||
      tree.static_draws.empty()) {
    return;
  }
  const auto& cam = settings.camera.trans;
  // (only a tree near the camera lights them: its time of day colors are the area's)
  auto tb = p.tree_bounds.find(&tree);
  if (tb == p.tree_bounds.end()) {
    std::array<float, 4> b = {1e30f, 1e30f, -1e30f, -1e30f};
    for (const auto& v : tree.unpacked.vertices) {
      b[0] = std::min(b[0], v.x);
      b[1] = std::min(b[1], v.z);
      b[2] = std::max(b[2], v.x);
      b[3] = std::max(b[3], v.z);
    }
    tb = p.tree_bounds.emplace(&tree, b).first;
  }
  const auto& b = tb->second;
  const float reach = 150.f * kMeter;
  if (cam.x() < b[0] - reach || cam.x() > b[2] + reach || cam.z() < b[1] - reach ||
      cam.z() > b[3] + reach) {
    return;
  }
  auto& lib = library();
  std::unique_lock<std::mutex> lock(lib.mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  std::lock_guard<std::mutex> plock(g_placed_mutex);
  p.drawn_frame = render_state->frame_idx;
  if (p.list.empty()) {
    return;
  }
  p.lit.resize(p.list.size());

  // each object's copy, grouped by mesh and look
  for (auto& g : p.groups) {
    g.copies.clear();
  }
  bool any = false;
  const auto& verts = tree.unpacked.vertices;
  for (size_t i = 0; i < p.list.size(); i++) {
    const auto& o = p.list[i];
    int m = placed_mesh(lib, o.model);
    if (m < 0) {
      continue;
    }
    const auto& mesh = lib.meshes[m];
    Vec3 pos(o.pos[0], o.pos[1], o.pos[2]);
    float cols[3][3];
    enhanced_placed_axes(o, cols);
    // (gameplay: moved, turned; taken, not drawn until it's back)
    if (o.action && !g_enhanced_menu.editing()) {
      auto h = g_play.hidden_until.find(i);
      if (h != g_play.hidden_until.end() && h->second > play_time()) {
        continue;
      }
    }
    {
      float off[3], yaw;
      if (placed_motion(o, play_time(), off, &yaw)) {
        pos = pos + Vec3(off[0], off[1], off[2]);
        const float c = std::cos(yaw), s = std::sin(yaw);
        for (auto& col : cols) {
          const float x = col[0], z = col[2];
          col[0] = c * x + s * z;
          col[2] = -s * x + c * z;
        }
      }
    }
    Affine full = {{Vec3(cols[0][0], cols[0][1], cols[0][2]), Vec3(cols[1][0], cols[1][1], cols[1][2]),
                    Vec3(cols[2][0], cols[2][1], cols[2][2])},
                   Vec3(0, 0, 0)};
    const float s = std::max({std::abs(o.scale[0]), std::abs(o.scale[1]), std::abs(o.scale[2]), 0.01f});
    Vec3 center = pos + full.turn(Vec3((mesh.lo.x() + mesh.hi.x()) * 0.5f, (mesh.hi.y() + mesh.foot_y) * 0.5f,
                                       (mesh.lo.z() + mesh.hi.z()) * 0.5f) -
                                  Vec3((mesh.lo.x() + mesh.hi.x()) * 0.5f, mesh.foot_y,
                                       (mesh.lo.z() + mesh.hi.z()) * 0.5f));
    float radius = s * (mesh.hi - mesh.lo).length() * 0.5f;
    float dx = center.x() - cam.x(), dy = center.y() - cam.y(), dz = center.z() - cam.z();
    float far_end = 400.f * kMeter + radius;
    if (dx * dx + dy * dy + dz * dz > far_end * far_end ||
        !sphere_in_view_ref(math::Vector4f(center.x(), center.y(), center.z(), radius),
                            settings.camera.planes)) {
      continue;
    }
    auto& lt = p.lit[i];
    if (lt.tree != &tree || lt.pos[0] != o.pos[0] || lt.pos[1] != o.pos[1] || lt.pos[2] != o.pos[2]) {
      lt.tree = &tree;
      std::copy(o.pos, o.pos + 3, lt.pos);
      float best = 1e30f;
      for (size_t v = 0; v < verts.size(); v += 3) {
        float ex = verts[v].x - pos.x(), ez = verts[v].z - pos.z();
        float d = ex * ex + ez * ez;
        if (d < best) {
          best = d;
          lt.tod = verts[v].color_index;
          // (its brightness only: the bush's hue isn't the ground's)
          lt.base = (0.3f * verts[v].rgba_base[0] + 0.59f * verts[v].rgba_base[1] +
                     0.11f * verts[v].rgba_base[2]) /
                    255.f;
        }
      }
    }
    full.t = pos - full.turn(Vec3((mesh.lo.x() + mesh.hi.x()) * 0.5f, mesh.foot_y,
                                  (mesh.lo.z() + mesh.hi.z()) * 0.5f));
    GpuInstance inst;
    for (int c = 0; c < 3; c++) {
      inst.col[c][0] = full.c[c].x();
      inst.col[c][1] = full.c[c].y();
      inst.col[c][2] = full.c[c].z();
      inst.col[c][3] = 0;
    }
    inst.col[0][3] = lt.tod;
    inst.col[3][0] = full.t.x();
    inst.col[3][1] = full.t.y();
    inst.col[3][2] = full.t.z();
    inst.col[3][3] = 1;
    // (the look rounded, so near alike objects share a draw)
    float rgb[3];
    for (int c = 0; c < 3; c++) {
      rgb[c] = std::round(lt.base * o.tint[c] * 32.f) / 32.f;
    }
    PlacedSet::Group* group = nullptr;
    for (auto& g : p.groups) {
      if (g.mesh == m && g.rgb[0] == rgb[0] && g.rgb[1] == rgb[1] && g.rgb[2] == rgb[2]) {
        group = &g;
        break;
      }
    }
    if (!group) {
      group = &p.groups.emplace_back();
      group->mesh = m;
      std::copy(rgb, rgb + 3, group->rgb);
    }
    group->copies.push_back(inst);
    any = true;
  }
  // (the looks no longer used)
  p.groups.erase(std::remove_if(p.groups.begin(), p.groups.end(),
                                [](const PlacedSet::Group& g) { return g.copies.empty(); }),
                 p.groups.end());
  if (!any) {
    return;
  }

  // the draw settings: a cutout one for blades and leaves, a solid one for stones
  DrawMode cut = tree.static_draws[0].mode, solid = tree.static_draws[0].mode;
  bool have_cut = false, have_solid = false;
  for (const auto& d : tree.static_draws) {
    if (d.mode.get_at_enable() && !have_cut) {
      cut = d.mode;
      have_cut = true;
    } else if (!d.mode.get_at_enable() && !have_solid) {
      solid = d.mode;
      have_solid = true;
    }
  }
  if (!have_solid) {
    solid.disable_at();
  }
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  const float now = std::chrono::duration<float>(clock::now() - start).count();
  auto& sh = render_state->shaders[ShaderId::SHRUB_INST];
  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB_INST);
  glUniform2f(glGetUniformLocation(sh.id(), "enh_lod"), 0.f, 0.f);
  glUniform1f(glGetUniformLocation(sh.id(), "enh_wet"), g_enhanced_wind.wet);
  glUniform1i(glGetUniformLocation(sh.id(), "tex_T10"), 10);
  glUniform1f(glGetUniformLocation(sh.id(), "enh_time"), now);
  const auto& pl = g_enhanced_player;
  glUniform4f(glGetUniformLocation(sh.id(), "enh_player"), pl.pos[0], pl.pos[1], pl.pos[2],
              pl.valid && g_enhanced_wind.push ? g_enhanced_wind.push_radius * kMeter : 0.f);
  const auto ps = enhanced_pushers_now();
  int pushers = g_enhanced_wind.push ? ps.count : 0;
  glUniform1i(glGetUniformLocation(sh.id(), "enh_pusher_count"), pushers);
  if (pushers > 0) {
    glUniform4fv(glGetUniformLocation(sh.id(), "enh_pushers"), pushers, &ps.pos[0][0]);
  }
  update_crush(lib);
  const auto& cr = lib.crush;
  const float cell = Crush::kCell * kMeter;
  glUniform4f(glGetUniformLocation(sh.id(), "enh_crush"), cr.ox * cell, cr.oz * cell,
              Crush::kCells * cell, cr.on && cr.tex ? 1.f : 0.f);
  glUniform1i(glGetUniformLocation(sh.id(), "tex_T2"), 2);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, cr.tex);
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_PRIMITIVE_RESTART);
  for (auto& g : p.groups) {
    auto& mesh = lib.meshes[g.mesh];
    if (!mesh.vao) {
      make_gl(mesh, lib);
    }
    glUniform3f(glGetUniformLocation(sh.id(), "enh_base_rgb"), g.rgb[0], g.rgb[1], g.rgb[2]);
    glUniform3f(glGetUniformLocation(sh.id(), "enh_model_avg"), mesh.avg.x(), mesh.avg.y(),
                mesh.avg.z());
    glUniform2f(glGetUniformLocation(sh.id(), "enh_tip"), mesh.lo.y(),
                std::max(mesh.hi.y() - mesh.lo.y(), 1.f));
    glUniform1f(glGetUniformLocation(sh.id(), "enh_foliage"), mesh.cutout ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(sh.id(), "enh_stone"), mesh.cutout ? 0.f : 1.f);
    glUniform1f(glGetUniformLocation(sh.id(), "enh_wind"),
                mesh.cutout && g_enhanced_wind.enabled ? g_enhanced_wind.strength : 0.f);
    setup_tfrag_shader(render_state, mesh.cutout ? cut : solid, ShaderId::SHRUB_INST);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, lib.instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, g.copies.size() * sizeof(GpuInstance), g.copies.data(),
                 GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mesh.tex);
    glDrawElementsInstanced(GL_TRIANGLES, mesh.indices.size(), GL_UNSIGNED_INT, nullptr,
                            g.copies.size());
    prof.add_draw_call();
    prof.add_tri(mesh.indices.size() / 3 * g.copies.size());
  }
  glEnable(GL_PRIMITIVE_RESTART);
  glBindVertexArray(0);
}
