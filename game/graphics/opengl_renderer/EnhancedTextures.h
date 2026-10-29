#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/loader/common.h"

/*!
 * Jak Enhanced: the editor's Textures tool. A click on the world finds the triangle under the
 * cursor in the loaded levels (tfrag, tie, wind tie, shrub) and lists the textures of what it
 * belongs to (a tie's or shrub's whole proto, the background's around the point); each may be
 * replaced, edited in the user's paint program or recolored. The user's versions are PNGs in
 * custom_assets/<game>/texture_edits/<tpage>/<name>.png (plain 0..255 alpha), put on the GPU at
 * once and whenever a level loads (not the decompiler's texture_replacements: those must look like
 * the original and are baked into the levels).
 */

struct EnhancedTexRef {
  std::string tpage, name;  // a level texture's (debug names)
  GLuint gl = 0;            // a loaded copy of it (thumbnails)
  int w = 0, h = 0;
};

struct EnhancedTexPick {
  std::string what;  // "Prop: palm-a", "Background", "Shrub: bush-b"
  std::vector<EnhancedTexRef> textures;  // the one under the cursor first
};

// the textures of what's at p (game units, the depth's point): the nearest triangle within margin
// (game units). False: nothing textured there
bool enhanced_pick_world_textures(const std::vector<LevelData*>& levels,
                                  const float p[3],
                                  float margin,
                                  EnhancedTexPick* out);

// the game's own picture of it (RGBA8, plain alpha), from a loaded level
bool enhanced_world_texture_original(const std::vector<LevelData*>& levels,
                                     const std::string& tpage,
                                     const std::string& name,
                                     std::vector<u8>* rgba,
                                     int* w,
                                     int* h);

// rgba (plain alpha) on every loaded copy of it; null: the game's own back
void enhanced_world_texture_upload(const std::vector<LevelData*>& levels,
                                   const std::string& tpage,
                                   const std::string& name,
                                   const u8* rgba,
                                   int w,
                                   int h);

// where the user's version of it is kept, and whether there is one (the set read once, kept up to
// date by enhanced_note_texture_edit)
fs::path enhanced_texture_edit_path(const std::string& tpage, const std::string& name);
bool enhanced_has_texture_edit(const std::string& tpage, const std::string& name);
void enhanced_note_texture_edit(const std::string& tpage, const std::string& name, bool exists);

// the loader's: the user's version of a level texture, in the level's alpha (PS2's, 128 opaque, if
// the game's is), else false
bool enhanced_texture_edit(const tfrag3::Texture& tex, std::vector<u32>& out, int& w, int& h);

// a file opened with the program the system has for it (a PNG: the user's paint program)
void enhanced_open_file(const fs::path& path);
