#pragma once

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

class ScopedProfilerNode;
struct SharedRenderState;

/*!
 * Jak Enhanced Trilogy: detailed scenery, a light take on Nanite for the game's shrubs (grass,
 * bushes, pebbles). A shrub proto listed in custom_assets/<game>/scenery/scenery.json gets one or
 * more detailed models (.glb) that stand in for it near the camera:
 *  - each model is loaded once and drawn instanced, one transform per copy of the proto (the
 *    level's own and the vegetation's), so memory stays flat however many copies there are;
 *  - only the copies near the camera and in view are drawn (culled on the CPU each frame);
 *  - farther out the game's own low-poly proto takes over, with a dithered crossfade (the proto's
 *    vertices are flagged, shrub.frag fades them in as the model fades out);
 *  - with the setting at 0, a missing or broken model, or a proto not listed: the game's own.
 *
 * scenery.json: { "<proto>": ["a.glb", "b.glb"], ... } (each copy picks one of them); the model is
 * fitted into the proto's own height, its foot on the proto's foot, and turned at random.
 *
 * TIE protos (crates, barrels, lamps, small rocks) go in the same json: fitted the same way but in
 * the proto's own turn, so the model should face as the proto does (+y up). Not for protos in the
 * wind or with an environment map. The collision and the sun's shadows stay the game's proto's.
 */

// called by the loader on its thread, after the vegetation: finds the copies of the listed protos
void enhanced_prepare_scenery(tfrag3::Level& level, const std::string& name);

// called by the shrub renderer after a tree's own draws (its time of day texture still bound)
void enhanced_draw_scenery(const tfrag3::ShrubTree& tree,
                           const TfragRenderSettings& settings,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof);

// the same for a TIE tree, called by Tie3 after its normal draws (its time of day texture still
// bound); proto_vis: which protos the game shows now (per proto, null: all)
void enhanced_draw_scenery(const tfrag3::TieTree& tree,
                           const std::vector<u8>* proto_vis,
                           const TfragRenderSettings& settings,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof);

// the shrub shader's crossfade band for a tree, game units from the camera (0, 0 = off)
math::Vector2f enhanced_scenery_fade(const tfrag3::ShrubTree& tree);
// (tfrag3's enh_lod for a TIE tree)
math::Vector2f enhanced_scenery_fade(const tfrag3::TieTree& tree);

// the crush map (where the grass still lies after someone went through), updated once a frame:
// its texture (0: none), and in out its corner x, z, its size (game units), on
GLuint enhanced_crush_map(float out[4]);

// how far out the detailed models are drawn, meters (0 = off)
extern float g_enhanced_scenery_dist;
// times that, set by the auto quality (1 = all of it): the far versions and impostors take over sooner
extern float g_enhanced_scenery_scale;

// Objects placed by hand (the in-game editor, EnhancedEditor): a model from the scenery folder,
// where its foot stands (game units), turned (a quaternion: x y z w), scaled along its own axes
// (first), its colors tinted
struct EnhancedPlaced {
  std::string model;
  float pos[3] = {0, 0, 0};
  float rot[4] = {0, 0, 0, 1};
  float scale[3] = {1, 1, 1};
  float tint[3] = {1, 1, 1};
  // Jak stands on it, as on the game's own ground: -1 the model's own way (solid if it's big and has
  // no see-through texture: rocks yes, grass no), 0 never, 1 always
  int collide = -1;
  // (not saved: the editor's things falling or moved by hand, not solid for now)
  bool ghost = false;
  // gameplay (played outside the editor only): how it moves, a cycle every `period` seconds
  // (1 there and back by `move`, 2 turning about the vertical, 3 bobbing by `move` either way)
  int motion = 0;
  float move[3] = {0, 0, 0};  // game units
  float period = 4.f;
  // what Jak touching it does: 1 health, 2 dark eco, 3 light eco, 4 ammo (each taken, back after a
  // while), 5 an eco vent (dark eco again and again), 6 a checkpoint (back here after dying), 7 a
  // teleport to the other one with the same link
  int action = 0;
  int link = 0;
  // the editor's: hidden while editing (H, the outliner's eye), not selectable (its lock), its group
  bool hidden = false;
  bool locked = false;
  std::string group;
};

enum EnhancedMotion { kMotionNone, kMotionMove, kMotionSpin, kMotionBob, kMotionCount };
enum EnhancedAction {
  kActionNone,
  kActionHealth,
  kActionDarkEco,
  kActionLightEco,
  kActionAmmo,
  kActionVent,
  kActionCheckpoint,
  kActionTeleport,
  kActionCount
};

// its turn and scale as a matrix's columns: cols[c] the model's axis c, scaled, in the world
inline void enhanced_placed_axes(const EnhancedPlaced& o, float cols[3][3]) {
  const float x = o.rot[0], y = o.rot[1], z = o.rot[2], w = o.rot[3];
  const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
                         {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
                         {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}};
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      cols[c][r] = m[c][r] * o.scale[c];
    }
  }
}

// the models in the scenery folder that can be placed (.glb, not the far versions)
std::vector<std::string> enhanced_scenery_models();

// the objects to draw from now on (copied)
void enhanced_set_placed(const std::vector<EnhancedPlaced>& placed);

// a model's bounds around its foot (game units, unscaled): false while it's still loading
bool enhanced_model_bounds(const std::string& model, float lo[3], float hi[3]);

// called by the shrub renderer after a tree's draws: the placed objects, once a frame (lit by the
// first tree drawn near them)
void enhanced_draw_placed(const tfrag3::ShrubTree& tree,
                          const TfragRenderSettings& settings,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& prof);
