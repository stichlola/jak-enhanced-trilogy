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
 */

// called by the loader on its thread, after the vegetation: finds the copies of the listed protos
void enhanced_prepare_scenery(tfrag3::Level& level, const std::string& name);

// called by the shrub renderer after a tree's own draws (its time of day texture still bound)
void enhanced_draw_scenery(const tfrag3::ShrubTree& tree,
                           const TfragRenderSettings& settings,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof);

// the shrub shader's crossfade band for a tree, game units from the camera (0, 0 = off)
math::Vector2f enhanced_scenery_fade(const tfrag3::ShrubTree& tree);

// how far out the detailed models are drawn, meters (0 = off)
extern float g_enhanced_scenery_dist;
