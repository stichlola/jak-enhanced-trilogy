#include "merc_replacement.h"

#include <algorithm>
#include <cmath>

#include "common/math/geometry.h"
#include "common/util/json_util.h"

using namespace gltf_util;

namespace decompiler {

MercReplacementOptions read_merc_replacement_options(const fs::path& glb_path) {
  MercReplacementOptions options;
  auto path = glb_path;
  path.replace_extension(".json");
  if (!file_util::file_exists(path.string())) {
    return options;
  }
  try {
    auto j = parse_commented_json(file_util::read_text_file(path), path.string());
    options.enabled = j.value("enabled", true);
    options.custom_weights = j.value("custom_weights", false);
    options.joints_by_name = j.value("joints_by_name", false);
    options.copy_eye_draws = j.value("copy_eye_draws", false);
    options.copy_mod_draws = j.value("copy_mod_draws", false);
    options.all_lods = j.value("all_lods", false);
  } catch (const std::exception& e) {
    // (a half-read json could make a mess that looks like the model's fault)
    lg::error("{}: {}. Keeping the game's model.", path.string(), e.what());
    options.enabled = false;
  }
  return options;
}

namespace {
/*!
 * Enhanced: for each joint of the glb's skin, the joint of the game's skeleton with its name: a rig
 * saved from Blender needn't keep the game's order. One the game doesn't have (a helper bone) goes
 * with its nearest ancestor that it does, as it would move about like it anyway. Warns about joints
 * away from the game's rest pose: the game skins from its own, not the glb's, so they would show.
 */
std::vector<int> joints_by_name(const tinygltf::Model& model,
                                const std::vector<NodeWithTransform>& all_nodes,
                                int skin_idx,
                                const std::vector<level_tools::Joint>& game_joints,
                                const std::string& model_name) {
  std::map<std::string, int> game_idx;
  for (size_t i = 0; i < game_joints.size(); i++) {
    game_idx[game_joints[i].name] = i;
  }
  const int fallback = game_idx.count("main") ? game_idx.at("main") : 0;
  std::map<int, int> parent;
  for (size_t i = 0; i < model.nodes.size(); i++) {
    for (int child : model.nodes[i].children) {
      parent[child] = i;
    }
  }
  std::map<int, const math::Matrix4f*> w_T_node;
  for (const auto& n : all_nodes) {
    w_T_node[n.node_idx] = &n.w_T_node;
  }

  std::vector<int> result;
  for (int node : model.skins.at(skin_idx).joints) {
    int at = node;
    while (at >= 0 && !game_idx.count(model.nodes[at].name)) {
      const auto it = parent.find(at);
      at = it == parent.end() ? -1 : it->second;
    }
    const int joint = at >= 0 ? game_idx.at(model.nodes[at].name) : fallback;
    result.push_back(joint);
    if (at != node) {
      lg::warn("{}: joint {} isn't in the game's skeleton, it goes with {}", model_name,
               model.nodes[node].name, game_joints[joint].name);
    } else if (game_joints[joint].parent_idx >= 0 && w_T_node.count(node)) {
      auto bind_T_w = game_joints[joint].bind_pose_T_w;
      for (int r = 0; r < 3; r++) {
        bind_T_w(r, 3) /= 4096.f;
      }
      const auto game_w = inverse(bind_T_w);
      const auto& glb_w = *w_T_node.at(node);
      float dist = 0;
      for (int r = 0; r < 3; r++) {
        dist += (game_w(r, 3) - glb_w(r, 3)) * (game_w(r, 3) - glb_w(r, 3));
      }
      if (dist > 0.05f * 0.05f) {
        lg::warn("{}: joint {} is {:.2f} m from the game's rest pose: expect its part to shift",
                 model_name, model.nodes[node].name, std::sqrt(dist));
      }
    }
  }
  return result;
}
}  // namespace

void extract(tfrag3::MercModel& mdl,
             MercExtractData& out,
             const tinygltf::Model& model,
             const std::vector<NodeWithTransform>& all_nodes,
             u32 index_offset,
             u32 vertex_offset,
             u32 tex_offset,
             bool& has_custom_weights,
             const MercReplacementOptions& options) {
  ASSERT(out.new_vertices.empty());

  // (one draw per primitive: two meshes sharing a material each keep their triangles)
  std::vector<std::pair<int, tfrag3::MercDraw>> draws;
  int mesh_count = 0;
  int prim_count = 0;
  int joints = 3;
  auto skin_idx = find_single_skin(model, all_nodes);
  if (skin_idx) {
    joints += get_joint_count(model, *skin_idx);
  }
  bool copy_eye_draws = false;
  bool copy_mod_draws = false;

  std::vector<int> joint_map;
  if (options.joints_by_name) {
    if (skin_idx && options.game_joints) {
      joint_map =
          joints_by_name(model, all_nodes, *skin_idx, *options.game_joints, mdl.name);
    } else {
      lg::warn("{}: joints_by_name needs a skin in the glb and the game's skeleton: not done",
               mdl.name);
    }
  }

  for (const auto& n : all_nodes) {
    const auto& node = model.nodes[n.node_idx];
    if (node.extras.Has("set_invisible") && node.extras.Get("set_invisible").Get<int>()) {
      continue;
    }
    if (node.mesh >= 0) {
      const auto& mesh = model.meshes[node.mesh];
      mesh_count++;
      has_custom_weights = (node.extras.Has("enable_custom_weights") &&
                            node.extras.Get("enable_custom_weights").Get<int>()) ||
                           options.custom_weights || !joint_map.empty();
      copy_eye_draws = (node.extras.Has("copy_eye_draws") &&
                        node.extras.Get("copy_eye_draws").Get<int>()) ||
                       options.copy_eye_draws;
      copy_mod_draws = (node.extras.Has("copy_mod_draws") &&
                        node.extras.Get("copy_mod_draws").Get<int>()) ||
                       options.copy_mod_draws;
      for (const auto& prim : mesh.primitives) {
        prim_count++;
        // extract index buffer
        std::vector<u32> prim_indices = gltf_util::gltf_index_buffer(
            model, prim.indices, out.new_vertices.size() + vertex_offset);
        ASSERT_MSG(prim.mode == TINYGLTF_MODE_TRIANGLES, "Unsupported triangle mode");
        // extract vertices
        auto verts =
            gltf_util::gltf_vertices(model, prim.attributes, n.w_T_node, true, true, mesh.name);
        out.new_vertices.insert(out.new_vertices.end(), verts.vtx.begin(), verts.vtx.end());
        out.new_colors.insert(out.new_colors.end(), verts.vtx_colors.begin(),
                              verts.vtx_colors.end());
        out.normals.insert(out.normals.end(), verts.normals.begin(), verts.normals.end());
        ASSERT(out.new_colors.size() == out.new_vertices.size());

        if (prim.attributes.count("JOINTS_0") && prim.attributes.count("WEIGHTS_0")) {
          auto joints_and_weights = gltf_util::extract_and_flatten_joints_and_weights(model, prim);
          ASSERT(joints_and_weights.size() == verts.vtx.size());
          if (!joint_map.empty()) {
            // a joint stored as its skin slot + 1, as the game's matrices are numbered
            for (auto& jw : joints_and_weights) {
              if (!(jw.weights[0] + jw.weights[1] + jw.weights[2] > 0.f)) {
                // unweighted (a NaN from the normalizing): else the vertex would vanish
                jw.weights[0] = 1.f;
                jw.weights[1] = jw.weights[2] = 0.f;
                jw.joints[0] = 3;  // main
                jw.joints[1] = jw.joints[2] = 0;
                continue;
              }
              for (int k = 0; k < 3; k++) {
                const int slot = (int)jw.joints[k] - 1;
                if (slot >= 0 && slot < (int)joint_map.size()) {
                  jw.joints[k] = joint_map[slot] + 1;
                }
              }
            }
          }
          out.joints_and_weights.insert(out.joints_and_weights.end(), joints_and_weights.begin(),
                                        joints_and_weights.end());
        } else {
          // add fake data for vertices without this data
          gltf_util::JointsAndWeights dummy;
          dummy.joints[0] = 3;
          dummy.weights[0] = 1.f;
          for (size_t i = 0; i < verts.vtx.size(); i++) {
            out.joints_and_weights.push_back(dummy);
          }
        }

        // TODO: just putting it all in one material
        auto& entry = draws.emplace_back();
        entry.first = prim.material;
        auto& draw = entry.second;
        draw.mode = gltf_util::make_default_draw_mode();  // todo rm
        draw.tree_tex_id = 0;                             // todo rm
        draw.num_triangles += prim_indices.size() / 3;
        draw.no_strip = true;
        draw.index_count = prim_indices.size();
        draw.first_index = index_offset + out.new_indices.size();

        out.new_indices.insert(out.new_indices.end(), prim_indices.begin(), prim_indices.end());
      }
    }
  }
  std::stable_sort(draws.begin(), draws.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });

  tfrag3::MercEffect e;
  tfrag3::MercEffect envmap_eff;
  envmap_eff.has_envmap = false;
  out.new_model.name = mdl.name;
  // if we have a skeleton, use that joint count, otherwise use a high default value since the model
  // we replace can have more
  out.new_model.max_bones = joints != 3 ? joints : 100;
  if (!joint_map.empty()) {
    // (its joints now go up to the game's)
    out.new_model.max_bones = options.game_joints->size() + 3;
  }
  out.new_model.max_draws = 0;

  for (const auto& [mat_idx, d_] : draws) {
    const auto& mat = model.materials[mat_idx];
    if (mat_idx < 0 || !gltf_util::material_has_envmap(model.materials[mat_idx]) ||
        !gltf_util::envmap_is_valid(model.materials[mat_idx])) {
      gltf_util::process_normal_merc_draw(model, out, tex_offset, e, mat_idx, d_);
    } else {
      envmap_eff.has_envmap = true;
      gltf_util::process_envmap_merc_draw(model, out, tex_offset, envmap_eff, mat_idx, d_);
    }
  }

  // in case a model only has envmap draws, we don't push the normal merc effect
  if (!e.all_draws.empty()) {
    out.new_model.effects.push_back(e);
  }
  if (envmap_eff.has_envmap) {
    out.new_model.effects.push_back(envmap_eff);
  }

