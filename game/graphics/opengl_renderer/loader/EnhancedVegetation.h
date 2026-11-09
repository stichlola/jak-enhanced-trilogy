#pragma once

#include "common/custom_data/Tfrag3Data.h"

/*!
 * Jak Enhanced Trilogy: vegetation density. When a level loads, its grass and small plants (cutout
 * shrubs made of crossed cards, standing on the level's floor) get copies scattered around them,
 * turned, scaled and set down on the floor below, and tufts of their own cards grow on the grassy
 * floor around them. The copies go in extra shrub draws with a tier
 * (1 = moderate, 2 = lush); the shrub renderer draws the tiers up to g_enhanced_vegetation, so the
 * density changes without reloading.
 */
void enhanced_add_vegetation(tfrag3::Level& level, const std::string& name);

// 0 classic (the game's own), 1 moderate, 2 lush
extern int g_enhanced_vegetation;

// the feet of the small plants standing on the floor (the game's own, their copies and the tufts),
// game units: x, z, the floor's height y under them, horizontal radius r, height h; tier as the
// copies' draws (0 = the game's)
struct EnhancedPlantFoot {
  float x, y, z, r, h;
  u8 tier;
};
// the feet in a box of the levels loaded, up to a tier; returns a number that changes whenever a
// level's feet are set
u64 enhanced_vegetation_feet(float x0,
                             float z0,
                             float x1,
                             float z1,
                             int max_tier,
                             std::vector<EnhancedPlantFoot>* out);
