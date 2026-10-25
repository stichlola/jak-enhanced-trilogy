#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/loader/common.h"

// Enhanced: a material for each level texture, from its name: the post pass gives metal and glossy
// surfaces (glass, marble) reflections and a highlight. Written by the background shaders in
// the normal target's alpha (2 bits): 1 plain, 2/3 metal, 1/3 glossy, 0 no normal.
enum EnhancedMaterial : u8 { kPlain = 0, kMetal = 1, kGlossy = 2 };

inline u8 enhanced_material(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  auto has = [&](std::initializer_list<const char*> words) {
    for (auto w : words) {
      if (name.find(w) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  // rusty, old or dirty: rough, whatever it's made of
  if (has({"rust", "old", "dirt", "grime", "moss", "mud", "sand", "rock", "wood", "cloth"})) {
    return kPlain;
  }
  if (has({"glass", "marble", "wet", "puddle", "shiny", "polish", "crystal", "gem", "jewel"}) ||
      (has({"window"}) && !has({"frame", "sill"}))) {
    return kGlossy;
  }
  if (has({"metal", "steel", "pipe", "chrome", "iron", "grate", "gold", "copper", "brass", "bolt",
           "rivet", "silver", "alum", "tank"})) {
    return kMetal;
  }
  return kPlain;
}

// the level's materials by texture index (computed on first use)
inline const std::vector<u8>& enhanced_level_materials(const LevelData* lev) {
  auto& m = lev->enh_materials;
  if (m.size() != lev->level->textures.size()) {
    m.clear();
    for (const auto& tex : lev->level->textures) {
      m.push_back(enhanced_material(tex.debug_name));
    }
  }
  return m;
}

// the draw's material for the background shaders (enh_material uniform)
inline void enhanced_set_material(SharedRenderState* render_state,
                                  ShaderId shader,
                                  const std::vector<u8>* materials,
                                  int tex) {
  GLuint program = render_state->shaders[shader].id();
  static GLuint cached_program[2] = {0, 0};
  static GLint cached_loc[2] = {-1, -1};
  int slot = shader == ShaderId::TFRAG3 ? 0 : 1;
  if (cached_program[slot] != program) {
    cached_program[slot] = program;
    cached_loc[slot] = glGetUniformLocation(program, "enh_material");
  }
  u8 m = materials && tex >= 0 && tex < (int)materials->size() ? (*materials)[tex] : kPlain;
  glUniform1i(cached_loc[slot], m);
}
