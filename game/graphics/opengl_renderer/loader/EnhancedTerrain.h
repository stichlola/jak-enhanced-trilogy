#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/opengl_renderer/background/background_common.h"

// Enhanced: a private level's own ground, shaped in the editor: a grid of heights and of what each
// point is made of. Drawn, solid for Jak and casting shadows like the game's ground; the game's world
// is hidden while it's on (g_enhanced_menu.blank_world).
constexpr int kTerrainSize = 513;    // points per side
constexpr float kTerrainCell = 2.f;  // m between them
enum TerrainMat { kMatSand, kMatGrass, kMatRock, kMatDirt, kMatCount };

// (heights in m; mask: one u32 per point, the weights of grass, rock, dirt in its bytes 0..2, the rest
// sand). x0, z0: the corner, m. An empty list: flat at y0
void enhanced_terrain_set(float x0,
                          float z0,
                          float y0,
                          const std::vector<float>& heights,
                          const std::vector<u32>& mask);
void enhanced_terrain_clear();
bool enhanced_terrain_on();
void enhanced_terrain_get(std::vector<float>* heights, std::vector<u32>* mask);
// from a point, m, along a direction: where it meets the ground. False: it doesn't
bool enhanced_terrain_ray(const float* from, const float* dir, float* out);
// the height at a point, m
float enhanced_terrain_height(float x, float z);

// a dab of a brush at x, z (m), of this radius, soft at its rim; amount: how much this frame.
// raise: m up (negative: down); smooth, flatten (to target, m): 0..1 of the way; paint: 0..1 of the way
// to all this material
enum TerrainBrush { kBrushRaise, kBrushSmooth, kBrushFlatten, kBrushPaint };
void enhanced_terrain_brush(TerrainBrush brush, float x, float z, float radius, float amount, float target, int mat);
// the grab brush: the ground as it is now is the start; each call moves it from there
void enhanced_terrain_grab_begin();
void enhanced_terrain_grab(float x, float z, float radius, float dy);

// the lakes: each x, z (m) somewhere in it, and its water's height (m). The water fills what's lower
// than that, around that point (from one bank to the other: no walls needed)
void enhanced_terrain_set_lakes(const std::vector<std::array<float, 3>>& lakes);
// drawn last of the level's own things: seen through, doesn't hide what's behind it
void enhanced_draw_terrain_water(const TfragRenderSettings& settings,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof);

void enhanced_draw_terrain(const tfrag3::ShrubTree& tree,
                           const TfragRenderSettings& settings,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof);
// into the bound shadow shader's vertex array, attribute 0
void enhanced_terrain_shadow_draw();
// the triangles for the game's collision (as enhanced_collide_tris; box, seg in game units)
int enhanced_terrain_collide(const float* box, const float* seg, float* out, int stride, int max);
