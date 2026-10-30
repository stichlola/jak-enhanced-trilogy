#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/loader/common.h"

// Enhanced: a material for each level texture, from its name: the post pass gives metal and glossy
// surfaces (glass, marble) reflections and a highlight. Written by the background shaders in
// the normal target's alpha (2 bits): 1 plain, 2/3 metal, 1/3 glossy, 0 no normal; sand is plain
// with its normal written 0.9 long (the composite draws the wind's ripples on it), rock 0.75 (a
// tiling rock picture laid over it), rocky ground 0.6 (rock, and darker: the game paints it as pale
// as the sand).
enum EnhancedMaterial : u8 { kPlain = 0, kMetal = 1, kGlossy = 2, kSand = 3, kRock = 4, kGround = 5 };

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
  // test switch: JAK_ENHANCED_ROCK_TEX=<a,b,...> takes textures whose names contain one of those
  // as rock
  if (const char* extra = std::getenv("JAK_ENHANCED_ROCK_TEX")) {
    std::string list = std::string(extra) + ",";
    for (size_t at = 0, end; (end = list.find(',', at)) != std::string::npos; at = end + 1) {
      if (end > at && name.find(list.substr(at, end - at)) != std::string::npos) {
        return kRock;
      }
    }
  }
  // ("environment" holds "iron": the sky's and sea's environment maps aren't metal)
  for (size_t at; (at = name.find("environment")) != std::string::npos;) {
    name.replace(at, 11, "env");
  }
  // the ground's sand (not sandstone, not a sandbag, not Misty Island's quicksand: mud);
  // Sandover's beaches (bch-beach-01, vil-beach-01path, vil1-medres-beach: not its rocks and grass)
  if ((has({"sand"}) && !has({"stone", "bag", "rock", "wall", "brick", "quick"})) ||
      (has({"beach"}) && !has({"rock", "grass", "shrub", "plant", "leaf"}))) {
    return kSand;
  }
  // the desert's cracked rocky ground (its slopes)
  if (has({"wascity-ground"})) {
    return kGround;
  }
  // bare rock: cliffs, mountains, boulders (not rock-shrub plants)
  if (has({"cliff", "mount", "rock", "boulder", "sandstone"}) && !has({"shrub", "grass", "metal", "rust"})) {
    return kRock;
  }
  // and the rock named otherwise: the mines' and caves' walls, the nest's cave, the forest's and
  // the rubble's stones, the far hills and peaks seen from Haven (Jak 3); the caves of Volcanic
  // Crater, the drill platform, the atoll and the hilltops behind it (Jak 1 and 2)
  if (has({"minb-stone", "minc-stone", "comb-stone", "cav-stone", "rostone", "fora-stone-",
           "rub-stone-", "stalag", "cave-wall", "cave-sides", "cave-plain", "cave-carved",
           "background-peaks", "background-hills", "crater-rim", "cavewall", "cv-wall-", "dig-cave-broken",
           "dig-cave-floor", "cpad-stone", "cpad-cave-floor", "boss-stone", "cboss-lowres-stone",
           "atoll-r-stone", "background-hilltops", "nest-stonewall"}) &&
      !has({"brick", "tile", "shrub"})) {
    return kRock;
  }
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

// Enhanced: the characters' parts, from their textures' names (merc2.frag's enh_mat). A name not
// recognized stays kCharDefault: exactly the look it had. They are read whole words where a short
// word hides in longer ones: "arm" in "armor", "ear" in "gear", "hair" in "chair", "face" in
// "surface", "fur" in "furnace", "eye" in "faceyellow".
enum EnhancedCharMaterial : u8 {
  kCharDefault = 0,
  kCharSkin = 1,
  kCharHair = 2,
  kCharMetal = 3,
  kCharLeather = 4,
  kCharCloth = 5,
  kCharFur = 6,
  kCharEye = 7,
  kCharLens = 8
};

