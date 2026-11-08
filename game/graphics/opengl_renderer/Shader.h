#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/versions/versions.h"

class Shader {
 public:
  static constexpr char shader_folder[] = "game/graphics/opengl_renderer/shaders/";
  Shader(const std::string& shader_name, GameVersion version);
  Shader() = default;
  void activate() const;
  bool okay() const { return m_is_okay; }
  const std::string& name() const { return m_name; }
  u64 id() const { return m_program; }
  // Enhanced TAA: NDC offset added to gl_Position (only for the 3D world's shaders)
  void set_jitter(float x, float y) const;
  // Enhanced dev loop: the fragment file the shader is built from (shrub_inst and shrub_grass
  // share shrub.frag)
  static std::string frag_file(const std::string& shader_name);
  // its `uniform ... tune_<name> = ...;` take "shaders.tune_<name>" from the tuning file
  // (EnhancedTune.h), sent again only when the file changed (activate)
  void apply_tuning() const;
  // the uniform values it holds now go to `fresh` (a reloaded copy); false (and a log line) when
  // one sits at another location there, which a renderer that looked it up once would miss
  bool hand_uniforms_to(const Shader& fresh, bool require_same_locations) const;

 private:
  struct TuneUniform {
    int loc = -1;
    unsigned type = 0;
    int n = 1;  // components
    float def[4] = {0, 0, 0, 0};
    std::string key;
  };
  std::vector<TuneUniform> m_tune;
  mutable int m_tune_gen = -1;
  std::string m_name;
  u64 m_frag_shader = 0;
  u64 m_vert_shader = 0;
  u64 m_program = 0;
  bool m_is_okay = false;
  bool m_jitters = false;
  int m_jitter_loc = -1;
};

// note: update the constructor in Shader.cpp
enum class ShaderId {
  SOLID_COLOR = 0,
  DIRECT_BASIC = 1,
  DIRECT_BASIC_TEXTURED = 2,
  DEBUG_RED = 3,
  SKY = 4,
  SKY_BLEND = 5,
  TFRAG3 = 6,
  TFRAG3_NO_TEX = 7,
  SPRITE = 8,
  SPRITE3 = 9,
  DIRECT2 = 10,
  EYE = 11,
  GENERIC = 12,
  OCEAN_TEXTURE = 13,
  OCEAN_TEXTURE_MIPMAP = 14,
  OCEAN_COMMON = 15,
  SHADOW = 16,
  SHRUB = 17,
  COLLISION = 18,
  MERC2 = 19,
  SPRITE_DISTORT = 20,
  SPRITE_DISTORT_INSTANCED = 21,
  POST_PROCESSING = 22,
  DEPTH_CUE = 23,
  EMERC = 24,
  GLOW_PROBE = 25,
  GLOW_PROBE_READ = 26,
  GLOW_PROBE_READ_DEBUG = 27,
  GLOW_PROBE_DOWNSAMPLE = 28,
  GLOW_DRAW = 29,
  ETIE_BASE = 30,
  ETIE = 31,
  SHADOW2 = 32,
  DIRECT_BASIC_TEXTURED_MULTI_UNIT = 33,
  TEX_ANIM = 34,
  GLOW_DEPTH_COPY = 35,
  GLOW_PROBE_ON_GRID = 36,
  HFRAG = 37,
  HFRAG_MONTAGE = 38,
  PLAIN_TEXTURE = 39,
  TIE_WIND = 40,
  SIMPLE_TEXTURE = 41,
  SLOW_TIME = 42,
  OCEAN_ENVMAP = 43,
  OCEAN_ENVMAP_HAZE = 44,
  ENH_BLOOM_DOWN = 45,
  ENH_BLOOM_UP = 46,
  ENH_COMPOSITE = 47,
  ENH_SSAO = 48,
  ENH_SSAO_BLUR = 49,
  ENH_SHAFTS = 50,
  ENH_GI = 51,
  ENH_GI_BLUR = 52,
  ENH_TAA = 53,
  ENH_VOLUME = 54,
  ENH_SHADOW = 55,
  ENH_SHADOW_MERC = 56,
  SHRUB_INST = 57,  // Enhanced: detailed scenery (EnhancedScenery)
  SHRUB_IMP = 58,   // Enhanced: its far pictures (impostors)
  SHRUB_GRASS = 59, // Enhanced: dense grass fields (EnhancedGrass)
  ENH_TERRAIN = 60, // Enhanced: a private level's own ground (EnhancedTerrain)
  ENH_HIZ = 61,     // Enhanced: the depth pyramid (post.hiz)
  ENH_SKY_MAP = 62, // Enhanced: the open sky straight up, traced (post.sky_map)
  MAX_SHADERS
};

class ShaderLibrary {
 public:
  ShaderLibrary(GameVersion version);
  Shader& operator[](ShaderId id) { return m_shaders[(int)id]; }
  Shader& at(ShaderId id) { return m_shaders[(int)id]; }
  void set_jitter(float x, float y);
  // Enhanced: test switch JAK_ENHANCED_SHADER_RELOAD=1 compiles the enh_* shaders again when
  // their files are saved (checked 4 times a second; a shader that fails keeps the old one);
  // JAK_ENHANCED_SHADER_RELOAD=all every shader (the game's own too: their uniform values are
  // carried over, and one whose uniforms moved keeps the old program, the log says "restart").
  // F5 (with the switch on) or the REPL's (pc-enhanced-getenv "JAK_ENHANCED_DO_RELOAD_SHADERS"
  // "" 0) rebuild them at once, switch or not. Called once a frame (EnhancedPost::begin_frame).
  void hot_reload();

 private:
  Shader m_shaders[(int)ShaderId::MAX_SHADERS];
  GameVersion m_version;
};
