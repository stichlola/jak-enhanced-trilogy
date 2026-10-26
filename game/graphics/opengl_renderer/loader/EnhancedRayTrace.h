#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"

/*!
 * Jak 3 Enhanced: ray-traced reflections. When a level loads (on the loader thread), its static
 * geometry (tfrag and tie, the most detailed level of detail, already in world space) goes into a
 * bounding volume hierarchy the post pass can trace rays through on the GPU: reflections on water,
 * metal and glass still find what's off the screen or hidden behind something. Each triangle keeps
 * its texture's mean color and its first vertex's time-of-day color index, so a hit is shaded with
 * the game's own baked light at the current hour.
 */
struct EnhancedRtLevel {
  // 32 bytes: bounds, then (inner) the right child (the left one follows the node), or (leaf) the
  // first triangle with the count in the high byte of w... see enh_rt.glsl
  struct Node {
    float min[3];
    u32 a;  // inner: index of the right child; leaf: first triangle
    float max[3];
    u32 b;  // inner: 0; leaf: triangle count
  };
  // 48 bytes: a vertex and two edges, the texture's mean color (RGBA8), the time-of-day color
  struct Tri {
    float v0[3];
    u32 color;
    float e1[3];
    u32 tod;  // index into palettes
    float e2[3];
    u32 pad;
  };
  std::string name;
  std::vector<Node> nodes;
  std::vector<Tri> tris;
  // 8 palettes per time-of-day color, RGBA8 each (the PackedTimeOfDay of each tree, unpacked)
  std::vector<u32> palettes;
};

// build the level's hierarchy and keep it for the renderer (replaces one of the same name)
void enhanced_build_rt(const tfrag3::Level& level, const std::string& name);

// the hierarchy built for this level, if any, taken out of the registry (the renderer uploads it
// and drops the CPU copy)
std::unique_ptr<EnhancedRtLevel> enhanced_take_rt(const std::string& name);

// off: levels load without building it (JAK_ENHANCED_RT=0)
extern bool g_enhanced_rt_build;