  // copy any effects from the old model that used mod or eye draws
  if (copy_eye_draws || copy_mod_draws) {
    for (auto& old_eff : mdl.effects) {
      tfrag3::MercEffect eff;
      bool has_eye_draw = false;
      if (copy_eye_draws) {
        for (auto& draw : old_eff.all_draws) {
          if (draw.eye_id != 0xff) {
            has_eye_draw = true;
            eff.all_draws.push_back(draw);
          }
        }
      }
      if (copy_mod_draws && old_eff.has_mod_draw) {
        eff.has_mod_draw = true;
        eff.mod = old_eff.mod;
        eff.mod.fix_draw.clear();
        // (while the mod draws are in use all_draws aren't: without this, no eyes during blinks)
        if (copy_eye_draws) {
          for (const auto& draw : old_eff.mod.fix_draw) {
            if (draw.eye_id != 0xff) {
              eff.mod.fix_draw.push_back(draw);
            }
          }
        }
      }
      if ((copy_eye_draws || copy_mod_draws) && (eff.has_mod_draw || has_eye_draw)) {
        lg::info("adding old effect for {} (mod draw {}, eye draw {})", mdl.name,
                 old_eff.has_mod_draw, has_eye_draw);
        out.new_model.effects.push_back(eff);
      }
    }
  }