inline u8 enhanced_char_material(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
  auto has = [&](std::initializer_list<const char*> words) {
    for (auto w : words) {
      if (name.find(w) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  // whole words, between the name's dashes and digits ("jakc-arm-norm", "grunt-skin-01")
  std::vector<std::string> tokens(1);
  for (char c : name) {
    if (c >= 'a' && c <= 'z') {
      tokens.back().push_back(c);
    } else if (!tokens.back().empty()) {
      tokens.emplace_back();
    }
  }
  auto word = [&](std::initializer_list<const char*> words) {
    for (auto w : words) {
      if (std::find(tokens.begin(), tokens.end(), w) != tokens.end()) {
        return true;
      }
    }
    return false;
  };
  // reflections and animation targets are no surface of their own
  if (has({"environment", "envmap", "reflection", "-dest", "shadow", "glow"})) {
    return kCharDefault;
  }
  const bool daxter = has({"daxter", "mechdax"}) || word({"sk", "pecker"});

  // the eyes, and what's around them
  if (has({"eye"}) && !has({"eyell"})) {
    if (has({"brow"})) {
      return kCharHair;
    }
    if (has({"lid"})) {
      return has({"metal"}) ? kCharMetal : daxter ? kCharFur : kCharSkin;
    }
    return has({"light", "ring", "let"}) ? kCharDefault : kCharEye;
  }
  if (has({"lens", "glass"})) {
    return kCharLens;
  }
  if (has({"teeth", "tooth", "tongue", "gem", "crystal"})) {
    return kCharDefault;
  }

  // Daxter is all fur but for what he wears
  if (has({"daxter"})) {
    if (has({"bolt"})) {
      return kCharMetal;
    }
    if (has({"goggle", "helmet", "strap", "belt", "glove"})) {
      return kCharLeather;
    }
    if (has({"pants"})) {
      return kCharCloth;
    }
    return kCharFur;
  }
  if (has({"fur", "wool", "plume"}) && !has({"furn"})) {
    return kCharFur;
  }
  if (has({"hair", "beard", "mustache", "moustache", "ponytail"}) && !has({"chair"})) {
    return kCharHair;
  }
  if (has({"skin", "flesh"}) && !has({"door"})) {
    return kCharSkin;
  }

  // straps and leather first: "jakc-chestplate-straps" isn't the plate
  if (has({"leather", "strap", "lthr"})) {
    return kCharLeather;
  }
  if ((has({"metal", "steel", "chrome", "armor", "armour", "buckle", "rivet", "brass", "copper", "bronze",
            "silver", "shield", "wristguard", "kneeguard", "helmet", "headgear", "sword", "gunbarrel", "guntube",
            "gunhandle", "gunbolt", "pipe", "chain"}) &&
       !has({"rust", "rubber"})) ||
      word({"iron", "gold", "gun", "bolt", "bolts", "blade", "clip", "clips"}) ||
      (has({"plate"}) && !has({"template"}))) {
    return kCharMetal;
  }
  if (has({"belt", "pouch", "glove", "shoe", "boot", "holster", "wristband", "sandal", "harness", "flap", "goggle"}) ||
      word({"horn", "spat", "lightbrownspat"})) {
    return kCharLeather;
  }
  if (has({"cloth", "scarf", "pants", "skirt", "wrap", "shirt", "sleeve", "jacket", "tunic", "waistband", "legg",
           "fabric", "towel", "bandana", "jeans", "vest", "coat", "underwear", "undergarment", "diaper", "corset",
           "headband", "pantleg", "inseam"}) ||
      word({"rag", "robe", "cape", "hood", "sash", "sock"})) {
    return kCharCloth;
  }
  // bare skin (a face on a door or a nose on a ship isn't)
  if (word({"face", "facelft", "facert", "faceleft", "faceright", "arm", "forearm", "armlft", "finger", "fingers",
            "fingertop", "fingerbottom", "hand", "handback", "handpalm", "handtop", "handbottom", "head", "neck",
            "nose", "ear", "ears", "lip", "lips", "leg", "legs", "foot", "feet", "toe", "belly", "chest", "thigh",
            "thighs"}) &&
      !has({"door", "panel", "cube", "wheel", "gate", "vehicle", "bot-", "robo", "mech", "turret", "ship", "saw",
            "missle", "catapult", "wood", "idol", "pipe"})) {
    return kCharSkin;
  }
  return kCharDefault;
}

// the level's character materials by texture index (computed on first use)
inline const std::vector<u8>& enhanced_level_char_materials(const LevelData* lev) {
  auto& m = lev->enh_char_materials;
  if (m.size() != lev->level->textures.size()) {
    m.clear();
    for (const auto& tex : lev->level->textures) {
      m.push_back(enhanced_char_material(tex.debug_name));
    }
  }
  return m;
}

// the level's materials by texture index (computed on first use)
inline const std::vector<u8>& enhanced_level_materials(const LevelData* lev) {
  auto& m = lev->enh_materials;
  if (m.size() != lev->level->textures.size()) {
    m.clear();
    int count[6] = {0, 0, 0, 0, 0, 0};
    // (JAK_ENHANCED_MATERIAL_NAMES=1: each texture's name and material in the log)
    static const bool names = std::getenv("JAK_ENHANCED_MATERIAL_NAMES") != nullptr;
    for (const auto& tex : lev->level->textures) {
      m.push_back(enhanced_material(tex.debug_name));
      count[m.back()]++;
      if (names) {
        lg::info("[enhanced] material {} {}: {}", lev->level->level_name, tex.debug_name, m.back());
      }
    }
    // (in the log: a level whose rock or sand isn't named so misses the sun on the rock, the
    // sand's ripples and grade)
    lg::info("[enhanced] materials {}: {} rock, {} rocky ground, {} sand, {} metal, {} glossy, {} plain",
             lev->level->level_name, count[kRock], count[kGround], count[kSand], count[kMetal],
             count[kGlossy], count[kPlain]);
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
