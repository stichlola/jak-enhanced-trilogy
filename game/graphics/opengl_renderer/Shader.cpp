#include "Shader.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/EnhancedTune.h"
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
                                "sky",         "shrub_inst",    "shrub_imp",
                                "shrub_grass"};
  for (const char* w : world) {
    if (name == w) {
      return true;
    }
  }
  return false;
}

// Enhanced: GLSL source must be plain ASCII for AMD's driver (a UTF-8 byte order mark, or an
// accented letter in a comment, fails the whole shader there; NVIDIA lets it through). Anything
// else becomes a space.
std::string ascii_only(const std::string& src) {
  std::string out = src;
  for (char& c : out) {
    if (static_cast<unsigned char>(c) >= 0x80) {
      c = ' ';
    }
  }
  return out;
}

// a uniform type's base (0 float, 1 int/bool/sampler, 2 unsigned, -1 unknown) and components
std::pair<int, int> uniform_layout(GLenum type) {
  switch (type) {
    case GL_FLOAT:
      return {0, 1};
    case GL_FLOAT_VEC2:
      return {0, 2};
    case GL_FLOAT_VEC3:
      return {0, 3};
    case GL_FLOAT_VEC4:
    case GL_FLOAT_MAT2:
      return {0, 4};
    case GL_FLOAT_MAT3:
      return {0, 9};
    case GL_FLOAT_MAT4:
      return {0, 16};
    case GL_FLOAT_MAT2x3:
    case GL_FLOAT_MAT3x2:
      return {0, 6};
    case GL_FLOAT_MAT2x4:
    case GL_FLOAT_MAT4x2:
      return {0, 8};
    case GL_FLOAT_MAT3x4:
    case GL_FLOAT_MAT4x3:
      return {0, 12};
    case GL_INT:
    case GL_BOOL:
      return {1, 1};
    case GL_INT_VEC2:
    case GL_BOOL_VEC2:
      return {1, 2};
    case GL_INT_VEC3:
    case GL_BOOL_VEC3:
      return {1, 3};
    case GL_INT_VEC4:
    case GL_BOOL_VEC4:
      return {1, 4};
    case GL_UNSIGNED_INT:
      return {2, 1};
    case GL_UNSIGNED_INT_VEC2:
      return {2, 2};
    case GL_UNSIGNED_INT_VEC3:
      return {2, 3};
    case GL_UNSIGNED_INT_VEC4:
      return {2, 4};
    case GL_SAMPLER_1D:
    case GL_SAMPLER_2D:
    case GL_SAMPLER_3D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_SHADOW:
    case GL_SAMPLER_1D_ARRAY:
    case GL_SAMPLER_2D_ARRAY:
    case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_SAMPLER_CUBE_SHADOW:
    case GL_SAMPLER_BUFFER:
    case GL_SAMPLER_2D_MULTISAMPLE:
    case GL_INT_SAMPLER_2D:
    case GL_INT_SAMPLER_BUFFER:
    case GL_UNSIGNED_INT_SAMPLER_2D:
    case GL_UNSIGNED_INT_SAMPLER_BUFFER:
      return {1, 1};
    default:
      return {-1, 0};
  }
}