  for (auto& effect : out.new_model.effects) {
    out.new_model.max_draws += effect.all_draws.size();
  }

  lg::info("total of {} unique materials ({} normal, {} envmap)", out.new_model.max_draws,
           e.all_draws.size(), envmap_eff.all_draws.size());
  lg::info("Merged {} meshes and {} prims into {} vertices", mesh_count, prim_count,
           out.new_vertices.size());
}

void extract(const std::string& name,
             MercExtractData& out,
             const tinygltf::Model& model,
             const std::vector<NodeWithTransform>& all_nodes,
             u32 index_offset,
             u32 vertex_offset,
             u32 tex_offset,
             bool& has_custom_weights) {
  ASSERT(out.new_vertices.empty());

  std::map<int, tfrag3::MercDraw> draw_by_material;
  int mesh_count = 0;
  int prim_count = 0;
  int joints = 3;
  auto skin_idx = find_single_skin(model, all_nodes);
  if (skin_idx) {
    joints += get_joint_count(model, *skin_idx);
  }

  for (const auto& n : all_nodes) {
    const auto& node = model.nodes[n.node_idx];
    if (node.extras.Has("set_invisible") && node.extras.Get("set_invisible").Get<int>()) {
      continue;
    }
    if (node.mesh >= 0) {
      const auto& mesh = model.meshes[node.mesh];
      mesh_count++;
      has_custom_weights = node.extras.Has("enable_custom_weights") &&
                           node.extras.Get("enable_custom_weights").Get<int>();
      for (const auto& prim : mesh.primitives) {
        prim_count++;
        // extract index buffer
        std::vector<u32> prim_indices = gltf_util::gltf_index_buffer(
            model, prim.indices, out.new_vertices.size() + vertex_offset);
        ASSERT_MSG(prim.mode == TINYGLTF_MODE_TRIANGLES, "Unsupported triangle mode");
        // extract vertices
        auto verts =
            gltf_util::gltf_vertices(model, prim.attributes, n.w_T_node, true, true, mesh.name);
        out.new_vertices.insert(out.new_vertices.end(), verts.vtx.begin(), verts.vtx.end());
        out.new_colors.insert(out.new_colors.end(), verts.vtx_colors.begin(),
                              verts.vtx_colors.end());
        out.normals.insert(out.normals.end(), verts.normals.begin(), verts.normals.end());
        ASSERT(out.new_colors.size() == out.new_vertices.size());

        if (prim.attributes.count("JOINTS_0") && prim.attributes.count("WEIGHTS_0")) {
          auto joints_and_weights = gltf_util::extract_and_flatten_joints_and_weights(model, prim);
          ASSERT(joints_and_weights.size() == verts.vtx.size());
          out.joints_and_weights.insert(out.joints_and_weights.end(), joints_and_weights.begin(),
                                        joints_and_weights.end());
        } else {
          // add fake data for vertices without this data
          gltf_util::JointsAndWeights dummy;
          dummy.joints[0] = 3;
          dummy.weights[0] = 1.f;
          for (size_t i = 0; i < out.new_vertices.size(); i++) {
            out.joints_and_weights.push_back(dummy);
          }
        }

        // TODO: just putting it all in one material
        auto& draw = draw_by_material[prim.material];
        draw.mode = gltf_util::make_default_draw_mode();  // todo rm
        draw.tree_tex_id = 0;                             // todo rm
        draw.num_triangles += prim_indices.size() / 3;
        draw.no_strip = true;
        draw.index_count = prim_indices.size();
        draw.first_index = index_offset + out.new_indices.size();

        out.new_indices.insert(out.new_indices.end(), prim_indices.begin(), prim_indices.end());
      }
    }
  }

  tfrag3::MercEffect e;
  tfrag3::MercEffect envmap_eff;
  envmap_eff.has_envmap = false;
  out.new_model.name = name;
  // if we have a skeleton, use that joint count, otherwise use a high default value since the model
  // we replace can have more
  out.new_model.max_bones = joints != 3 ? joints : 100;
  out.new_model.max_draws = 0;

  for (const auto& [mat_idx, d_] : draw_by_material) {
    const auto& mat = model.materials[mat_idx];
    if (mat_idx < 0 || !gltf_util::material_has_envmap(model.materials[mat_idx]) ||
        !gltf_util::envmap_is_valid(model.materials[mat_idx])) {
      gltf_util::process_normal_merc_draw(model, out, tex_offset, e, mat_idx, d_);
    } else {
      envmap_eff.has_envmap = true;
      gltf_util::process_envmap_merc_draw(model, out, tex_offset, envmap_eff, mat_idx, d_);
    }
  }

  // in case a model only has envmap draws, we don't push the normal merc effect
  if (!e.all_draws.empty()) {
    out.new_model.effects.push_back(e);
  }
  if (envmap_eff.has_envmap) {
    out.new_model.effects.push_back(envmap_eff);
  }

  for (auto& effect : out.new_model.effects) {
    out.new_model.max_draws += effect.all_draws.size();
  }

  lg::info("total of {} unique materials ({} normal, {} envmap)", out.new_model.max_draws,
           e.all_draws.size(), envmap_eff.all_draws.size());
  lg::info("Merged {} meshes and {} prims into {} vertices", mesh_count, prim_count,
           out.new_vertices.size());
}

