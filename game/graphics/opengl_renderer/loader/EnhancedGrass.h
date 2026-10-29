#pragma once

#include <array>
#include <functional>
#include <vector>
#include <string>

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

class ScopedProfilerNode;
struct SharedRenderState;

/*!
 * Jak Enhanced Trilogy: dense grass fields, in the manner of Ghost of Tsushima's. A zone listed in
 * game/assets/enhanced/grass.json gets a meadow of single blades, made on the graphics card from
 * nothing but instance and vertex numbers (shrub_grass.vert):
 *  - when a level with floor in the zone loads, the floor's heights are baked into a small
 *    texture (half a meter apart), the only memory the field takes;
 *  - each frame the zone's 4 m tiles near the camera and in view are picked, each with a level of
 *    detail by its distance (many blades of many segments near, few of few far);
 *  - a lower level's blades are a subset of a higher one's, the extra ones shrink away across the
 *    edge between them (no popping); wind gusts roll across the field, Jak pushes it aside.
 * The field is drawn with the shrub tree nearest its middle (its time of day colors light it).
 */

// the floor height (game units) at (x, z), false where there's none; own: the level's own floor (not
// the height field under everything)
using GrassFloor = std::function<bool(float x, float z, float* y, bool* own)>;
// a loaded level's floor nearest a height (game units), false where it has none
using GrassFloorNear = std::function<bool(float x, float z, float y_ref, float* y)>;

// how the painted grass looks (the editor's grass brush): its height and blades' width (times the
// usual), how dense (times the zone's), its green
struct EnhancedGrassStyle {
  float height = 1.f;
  float width = 1.f;
  float density = 1.f;
  float wind = 1.f;  // how much the gusts bend it (times the wind's)
  float color[3] = {0.14f, 0.36f, 0.05f};
  float dry[3] = {0.36f, 0.44f, 0.1f};  // the drier patches' tips
};

// the dense grass field drawn this frame (the composite darkens the ground under it): its middle
// x, z, radius (m) and density, the water's height (m), the frame it was drawn in
struct EnhancedGrassGround {
  float zone[4] = {0, 0, 0, 0};
  float water = -1000.f;
  u64 frame = 0;
  // its texture (g: how lush, 0..1) and where it lies: its corner x, z and side (m)
  unsigned tex = 0;
  float field[3] = {0, 0, 1};
};
extern EnhancedGrassGround g_enhanced_grass_ground;

// whether this game has any dense grass zones (grass.json)
bool enhanced_grass_has_zones();

// called by the loader on its thread (enhanced_add_vegetation): bakes the zones the level covers
void enhanced_prepare_grass(tfrag3::Level& level, const std::string& name, const GrassFloor& floor);
// and keeps the level's floor (the last few levels'), for the fields painted anywhere: the zones made
// by the editor's brush are baked (again) from these
void enhanced_grass_keep_floor(const std::string& level, GrassFloorNear floor);

// called by the shrub renderer after a tree's draws (its time of day texture still bound)
void enhanced_draw_grass(const tfrag3::ShrubTree& tree,
                         const TfragRenderSettings& settings,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof);

// painted in the in-game editor: the lush strokes and the edge ones (x, z, the brush's radius, m),
// how far it thins away past the lush where no edge is near (m). The painted zones are baked again
// (on a worker), and with save kept in their grass_paint files
void enhanced_grass_paint(const std::vector<std::array<float, 3>>& lush,
                          const std::vector<std::array<float, 3>>& edge,
                          float fade,
                          bool save);
// whether there's a painted dense grass field at x, z (m, the game's world)
bool enhanced_grass_zone_at(float x, float z);
// whether a field covers x, z (m) at least `margin` m inside its edge (else the brush makes one)
bool enhanced_grass_field_at(float x, float z, float margin);
// a new zone of dense grass, 40 m around x, y, z (m, the game's world): kept in grass_paint/user_zones.json,
// its field baked from the loaded levels' floors (a moment, on a worker); false: too many
bool enhanced_grass_add_zone(float x, float y, float z);
// the zones made by the brush, the fields baked (for the editor's status line)
void enhanced_grass_counts(int* zones, int* user_zones, int* fields);
// the painted grass's look (kept with the painting on save)
EnhancedGrassStyle enhanced_grass_style();
void enhanced_set_grass_style(const EnhancedGrassStyle& style);
// the painting as it is now (the first painted zone's): false when there's none
bool enhanced_grass_painted(std::vector<std::array<float, 3>>* lush,
                            std::vector<std::array<float, 3>>* edge,
                            float* fade);

// the grass's density, 0 (off) to 1
extern float g_enhanced_grass;
