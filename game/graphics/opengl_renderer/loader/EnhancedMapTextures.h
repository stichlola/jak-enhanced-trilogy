#pragma once

// Enhanced: sharper minimaps (Jak II and 3). The district maps (textures "map-..." in the
// "...-minimap" pages) are 4-bit images of 256 x 256 at most, magnified a lot on screen; they are
// redrawn at 4x when the level loads, each color region's edge smoothed (no stairs, no blur).
// The game still addresses them in its own texels: enhanced_texel_scale tells the direct renderer
// how many GL texels each game texel is (also the minimap's own render target, ProgressRenderer).

#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/pipelines/opengl.h"

// a map texture to redraw: its 4x image in out (w * 4 x h * 4 RGBA)
bool enhanced_upscale_map(const tfrag3::Texture& tex, std::vector<u32>& out);
constexpr int kEnhancedMapScale = 4;

// a minimap texture redrawn ahead (the HD icons, scripts/enhanced/minimap_icons.py, and the district
// maps, minimap_maps.py): out/enhanced/minimap/<game>/<tpage>/<name>.png, a whole multiple of the
// game's size (scale), in the PS2's alpha (128 opaque). Not in git: made from the game's art
bool enhanced_hd_minimap_texture(const tfrag3::Texture& tex, std::vector<u32>& out, int& scale);

void enhanced_set_texel_scale(GLuint tex, float scale);
void enhanced_forget_texture(GLuint tex);
float enhanced_texel_scale(GLuint tex);
