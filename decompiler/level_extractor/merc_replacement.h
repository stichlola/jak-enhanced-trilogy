#pragma once

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/gltf_util.h"

#include "decompiler/level_extractor/common_formats.h"

namespace decompiler {

/*!
 * Jak Enhanced Trilogy: how one replacement is read, from <model>.json next to its <model>.glb
 * (see docs/character-models.md). No json: as before, node extras only.
 */
struct MercReplacementOptions {
  bool enabled = true;         // false: keep the game's model, the glb can stay in the folder
  bool custom_weights = false;  // the glb's own weights (else from the game's nearest vertex)
  bool joints_by_name = false;  // custom_weights, its joints found by name in the game's skeleton
  bool copy_eye_draws = false;  // keep the game's eyes
  bool copy_mod_draws = false;  // keep the game's blend-shape face
  bool all_lods = false;        // this -lod0 glb also stands in for -lod1, -lod2 without their own
  // the game's skeleton (from the art group, not the json), for joints_by_name
  const std::vector<level_tools::Joint>* game_joints = nullptr;
};
MercReplacementOptions read_merc_replacement_options(const fs::path& glb_path);

gltf_util::MercSwapData load_replacement_merc_model(
    tfrag3::MercModel& mdl,
    u32 current_idx_count,
    u32 current_vtx_count,
    u32 current_tex_count,
    const std::string& path,
    const std::vector<tfrag3::MercVertex>& old_verts,
    bool custom_mdl,
    const MercReplacementOptions& options = {});

gltf_util::MercSwapData load_custom_merc_model(const std::string& name,
                                               u32 current_idx_count,
                                               u32 current_vtx_count,
                                               u32 current_tex_count,
                                               const std::string& path,
                                               const std::vector<tfrag3::MercVertex>& old_verts,
                                               bool custom_mdl);
}  // namespace decompiler