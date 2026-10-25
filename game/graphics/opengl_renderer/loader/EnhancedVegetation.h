#pragma once

#include "common/custom_data/Tfrag3Data.h"

/*!
 * Jak 3 Enhanced: vegetation density. When a level loads, its grass and small plants (cutout
 * shrubs made of crossed cards, standing on the level's floor) get copies scattered around them,
 * turned, scaled and set down on the floor below. The copies go in extra shrub draws with a tier
 * (1 = moderate, 2 = lush); the shrub renderer draws the tiers up to g_enhanced_vegetation, so the
 * density changes without reloading.
 */
void enhanced_add_vegetation(tfrag3::Level& level, const std::string& name);

// 0 classic (the game's own), 1 moderate, 2 lush
extern int g_enhanced_vegetation;