// sets a uniform of the current program from floats (an int type from `iv`/`uv` when given,
// else from the floats rounded)
void set_uniform(GLint loc,
                 GLenum type,
                 const float* f,
                 const GLint* iv = nullptr,
                 const GLuint* uv = nullptr) {
  const auto [kind, n] = uniform_layout(type);
  GLint ibuf[4];
  GLuint ubuf[4];
  if (kind == 1 && !iv) {
    for (int k = 0; k < 4; k++) {
      ibuf[k] = (GLint)std::lround(f[k]);
    }
    iv = ibuf;
  }
  if (kind == 2 && !uv) {
    for (int k = 0; k < 4; k++) {
      ubuf[k] = (GLuint)std::max(0L, std::lround(f[k]));
    }
    uv = ubuf;
  }
  switch (type) {
    case GL_FLOAT_MAT2:
      glUniformMatrix2fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT3:
      glUniformMatrix3fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT4:
      glUniformMatrix4fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT2x3:
      glUniformMatrix2x3fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT3x2:
      glUniformMatrix3x2fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT2x4:
      glUniformMatrix2x4fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT4x2:
      glUniformMatrix4x2fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT3x4:
      glUniformMatrix3x4fv(loc, 1, GL_FALSE, f);
      return;
    case GL_FLOAT_MAT4x3:
      glUniformMatrix4x3fv(loc, 1, GL_FALSE, f);
      return;
    default:
      break;
  }
  if (kind == 0) {
    if (n == 1) {
      glUniform1fv(loc, 1, f);
    } else if (n == 2) {
      glUniform2fv(loc, 1, f);
    } else if (n == 3) {
      glUniform3fv(loc, 1, f);
    } else {
      glUniform4fv(loc, 1, f);
    }
  } else if (kind == 1) {
    if (n == 1) {
      glUniform1iv(loc, 1, iv);
    } else if (n == 2) {
      glUniform2iv(loc, 1, iv);
    } else if (n == 3) {
      glUniform3iv(loc, 1, iv);
    } else {
      glUniform4iv(loc, 1, iv);
    }
  } else if (kind == 2) {
    if (n == 1) {
      glUniform1uiv(loc, 1, uv);
    } else if (n == 2) {
      glUniform2uiv(loc, 1, uv);
    } else if (n == 3) {
      glUniform3uiv(loc, 1, uv);
    } else {
      glUniform4uiv(loc, 1, uv);
    }
  }
}
}  // namespace

