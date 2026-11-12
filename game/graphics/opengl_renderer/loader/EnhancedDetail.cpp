#include "EnhancedDetail.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/graphics/opengl_renderer/loader/EnhancedMaterials.h"
#include "game/graphics/opengl_renderer/loader/EnhancedPbr.h"

#include "third-party/stb_image/stb_image.h"

bool g_enhanced_detail = true;
float g_enhanced_detail_sun[4] = {0.f, 1.f, 0.f, 0.f};

bool enhanced_detail_allowed() {
  static const bool allowed = [] {
    const char* e = std::getenv("JAK_ENHANCED_DETAIL");
    return !(e && std::atof(e) <= 0.0);
  }();
  return allowed;
}

namespace {
struct Material {
  std::string name;
  std::string file;
  float tile_m = 1.8f;
  float normal = 1.f, rough = 0.6f, ao = 0.6f;
  GLuint tex = 0;
  bool tried = false;  // (its picture read, or found missing)
};
struct Rule {
  std::vector<std::string> contains, nots;
  int need_class = -1;
  u8 material = 0;
};
struct Config {
  std::vector<Material> materials;
  std::vector<Rule> rules;
  u8 hfrag = 0;
};

std::mutex g_mutex;
bool g_read = false;
Config g_cfg;

fs::path dir() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "materials";
}

const Config& config() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_read) {
    return g_cfg;
  }
  g_read = true;
  if (!enhanced_detail_allowed()) {
    return g_cfg;
  }
  const auto path = dir() / "detail.json";
  if (!fs::exists(path)) {
    return g_cfg;
  }
  try {
    auto j = parse_commented_json(file_util::read_text_file(path), path.string());
    auto index = [&](const std::string& n) -> u8 {
      for (size_t i = 0; i < g_cfg.materials.size(); i++) {
        if (g_cfg.materials[i].name == n) {
          return (u8)(i + 1);
        }
      }
      return 0;
    };
    if (j.contains("materials")) {
      for (auto& [name, m] : j["materials"].items()) {
        Material mat;
        mat.name = name;
        mat.file = m.value("file", name + "_detail.png");
        mat.tile_m = m.value("tile_m", mat.tile_m);
        mat.normal = m.value("normal", mat.normal);
        mat.rough = m.value("rough", mat.rough);
        mat.ao = m.value("ao", mat.ao);
        g_cfg.materials.push_back(mat);
      }
    }
    if (j.contains("textures")) {
      for (auto& r : j["textures"]) {
        Rule rule;
        if (r.contains("contains")) {
          rule.contains = r["contains"].get<std::vector<std::string>>();
        }
        if (r.contains("not")) {
          rule.nots = r["not"].get<std::vector<std::string>>();
        }
        const std::string cls = r.value("class", std::string());
        if (cls == "sand") {
          rule.need_class = kSand;
        } else if (cls == "rock") {
          rule.need_class = kRock;
        } else if (cls == "ground") {
          rule.need_class = kGround;
        }
        rule.material = index(r.value("material", std::string()));
        if (rule.material) {
          g_cfg.rules.push_back(rule);
        }
      }
    }
    g_cfg.hfrag = index(j.value("hfrag", std::string()));
    lg::info("[enhanced] detail materials: {} ({} texture rules, hfrag {})", g_cfg.materials.size(),
             g_cfg.rules.size(), g_cfg.hfrag);
  } catch (const std::exception& e) {
    lg::warn("[enhanced] detail materials {}: {}", path.string(), e.what());
    g_cfg = Config();
  }
  return g_cfg;
}
}  // namespace

u8 enhanced_detail_for_texture(const std::string& name, u8 material_class) {
  const auto& cfg = config();
  if (cfg.rules.empty()) {
    return 0;
  }
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const auto& r : cfg.rules) {
    if (r.need_class >= 0 && r.need_class != (material_class & kEnhMaterialMask)) {
      continue;
    }
    bool hit = r.contains.empty();
    for (const auto& w : r.contains) {
      hit = hit || n.find(w) != std::string::npos;
    }
    for (const auto& w : r.nots) {
      hit = hit && n.find(w) == std::string::npos;
    }
    if (hit) {
      return r.material;
    }
  }
  return 0;
}

u8 enhanced_detail_hfrag() {
  return config().hfrag;
}

void enhanced_detail_bind(GLint loc_on, GLint loc_k, int unit, u8 id) {
  if (loc_on < 0) {
    return;
  }
  const auto& cfg = config();
  Material* m = id > 0 && id <= cfg.materials.size() && g_enhanced_detail
                    ? &const_cast<Material&>(cfg.materials[id - 1])
                    : nullptr;
  if (m && !m->tried) {
    // (render thread only: the picture read and uploaded on first use, with mips: far off its
    // normals average out flat, so it can't show its repeat there)
    m->tried = true;
    const auto path = dir() / m->file;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    if (px) {
      glGenTextures(1, &m->tex);
      glBindTexture(GL_TEXTURE_2D, m->tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.f);
      glGenerateMipmap(GL_TEXTURE_2D);
      stbi_image_free(px);
      lg::info("[enhanced] detail material {}: {} ({}x{})", m->name, path.string(), w, h);
    } else {
      lg::warn("[enhanced] detail material {}: can't read {}", m->name, path.string());
    }
  }
  if (!m || !m->tex) {
    glUniform1i(loc_on, 0);
    return;
  }
  // (2: the albedo may carry its roughness for the composite's GGX: only while PBR is on)
  glUniform1i(loc_on, g_enhanced_pbr ? 2 : 1);
  glUniform4f(loc_k, 1.f / (m->tile_m * 4096.f), m->normal, m->rough, m->ao);
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_2D, m->tex);
  glActiveTexture(GL_TEXTURE0);
}
