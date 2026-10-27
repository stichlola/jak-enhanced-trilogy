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

// a HUD, menu or minimap texture redrawn ahead (the minimap's icons, minimap_icons.py, its district
// maps, minimap_maps.py, the rest 4x with Real-ESRGAN, hd_textures.py --hud):
// out/enhanced/hud/<game>/<tpage>/<name>.png (not in git: made from the game's art) or
// game/assets/enhanced/hud/... (the Tripo icons, in git), a whole multiple of the game's size
// (scale), in the PS2's alpha (128 opaque). Unlike the level textures'
// replacements (baked into the levels), the game keeps addressing these in its own texels
bool enhanced_hd_screen_texture(const tfrag3::Texture& tex, std::vector<u32>& out, int& scale);

// Jak 3's pause map: the window the game uploads (256 x 416 texels from column strip * 256, row
// row, of world map `map`, map_w x map_h) cut from game/assets/enhanced/bigmap/jak3/mapNN.jpg (a
// whole multiple of the map's size, scale), PS2 alpha. False when that map has no HD picture
bool enhanced_bigmap_window(int map,
                            int map_w,
                            int map_h,
                            int strip,
                            int row,
                            std::vector<u32>& out,
                            int& scale);

void enhanced_set_texel_scale(GLuint tex, float scale);
void enhanced_forget_texture(GLuint tex);
float enhanced_texel_scale(GLuint tex);
