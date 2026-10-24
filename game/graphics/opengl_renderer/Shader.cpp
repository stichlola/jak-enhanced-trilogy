#include "Shader.h"

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/graphics/pipelines/opengl.h"

namespace {
// Enhanced TAA: the shaders that draw the 3D world into the frame get a sub-pixel jitter. Not the
// HUD, post effects, or anything drawing into an offscreen texture (texture animation, sky
// blend, eyes, ocean textures, glow probes).
bool enhanced_jitters(const std::string& name) {
  static const char* world[] = {"tfrag3",      "tfrag3_no_tex", "shrub",       "tie_wind",
                                "etie",        "etie_base",     "merc2",       "emerc",
                                "generic",     "hfrag",         "ocean_common", "sprite_3d",
                                "sprite3_3d",  "shadow2",       "glow_draw",   "collision",
                                "sky"};
  for (const char* w : world) {
    if (name == w) {
      return true;
    }
  }
  return false;
}
}  // namespace

Shader::Shader(const std::string& shader_name, GameVersion version) : m_name(shader_name) {
  const std::string height_scale = version == GameVersion::Jak1 ? "1.0" : "0.5";
  const std::string scissor_height = version == GameVersion::Jak1 ? "448.0" : "416.0";
  const std::string scissor_adjust = "512.0 / " + scissor_height;

  // read the shader source
  auto vert_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, shader_name + ".vert"}));
  auto frag_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, shader_name + ".frag"}));

  vert_src = std::regex_replace(vert_src, std::regex("HEIGHT_SCALE"), height_scale);
  vert_src = std::regex_replace(vert_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
  frag_src = std::regex_replace(frag_src, std::regex("SCISSOR_HEIGHT"), scissor_height);
  vert_src = std::regex_replace(vert_src, std::regex("SCISSOR_ADJUST"), "(" + scissor_adjust + ")");

  // Enhanced: background shaders pass their world position on, and the fragment shader drops
  // what lies inside the cut volumes when the level asks for it (enh_cut, background_common.cpp):
  // the passages from the Metal Head city and the port into the Jak 2 plaza (custom level
  // "piazza"). Volumes in GOAL meters.
  if (shader_name == "tfrag3" || shader_name == "shrub" || shader_name == "etie" ||
      shader_name == "etie_base") {
    vert_src = std::regex_replace(vert_src, std::regex("void\\s+main\\s*\\(\\s*\\)"),
                                  "void enh_cut_main()");
    vert_src += "\nout vec3 enh_world;\nvoid main() {\n  enh_world = position_in / 4096.0;\n"
                "  enh_cut_main();\n}\n";
    frag_src = std::regex_replace(frag_src, std::regex("void\\s+main\\s*\\(\\s*\\)"),
                                  "void enh_cut_frag_main()");
    frag_src +=
        "\nin vec3 enh_world;\nuniform int enh_cut;\n"
        "bool enh_in_cut(vec3 p) {\n"
        // the gap into the Metal Head city (mhcitya)
        "  if (p.x > 42.0 && p.x < 92.0 && p.y > 7.0 && p.y < 60.0 && p.z > 950.0 && p.z < 968.0)"
        " return true;\n"
        // the lane under the fallen tower into the port (ctyport)
        "  vec2 a = vec2(229.0, 1146.0), b = vec2(200.0, 1206.0);\n"
        "  vec2 ab = b - a; float t = clamp(dot(p.xz - a, ab) / dot(ab, ab), 0.0, 1.0);\n"
        "  return length(p.xz - (a + ab * t)) < 6.5 && p.y > 4.6 && p.y < 16.0;\n"
        "}\n"
        "void main() {\n  if (enh_cut != 0 && enh_in_cut(enh_world)) discard;\n"
        "  enh_cut_frag_main();\n}\n";
  }

  // Enhanced TAA: wrap main() to add the frame's sub-pixel jitter to the final position
  m_jitters = enhanced_jitters(shader_name);
  if (m_jitters) {
    vert_src = std::regex_replace(vert_src, std::regex("void\\s+main\\s*\\(\\s*\\)"),
                                  "void enh_original_main()");
    vert_src += "\nuniform vec2 enh_jitter;\nvoid main() {\n  enh_original_main();\n"
                "  gl_Position.xy += enh_jitter * gl_Position.w;\n}\n";
  }

  m_vert_shader = glCreateShader(GL_VERTEX_SHADER);
  const char* src = vert_src.c_str();
  glShaderSource(m_vert_shader, 1, &src, nullptr);
  glCompileShader(m_vert_shader);

  constexpr int len = 1024;
  int compile_ok;
  char err[len];

  glGetShaderiv(m_vert_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_vert_shader, len, nullptr, err);
    lg::error("Failed to compile vertex shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  m_frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
  src = frag_src.c_str();
  glShaderSource(m_frag_shader, 1, &src, nullptr);
  glCompileShader(m_frag_shader);

  glGetShaderiv(m_frag_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_frag_shader, len, nullptr, err);
    lg::error("Failed to compile fragment shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  m_program = glCreateProgram();
  glAttachShader(m_program, m_vert_shader);
  glAttachShader(m_program, m_frag_shader);
  glLinkProgram(m_program);

  glGetProgramiv(m_program, GL_LINK_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetProgramInfoLog(m_program, len, nullptr, err);
    lg::error("Failed to link shader {}:\n{}", shader_name.c_str(), err);
    m_is_okay = false;
    return;
  }

  // uniform samplers must be named matching the texture unit
  glUseProgram(m_program);
  for (int i = 1; i < 30; ++i) {
    std::string uniformName = "tex_T" + std::to_string(i);
    GLint texLoc = glGetUniformLocation(m_program, uniformName.c_str());
    if (texLoc != -1) {
      glUniform1i(texLoc, i);
    }
  }
  // assuming that the bones uniform block is always using binding point 1
  GLint bonesLoc = glGetUniformBlockIndex(m_program, "ub_bones");
  if (bonesLoc != -1) {
    glUniformBlockBinding(m_program, bonesLoc, 1);
  }

  glDeleteShader(m_vert_shader);
  glDeleteShader(m_frag_shader);
  m_is_okay = true;
  if (m_jitters) {
    m_jitter_loc = glGetUniformLocation(m_program, "enh_jitter");
  }
}

void Shader::set_jitter(float x, float y) const {
  if (m_jitters && m_is_okay && m_jitter_loc != -1) {
    glUseProgram(m_program);
    glUniform2f(m_jitter_loc, x, y);
  }
}

void Shader::activate() const {
  ASSERT(m_is_okay);
  glUseProgram(m_program);
}

ShaderLibrary::ShaderLibrary(GameVersion version) {
  at(ShaderId::SOLID_COLOR) = {"solid_color", version};
  at(ShaderId::DIRECT_BASIC) = {"direct_basic", version};
  at(ShaderId::DIRECT_BASIC_TEXTURED) = {"direct_basic_textured", version};
  at(ShaderId::DIRECT_BASIC_TEXTURED_MULTI_UNIT) = {"direct_basic_textured_multi_unit", version};
  at(ShaderId::DEBUG_RED) = {"debug_red", version};
  at(ShaderId::SPRITE) = {"sprite_3d", version};
  at(ShaderId::SKY) = {"sky", version};
  at(ShaderId::SKY_BLEND) = {"sky_blend", version};
  at(ShaderId::TFRAG3) = {"tfrag3", version};
  at(ShaderId::TFRAG3_NO_TEX) = {"tfrag3_no_tex", version};
  at(ShaderId::SPRITE3) = {"sprite3_3d", version};
  at(ShaderId::DIRECT2) = {"direct2", version};
  at(ShaderId::EYE) = {"eye", version};
  at(ShaderId::GENERIC) = {"generic", version};
  at(ShaderId::OCEAN_TEXTURE) = {"ocean_texture", version};
  at(ShaderId::OCEAN_TEXTURE_MIPMAP) = {"ocean_texture_mipmap", version};
  at(ShaderId::OCEAN_COMMON) = {"ocean_common", version};
  at(ShaderId::SHRUB) = {"shrub", version};
  at(ShaderId::SHADOW) = {"shadow", version};
  at(ShaderId::COLLISION) = {"collision", version};
  at(ShaderId::MERC2) = {"merc2", version};
  at(ShaderId::SPRITE_DISTORT) = {"sprite_distort", version};
  at(ShaderId::SPRITE_DISTORT_INSTANCED) = {"sprite_distort_instanced", version};
  at(ShaderId::POST_PROCESSING) = {"post_processing", version};
  at(ShaderId::DEPTH_CUE) = {"depth_cue", version};
  at(ShaderId::EMERC) = {"emerc", version};
  at(ShaderId::GLOW_PROBE) = {"glow_probe", version};
  at(ShaderId::GLOW_PROBE_READ) = {"glow_probe_read", version};
  at(ShaderId::GLOW_PROBE_READ_DEBUG) = {"glow_probe_read_debug", version};
  at(ShaderId::GLOW_PROBE_DOWNSAMPLE) = {"glow_probe_downsample", version};
  at(ShaderId::GLOW_DRAW) = {"glow_draw", version};
  at(ShaderId::ETIE_BASE) = {"etie_base", version};
  at(ShaderId::ETIE) = {"etie", version};
  at(ShaderId::SHADOW2) = {"shadow2", version};
  at(ShaderId::TEX_ANIM) = {"tex_anim", version};
  at(ShaderId::GLOW_DEPTH_COPY) = {"glow_depth_copy", version};
  at(ShaderId::GLOW_PROBE_ON_GRID) = {"glow_probe_on_grid", version};
  at(ShaderId::HFRAG) = {"hfrag", version};
  at(ShaderId::HFRAG_MONTAGE) = {"hfrag_montage", version};
  at(ShaderId::PLAIN_TEXTURE) = {"plain_texture", version};
  at(ShaderId::TIE_WIND) = {"tie_wind", version};
  at(ShaderId::SIMPLE_TEXTURE) = {"simple_texture", version};
  at(ShaderId::SLOW_TIME) = {"slow_time", version};
  at(ShaderId::OCEAN_ENVMAP) = {"ocean_envmap", version};
  at(ShaderId::OCEAN_ENVMAP_HAZE) = {"ocean_envmap_haze", version};
  at(ShaderId::ENH_BLOOM_DOWN) = {"enh_bloom_down", version};
  at(ShaderId::ENH_BLOOM_UP) = {"enh_bloom_up", version};
  at(ShaderId::ENH_COMPOSITE) = {"enh_composite", version};
  at(ShaderId::ENH_SSAO) = {"enh_ssao", version};
  at(ShaderId::ENH_SSAO_BLUR) = {"enh_ssao_blur", version};
  at(ShaderId::ENH_SHAFTS) = {"enh_shafts", version};
  at(ShaderId::ENH_GI) = {"enh_gi", version};
  at(ShaderId::ENH_GI_BLUR) = {"enh_gi_blur", version};

  for (auto& shader : m_shaders) {
    ASSERT_MSG(shader.okay(), "error compiling shader");
  }
}

void ShaderLibrary::set_jitter(float x, float y) {
  for (auto& shader : m_shaders) {
    shader.set_jitter(x, y);
  }
}