const tfrag3::MercVertex& find_closest(const std::vector<tfrag3::MercVertex>& old,
                                       float x,
                                       float y,
                                       float z) {
  float best_dist = 1e10;
  size_t best_idx = 0;

  for (size_t i = 0; i < old.size(); i++) {
    auto& v = old[i];
    float dx = v.pos[0] - x;
    float dy = v.pos[1] - y;
    float dz = v.pos[2] - z;
    float dist = (dx * dx) + (dy * dy) + (dz * dz);
    if (dist < best_dist) {
      best_dist = dist;
      best_idx = i;
    }
  }

  return old[best_idx];
}

void merc_convert_replacement(MercSwapData& out,
                              const MercExtractData& in,
                              const std::vector<tfrag3::MercVertex>& old_verts,
                              bool use_custom_weights) {
  out.new_model = in.new_model;
  out.new_indices = in.new_indices;
  out.new_textures = in.tex_pool.textures_by_idx;

  // convert vertices
  for (size_t i = 0; i < in.new_vertices.size(); i++) {
    const auto& y = in.new_vertices[i];

    // (slow, and not needed with the model's own weights)
    const auto& copy_from =
        use_custom_weights ? old_verts.front() : find_closest(old_verts, y.x, y.y, y.z);
    auto& x = out.new_vertices.emplace_back();
    x.pos[0] = y.x;
    x.pos[1] = y.y;
    x.pos[2] = y.z;
    x.normal[0] = in.normals.at(i).x();
    x.normal[1] = in.normals.at(i).y();
    x.normal[2] = in.normals.at(i).z();
    if (use_custom_weights) {
      x.weights[0] = in.joints_and_weights.at(i).weights[0];
      x.weights[1] = in.joints_and_weights.at(i).weights[1];
      x.weights[2] = in.joints_and_weights.at(i).weights[2];
      x.mats[0] = in.joints_and_weights.at(i).joints[0];
      x.mats[1] = in.joints_and_weights.at(i).joints[1];
      x.mats[2] = in.joints_and_weights.at(i).joints[2];
    } else {
      x.weights[0] = copy_from.weights[0];
      x.weights[1] = copy_from.weights[1];
      x.weights[2] = copy_from.weights[2];
      x.mats[0] = copy_from.mats[0];
      x.mats[1] = copy_from.mats[1];
      x.mats[2] = copy_from.mats[2];
    }
    x.st[0] = y.s;
    x.st[1] = y.t;
    x.rgba[0] = in.new_colors[i][0];
    x.rgba[1] = in.new_colors[i][1];
    x.rgba[2] = in.new_colors[i][2];
    x.rgba[3] = in.new_colors[i][3];
  }
}

