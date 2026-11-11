#pragma once

// Enhanced: true PBR's companion maps (scripts/enhanced/pbr_maps.py): per HD texture, a map at half its
// size with the tangent-space normal (R, G) and a roughness offset (B, 0.5 = the material class's own).
// Read on the loader thread with the level, put on the GPU with its textures, bound per draw on
// texture unit 11 (tfrag3.frag) or 1 (merc2.frag). None without HD textures: then all as before.

#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/opengl_renderer/loader/common.h"

// the switch (JAK_ENHANCED_PBR=0, menu "True PBR"); the maps stay loaded either way
extern bool g_enhanced_pbr;

// the maps of a level's textures, from disk (any thread); empty entries where there are none
std::vector<EnhPbrPixels> enhanced_pbr_read(const tfrag3::Level& level);

// the next texture's map on the GPU (after its texture: lev.enh_pbr follows lev.textures). Returns the
// bytes uploaded
int enhanced_pbr_upload_next(LevelData& lev);

// every map read for the level on the GPU at once (the common level, loaded before play)
void enhanced_pbr_upload_all(LevelData& lev);