Shader::Shader(const std::string& shader_name, GameVersion version) : m_name(shader_name) {
  const std::string height_scale = version == GameVersion::Jak1 ? "1.0" : "0.5";
  const std::string scissor_height = version == GameVersion::Jak1 ? "448.0" : "416.0";
  const std::string scissor_adjust = "512.0 / " + scissor_height;

  // read the shader source
  auto vert_src = ascii_only(file_util::read_text_file(
      file_util::get_file_path({shader_folder, vert_file(shader_name) + ".vert"})));
  // (Enhanced: the detailed scenery's instanced models are colored like the shrubs they replace)
  const std::string frag_name = frag_file(shader_name);
  auto frag_src = ascii_only(
      file_util::read_text_file(file_util::get_file_path({shader_folder, frag_name + ".frag"})));
  // (Enhanced: the cloud pass alone, without the composite's registers: more pixels in flight)
  // (and the cloud rays alone, enh_cloud_rays)
  if (shader_name == "enh_clouds" || shader_name == "enh_cloud_rays") {
    const auto nl = frag_src.find('\n');
    frag_src.insert(nl == std::string::npos ? 0 : nl + 1,
                    shader_name == "enh_clouds" ? "#define ENH_CLOUD_ONLY 1\n" : "#define ENH_RAYS_ONLY 1\n");
  }

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
        // a final grade for the levels that need it (the Jak 2 plaza: sit with Haven's palette)
        "uniform int enh_grade_on;\nuniform vec4 enh_grade;\n"
        "void main() {\n  if (enh_cut != 0 && enh_in_cut(enh_world)) discard;\n"
        "  enh_cut_frag_main();\n"
        "  if (enh_grade_on != 0) {\n"
        "    float l = dot(color.rgb, vec3(0.299, 0.587, 0.114));\n"
        "    color.rgb = mix(vec3(l), color.rgb, enh_grade.a) * enh_grade.rgb;\n"
        "  }\n}\n";
  }

  // Enhanced: the game's fragment shaders write their colour clamped to 0..1, as an RGBA8 frame
  // stores it. In a half-float frame (post.hdr_scene) the PS2 shaders' "color *= 2" kept up to 2.0
  // per fragment: an alpha past 1 made SRC_ALPHA / ONE_MINUS_SRC_ALPHA blends add light (1 - a < 0
  // takes it away), and HUD textures (the minimap) came out over-bright. Only additive sums of
  // several fragments go past 1 now, which is what the float frame is for. Shaders that copy the
  // frame itself (distortion, depth cue, glow probe reads) keep their values.
  // JAK_ENHANCED_FRAG_CLAMP=0 turns it off.
  {
    static const bool clamp_on = [] {
      const char* e = std::getenv("JAK_ENHANCED_FRAG_CLAMP");
      return !(e && e[0] == '0');
    }();
    static const char* const keep[] = {"sprite_distort", "sprite_distort_instanced", "slow_time",
                                       "depth_cue", "post_processing", "simple_texture",
                                       "glow_probe_read", "glow_probe_read_debug",
                                       "glow_probe_downsample", "glow_depth_copy"};
    bool skip = !clamp_on || shader_name.rfind("enh_", 0) == 0;
    for (const char* k : keep) {
      skip = skip || shader_name == k;
    }
    std::smatch out;
    if (!skip && std::regex_search(frag_src, out, std::regex("out\\s+vec4\\s+(\\w+)\\s*;"))) {
      const std::string var = out[1].str();
      frag_src = std::regex_replace(frag_src, std::regex("void\\s+main\\s*\\(\\s*\\)"),
                                    "void enh_unclamped_main()");
      frag_src += "\nvoid main() {\n  enh_unclamped_main();\n  " + var + " = clamp(" + var +
                  ", 0.0, 1.0);\n}\n";
    }
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
  // (a driver that gives no reason leaves it as it is: it was printed as whatever the stack held)
  char err[len] = "(no reason given by the driver)";

  glGetShaderiv(m_vert_shader, GL_COMPILE_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetShaderInfoLog(m_vert_shader, len, nullptr, err);
    lg::error("Failed to compile vertex shader {}:\n{}", shader_name.c_str(), err);
    glDeleteShader(m_vert_shader);  // (Enhanced: a failed hot reload leaves nothing behind)
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
    glDeleteShader(m_vert_shader);
    glDeleteShader(m_frag_shader);
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
    glDeleteShader(m_vert_shader);
    glDeleteShader(m_frag_shader);
    glDeleteProgram(m_program);
    m_program = 0;
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
  // (Enhanced test switch JAK_ENHANCED_SHADER_STATS: the enh_* programs' size as the driver built
  // them, in the log. NVIDIA's program binary carries its assembly, ending in a line such as
  // "# 2318 instructions, 63 R-regs": registers per pixel decide how many pixels run at once)
  static const bool shader_stats = std::getenv("JAK_ENHANCED_SHADER_STATS") != nullptr;
  if (shader_stats && shader_name.rfind("enh_", 0) == 0) {
    GLint blen = 0;
    glGetProgramiv(m_program, GL_PROGRAM_BINARY_LENGTH, &blen);
    if (blen > 0) {
      std::string bin((size_t)blen, '\0');
      GLenum fmt = 0;
      glGetProgramBinary(m_program, blen, nullptr, &fmt, bin.data());
      std::string stats;
      for (size_t at = bin.find("R-regs"); at != std::string::npos; at = bin.find("R-regs", at + 6)) {
        const size_t from = bin.rfind('#', at);
        if (from != std::string::npos && at - from < 80) {
          stats += (stats.empty() ? "" : " | ") + bin.substr(from + 1, at + 6 - from - 1);
        }
      }
      lg::info("[enhanced] shader {} stats: {}", shader_name,
               stats.empty() ? fmt::format("{} bytes, no assembly text", blen) : stats);
    }
  }
  if (m_jitters) {
    m_jitter_loc = glGetUniformLocation(m_program, "enh_jitter");
  }

  // Enhanced dev loop: the tunable uniforms (tune_*), with the values the shader gives them
  GLint count = 0;
  glGetProgramiv(m_program, GL_ACTIVE_UNIFORMS, &count);
  for (GLint i = 0; i < count; i++) {
    char name[128];
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(m_program, i, sizeof(name), nullptr, &size, &type, name);
    if (std::strncmp(name, "tune_", 5) != 0 || size != 1) {
      continue;
    }
    const auto [kind, n] = uniform_layout(type);
    if (kind < 0 || kind == 2 || n > 4 || type == GL_FLOAT_MAT2) {
      continue;  // (floats and ints, up to 4)
    }
    TuneUniform u;
    u.loc = glGetUniformLocation(m_program, name);
    u.type = type;
    u.n = n;
    u.key = std::string("shaders.") + name;
    if (kind == 0) {
      glGetUniformfv(m_program, u.loc, u.def);
    } else {
      GLint iv[4] = {0, 0, 0, 0};
      glGetUniformiv(m_program, u.loc, iv);
      for (int k = 0; k < 4; k++) {
        u.def[k] = (float)iv[k];
      }
    }
    m_tune.push_back(u);
  }
}

std::string Shader::frag_file(const std::string& shader_name) {
  if (shader_name == "enh_clouds" || shader_name == "enh_cloud_rays") {
    return "enh_composite";
  }
  return shader_name == "shrub_inst" || shader_name == "shrub_grass" ? "shrub" : shader_name;
}

std::string Shader::vert_file(const std::string& shader_name) {
  return shader_name == "enh_clouds" || shader_name == "enh_cloud_rays" ? "enh_composite" : shader_name;
}

void Shader::copy_uniforms_from(const Shader& src) const {
  if (m_copy_src != src.m_program || m_copy_dst != m_program) {
    m_copy.clear();
    m_copy_src = src.m_program;
    m_copy_dst = m_program;
    GLint count = 0;
    glGetProgramiv(m_program, GL_ACTIVE_UNIFORMS, &count);
    for (GLint idx = 0; idx < count; idx++) {
      char name[256];
      GLint size = 0;
      GLenum type = 0;
      glGetActiveUniform(m_program, idx, sizeof(name), nullptr, &size, &type, name);
      if (uniform_layout(type).first < 0) {
        continue;
      }
      std::string base = name;
      if (auto b = base.find('['); b != std::string::npos) {
        base.resize(b);
      }
      for (GLint e = 0; e < size; e++) {
        const std::string el = size > 1 ? base + "[" + std::to_string(e) + "]" : std::string(name);
        CopiedUniform c;
        c.dst_loc = glGetUniformLocation(m_program, el.c_str());
        c.src_loc = glGetUniformLocation(src.m_program, el.c_str());
        c.type = type;
        if (c.dst_loc >= 0 && c.src_loc >= 0) {
          m_copy.push_back(c);
        }
      }
    }
    lg::info("[enhanced] shader {}: {} uniforms copied from {}", m_name, m_copy.size(), src.m_name);
  }
  glUseProgram(m_program);
  for (const auto& c : m_copy) {
    float f[16];
    GLint i[16];
    GLuint u[16];
    const int kind = uniform_layout(c.type).first;
    if (kind == 0) {
      glGetUniformfv(src.m_program, c.src_loc, f);
    } else if (kind == 1) {
      glGetUniformiv(src.m_program, c.src_loc, i);
    } else {
      glGetUniformuiv(src.m_program, c.src_loc, u);
    }
    set_uniform(c.dst_loc, c.type, f, i, u);
  }
}

void Shader::apply_tuning() const {
  const int gen = enhanced_tune_generation();
  if (gen == m_tune_gen) {
    return;
  }
  m_tune_gen = gen;
  for (const auto& u : m_tune) {
    float v[4] = {u.def[0], u.def[1], u.def[2], u.def[3]};
    if (enhanced_tune_lookup(u.key.c_str(), v, u.n)) {
      lg::info("[enhanced] shader {}: {} = {:g} {:g} {:g} {:g}", m_name, u.key, v[0], v[1], v[2],
               v[3]);
    }
    set_uniform(u.loc, u.type, v);
  }
}

bool Shader::hand_uniforms_to(const Shader& fresh, bool require_same_locations) const {
  struct Value {
    GLint new_loc;
    GLenum type;
    float f[16];
    GLint i[16];
    GLuint u[16];
  };
  std::vector<Value> values;
  GLint count = 0;
  glGetProgramiv(m_program, GL_ACTIVE_UNIFORMS, &count);
  for (GLint idx = 0; idx < count; idx++) {
    char name[256];
    GLint size = 0;
    GLenum type = 0;
    glGetActiveUniform(m_program, idx, sizeof(name), nullptr, &size, &type, name);
    const auto [kind, n] = uniform_layout(type);
    if (kind < 0) {
      continue;
    }
    // (an array is "name[0]": each element has its own location)
    std::string base = name;
    if (auto b = base.find('['); b != std::string::npos) {
      base.resize(b);
    }
    for (GLint e = 0; e < size; e++) {
      const std::string el = size > 1 ? base + "[" + std::to_string(e) + "]" : std::string(name);
      const GLint old_loc = glGetUniformLocation(m_program, el.c_str());
      if (old_loc < 0) {
        continue;  // (in a uniform block)
      }
      const GLint new_loc = glGetUniformLocation(fresh.m_program, el.c_str());
      if (new_loc != old_loc && require_same_locations) {
        lg::warn("[enhanced] shader {}: uniform {} moved ({} -> {}), restart to see the change",
                 m_name, el, old_loc, new_loc);
        return false;
      }
      if (new_loc < 0) {
        continue;
      }
      Value v{};
      v.new_loc = new_loc;
      v.type = type;
      if (kind == 0) {
        glGetUniformfv(m_program, old_loc, v.f);
      } else if (kind == 1) {
        glGetUniformiv(m_program, old_loc, v.i);
      } else {
        glGetUniformuiv(m_program, old_loc, v.u);
      }
      values.push_back(v);
    }
  }
  glUseProgram(fresh.m_program);
  for (const auto& v : values) {
    set_uniform(v.new_loc, v.type, v.f, v.i, v.u);
  }
  return true;
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
  if (!m_tune.empty()) {
    apply_tuning();
  }
}

ShaderLibrary::ShaderLibrary(GameVersion version) : m_version(version) {
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
  at(ShaderId::ENH_TAA) = {"enh_taa", version};
  at(ShaderId::ENH_VOLUME) = {"enh_volume", version};
  at(ShaderId::ENH_SHADOW) = {"enh_shadow", version};
  at(ShaderId::ENH_SHADOW_MERC) = {"enh_shadow_merc", version};
  at(ShaderId::SHRUB_INST) = {"shrub_inst", version};
  at(ShaderId::ENH_TERRAIN) = {"enh_terrain", version};
  at(ShaderId::ENH_CLOUDS) = {"enh_clouds", version};
  at(ShaderId::ENH_CLOUD_RAYS) = {"enh_cloud_rays", version};
  at(ShaderId::SHRUB_IMP) = {"shrub_imp", version};
  at(ShaderId::SHRUB_GRASS) = {"shrub_grass", version};

  for (auto& shader : m_shaders) {
    ASSERT_MSG(shader.okay(), "error compiling shader");
  }
}

void ShaderLibrary::hot_reload() {
  // (unset: off; "all": the game's shaders too, not only the enh_* passes)
  static const char* env = std::getenv("JAK_ENHANCED_SHADER_RELOAD");
  static const bool on = env != nullptr;
  static const bool all = env && std::string(env) == "all";
  static int frames = 0;
  static std::filesystem::file_time_type last[(int)ShaderId::MAX_SHADERS]{};
  const bool forced = enhanced_take_shader_reload_request();
  if (!forced && (!on || ++frames < 15)) {
    return;
  }
  frames = 0;
  int reloaded = 0, failed = 0;
  const auto t0 = std::chrono::steady_clock::now();
  auto ms_since = [](std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  };
  for (auto& shader : m_shaders) {
    const auto& name = shader.name();
    const bool enh = name.rfind("enh_", 0) == 0;
    // (tex_anim: TextureAnimator keeps the program's id, a new one would never be used)
    if ((!enh && !all) || name == "tex_anim") {
      continue;
    }
    std::error_code ec;
    auto t = std::filesystem::last_write_time(
        file_util::get_file_path({Shader::shader_folder, Shader::frag_file(name) + ".frag"}), ec);
    const auto tv = std::filesystem::last_write_time(
        file_util::get_file_path({Shader::shader_folder, Shader::vert_file(name) + ".vert"}), ec);
    t = std::max(t, tv);
    auto& seen = last[&shader - m_shaders];
    if (!forced && seen == std::filesystem::file_time_type{}) {
      seen = t;
      continue;
    }
    if (!forced && t == seen) {
      continue;
    }
    seen = t;
    const auto s0 = std::chrono::steady_clock::now();
    Shader fresh(name, m_version);
    if (!fresh.okay()) {
      failed++;
      lg::error("[enhanced] shader {} failed, keeping the old one", name);
      continue;
    }
    // (the game's renderers look their uniforms up once: the new program must keep them where
    // they were, and gets the values they set; the enh_* passes look theirs up every frame)
    if (shader.okay() && !shader.hand_uniforms_to(fresh, !enh)) {
      glDeleteProgram(fresh.id());
      failed++;
      continue;
    }
    if (shader.okay()) {
      glDeleteProgram(shader.id());
    }
    shader = fresh;
    reloaded++;
    lg::info("[enhanced] shader {} reloaded ({:.0f} ms)", name, ms_since(s0));
  }
  glUseProgram(0);
  if (forced) {
    lg::info("[enhanced] shaders reloaded: {} ok, {} failed ({:.0f} ms)", reloaded, failed,
             ms_since(t0));
  }
}

void ShaderLibrary::set_jitter(float x, float y) {
  for (auto& shader : m_shaders) {
    shader.set_jitter(x, y);
  }
}
