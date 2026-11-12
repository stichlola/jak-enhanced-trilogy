#pragma once

// Enhanced: tileable PBR detail materials (game/assets/enhanced/materials/detail.json, pictures from
// scripts/enhanced/detail_materials.py). A material is one RGBA picture laid in world space over the
// level textures the config names (R G a normal in world X/Z, B a roughness offset, A AO): the game's
// texture keeps the color, the material adds fine relief, roughness and a little occlusion near the
// camera (tfrag3.frag for tfrag/TIE, hfrag.frag for the desert's dunes). Off: JAK_ENHANCED_DETAIL=0
// at boot (no config read, no pictures), the menu's "Sand detail material", tuning "detail.on" 0.

#include <string>

#include "common/common_types.h"

#include "game/graphics/pipelines/opengl.h"

// on this frame (menu and tuning; false at boot with JAK_ENHANCED_DETAIL=0). Set by the post pass
extern bool g_enhanced_detail;
// the key light in world space (xyz) and its strength (w, 0 when down): the dunes shade the fine normal by it
// themselves (hfrag.frag). Set by the post pass each frame
extern float g_enhanced_detail_sun[4];
// the boot switch alone (JAK_ENHANCED_DETAIL=0: false)
bool enhanced_detail_allowed();

// the material for a level texture: 0 none, else its index + 1 (any thread)
u8 enhanced_detail_for_texture(const std::string& name, u8 material_class);
// the material on the hfrag dunes (0 none)
u8 enhanced_detail_hfrag();

// render thread: binds material `id`'s picture on `unit` and sets the shader's
// `enh_detail` (int: 1 on) and `enh_detail_k` (vec4: 1 / tile in game units, normal, rough, ao).
// id 0, off, or a missing picture: enh_detail 0
void enhanced_detail_bind(GLint loc_on, GLint loc_k, int unit, u8 id);