void merc_convert_custom(MercSwapData& out, const MercExtractData& in) {
  out.new_model = in.new_model;
  out.new_indices = in.new_indices;
  out.new_textures = in.tex_pool.textures_by_idx;

  // convert vertices
  for (size_t i = 0; i < in.new_vertices.size(); i++) {
    const auto& y = in.new_vertices[i];
    auto& x = out.new_vertices.emplace_back();
    x.pos[0] = y.x;
    x.pos[1] = y.y;
    x.pos[2] = y.z;
    x.normal[0] = in.normals.at(i).x();
    x.normal[1] = in.normals.at(i).y();
    x.normal[2] = in.normals.at(i).z();
    x.weights[0] = in.joints_and_weights.at(i).weights[0];
    x.weights[1] = in.joints_and_weights.at(i).weights[1];
    x.weights[2] = in.joints_and_weights.at(i).weights[2];
    x.st[0] = y.s;
    x.st[1] = y.t;
    x.rgba[0] = in.new_colors[i][0];
    x.rgba[1] = in.new_colors[i][1];
    x.rgba[2] = in.new_colors[i][2];
    x.rgba[3] = in.new_colors[i][3];
    x.mats[0] = in.joints_and_weights.at(i).joints[0];
    x.mats[1] = in.joints_and_weights.at(i).joints[1];
    x.mats[2] = in.joints_and_weights.at(i).joints[2];
  }
}

MercSwapData load_replacement_merc_model(tfrag3::MercModel& mdl,
                                         u32 current_idx_count,
                                         u32 current_vtx_count,
                                         u32 current_tex_count,
                                         const std::string& path,
                                         const std::vector<tfrag3::MercVertex>& old_verts,
                                         bool custom_mdl,
                                         const MercReplacementOptions& options) {
  MercSwapData result;
  lg::info("Reading gltf mesh: {}", path);
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  bool res = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  ASSERT_MSG(warn.empty(), warn.c_str());
  ASSERT_MSG(err.empty(), err.c_str());
  ASSERT_MSG(res, "Failed to load GLTF file!");
  auto all_nodes = flatten_nodes_from_all_scenes(model);

  MercExtractData extract_data;
  auto has_custom_weights = false;
  extract(mdl, extract_data, model, all_nodes, current_idx_count, current_vtx_count,
          current_tex_count, has_custom_weights, options);
  if (custom_mdl) {
    merc_convert_custom(result, extract_data);
  } else {
    merc_convert_replacement(result, extract_data, old_verts, has_custom_weights);
  }

  return result;
}

MercSwapData load_custom_merc_model(const std::string& name,
                                    u32 current_idx_count,
                                    u32 current_vtx_count,
                                    u32 current_tex_count,
                                    const std::string& path,
                                    const std::vector<tfrag3::MercVertex>& old_verts,
                                    bool custom_mdl) {
  MercSwapData result;
  lg::info("Reading gltf mesh: {}", path);
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  bool res = loader.LoadBinaryFromFile(&model, &err, &warn, path);
  ASSERT_MSG(warn.empty(), warn.c_str());
  ASSERT_MSG(err.empty(), err.c_str());
  ASSERT_MSG(res, "Failed to load GLTF file!");
  auto all_nodes = flatten_nodes_from_all_scenes(model);

  MercExtractData extract_data;
  auto has_custom_weights = false;
  extract(name, extract_data, model, all_nodes, current_idx_count, current_vtx_count,
          current_tex_count, has_custom_weights);
  if (custom_mdl) {
    merc_convert_custom(result, extract_data);
  } else {
    merc_convert_replacement(result, extract_data, old_verts, has_custom_weights);
  }

  return result;
}
}  // namespace decompiler