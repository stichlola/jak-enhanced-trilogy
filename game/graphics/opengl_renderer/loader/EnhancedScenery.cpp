#include "EnhancedScenery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

float g_enhanced_scenery_dist = [] {
  const char* env = std::getenv("JAK_ENHANCED_SCENERY");
  return env ? std::clamp((float)std::atof(env), 0.f, 200.f) : 60.f;
}();

namespace {

constexpr float kMeter = 4096.f;

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
  float base_rgb[3] = {0.5f, 0.5f, 0.5f};
  float tint[3] = {1, 1, 1};  // the model's texture brought to the proto's colors
  std::vector<GpuInstance> instances;
  std::vector<math::Vector4f> spheres;  // culling: center, radius (game units)
  std::vector<u8> tiers;                // the vegetation density they show at (0 = always)
};

struct SceneryTree {
  std::vector<Batch> batches;
};

struct Library {
  std::mutex mutex;
  bool loaded_config = false;
  std::unordered_map<std::string, std::vector<int>> protos;  // proto name -> meshes
  std::vector<Mesh> meshes;
  GLuint instance_vbo = 0;
  std::vector<GpuInstance> scratch;
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
      for (const auto& f : files) {
        auto file = f.get<std::string>();
        auto it = by_file.find(file);
        if (it == by_file.end()) {
          Mesh mesh;
          mesh.file = file;
          mesh.ok = load_mesh(mesh);
          it = by_file.emplace(file, (int)lib.meshes.size()).first;
          lib.meshes.push_back(std::move(mesh));
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

// the models listed for a proto: by its name without the ".mb" the levels give it
const std::vector<int>* models_for(const Library& lib, const std::string& proto) {
  std::string name = proto;
  if (name.size() > 3 && name.compare(name.size() - 3, 3, ".mb") == 0) {
    name.resize(name.size() - 3);
  }
  auto it = lib.protos.find(name);
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
  auto fit = [&](u32 g, const Mesh& mesh, float yaw) {
    const auto& grp = groups[g];
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (u32 v = grp.start_vert; v < grp.end_vert; v++) {
      const auto& pv = tree.packed_vertices.vertices[v];
      lo = Vec3(std::min(lo.x(), pv.x), std::min(lo.y(), pv.y), std::min(lo.z(), pv.z));
      hi = Vec3(std::max(hi.x(), pv.x), std::max(hi.y(), pv.y), std::max(hi.z(), pv.z));
    }
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
    int m = meshes[std::min((size_t)(hash01(tree_idx, seed, 1) * meshes.size()), meshes.size() - 1)];
    const auto& mesh = lib.meshes[m];
    Affine full = place.after(fit(g, mesh, 6.2831853f * hash01(tree_idx, seed, 2)));
    u64 key = ((u64)proto << 32) | (u32)m;
    auto it = batch_of.find(key);
    if (it == batch_of.end()) {
      Batch b;
      b.mesh = m;
      const auto& draw = tree.static_draws[first_draw_of_proto.at(proto)];
      b.mode = draw.mode;
      b.foliage = draw.mode.get_at_enable();
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
      // the model's colors toward the proto's texture (a straw-pale grass stays straw-pale)
      if (draw.tree_tex_id < textures.size()) {
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
  tree.enh_scenery = data;
}

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

  glGenTextures(1, &mesh.tex);
  glBindTexture(GL_TEXTURE_2D, mesh.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mesh.tex_w, mesh.tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               mesh.rgba.data());
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

}  // namespace

void enhanced_prepare_scenery(tfrag3::Level& level, const std::string& name) {
  if (level.shrub_trees.empty()) {
    return;
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
  float end = g_enhanced_scenery_dist * kMeter;
  return math::Vector2f(end * 0.9f, end);
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
  const float far_end = g_enhanced_scenery_dist * kMeter;
  const auto& cam = settings.camera.trans;

  bool setup = false;
  auto& sh = render_state->shaders[ShaderId::SHRUB_INST];
  for (const auto& b : data.batches) {
    auto& mesh = lib.meshes[b.mesh];
    // the copies near enough, in view, at the vegetation density shown
    lib.scratch.clear();
    for (size_t i = 0; i < b.instances.size(); i++) {
      const auto& s = b.spheres[i];
      float dx = s.x() - cam.x(), dy = s.y() - cam.y(), dz = s.z() - cam.z();
      float reach = far_end + s.w();
      if (dx * dx + dy * dy + dz * dz > reach * reach || b.tiers[i] > g_enhanced_vegetation ||
          !sphere_in_view_ref(s, settings.camera.planes)) {
        continue;
      }
      lib.scratch.push_back(b.instances[i]);
    }
    if (lib.scratch.empty()) {
      continue;
    }
    if (!mesh.vao) {
      make_gl(mesh, lib);
    }
    if (!setup) {
      setup = true;
      first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB_INST);
      auto fade = enhanced_scenery_fade(tree);
      glUniform2f(glGetUniformLocation(sh.id(), "enh_lod"), fade.x(), fade.y());
      glUniform1f(glGetUniformLocation(sh.id(), "enh_wet"), g_enhanced_wind.wet);
      glUniform1i(glGetUniformLocation(sh.id(), "tex_T10"), 10);
      // wind and the push of Jak and whoever is near him, as the shrubs
      using clock = std::chrono::steady_clock;
      static const auto start = clock::now();
      glUniform1f(glGetUniformLocation(sh.id(), "enh_time"),
                  std::chrono::duration<float>(clock::now() - start).count());
      const auto& pl = g_enhanced_player;
      glUniform4f(glGetUniformLocation(sh.id(), "enh_player"), pl.pos[0], pl.pos[1], pl.pos[2],
                  pl.valid && g_enhanced_wind.push ? g_enhanced_wind.push_radius * kMeter : 0.f);
      const auto& ps = g_enhanced_pushers;
      int pushers = g_enhanced_wind.push ? ps.count : 0;
      glUniform1i(glGetUniformLocation(sh.id(), "enh_pusher_count"), pushers);
      if (pushers > 0) {
        glUniform4fv(glGetUniformLocation(sh.id(), "enh_pushers"), pushers, &ps.pos[0][0]);
      }
    }
    glUniform3f(glGetUniformLocation(sh.id(), "enh_base_rgb"), b.base_rgb[0] * b.tint[0],
                b.base_rgb[1] * b.tint[1], b.base_rgb[2] * b.tint[2]);
    glUniform2f(glGetUniformLocation(sh.id(), "enh_tip"), mesh.lo.y(),
                std::max(mesh.hi.y() - mesh.lo.y(), 1.f));
    glUniform1f(glGetUniformLocation(sh.id(), "enh_foliage"), b.foliage ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(sh.id(), "enh_wind"),
                b.foliage && g_enhanced_wind.enabled ? g_enhanced_wind.strength : 0.f);
    setup_tfrag_shader(render_state, b.mode, ShaderId::SHRUB_INST);

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, lib.instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, lib.scratch.size() * sizeof(GpuInstance), lib.scratch.data(),
                 GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mesh.tex);
    glDisable(GL_PRIMITIVE_RESTART);
    glDrawElementsInstanced(GL_TRIANGLES, mesh.indices.size(), GL_UNSIGNED_INT, nullptr,
                            lib.scratch.size());
    glEnable(GL_PRIMITIVE_RESTART);
    prof.add_draw_call();
    prof.add_tri(mesh.indices.size() / 3 * lib.scratch.size());
  }
  glBindVertexArray(0);
}
