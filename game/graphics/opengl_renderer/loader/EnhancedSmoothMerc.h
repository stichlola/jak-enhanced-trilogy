#pragma once

#include <string>

#include "common/custom_data/Tfrag3Data.h"

// Rounds off the characters' silhouettes when a level loads (Jak 3 only): the listed merc models get
// one pass of PN-triangle-style subdivision, new vertices appended to the level's merc data, so the
// game's skeleton and animations apply as before. Draws stay 1:1 (same textures, same effects, same
// counts for Merc2's buffers). Parts the game can move on its own (blend shapes, ripple) and eyes
// keep their triangles; the edges they share with the rest aren't split, so nothing cracks.
void enhanced_smooth_characters(tfrag3::Level& level, const std::string& name);

// 1: on. Read when a level loads, so it needs a restart for the models in the common level (Jak,
// Daxter). JAK_ENHANCED_SMOOTH_CHARS, saved as smooth_chars; off by default
extern int g_enhanced_smooth_chars;
