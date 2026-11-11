#include "EnhancedPbr.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/runtime.h"

#include "third-party/stb_image/stb_image.h"

bool g_enhanced_pbr = [] {
  const char* e = std::getenv("JAK_ENHANCED_PBR");
  return !(e && std::atof(e) <= 0.0);
}();

namespace {
std::mutex g_index_mutex;
bool g_index_read = false;
fs::path g_maps_dir;
std::unordered_map<std::string, std::string> g_index;  // "tpage/name" -> the map's file (its key)

void read_index() {
  std::lock_guard<std::mutex> lock(g_index_mutex);
  if (g_index_read) {
    return;
  }
  g_index_read = true;
  // (JAK_ENHANCED_PBR=0 at boot: none read, the game exactly as before)
  const char* e = std::getenv("JAK_ENHANCED_PBR");
  if (e && std::atof(e) <= 0.0) {
    return;
  }
  const auto dir = file_util::get_jak_project_dir() / "out" / "enhanced" / "pbr" /
                   game_version_names[g_game_version];
  std::ifstream in((dir / "index.txt").string());
  if (!in) {
    lg::info("[enhanced] pbr: no maps (scripts/enhanced/pbr_maps.py, made with the HD textures)");
    return;
  }
  g_maps_dir = dir / "maps";
  std::string name, key;
  while (in >> name >> key) {
    g_index[name] = key;
  }
  lg::info("[enhanced] pbr: {} textures with maps", g_index.size());
}
}  // namespace

std::vector<EnhPbrPixels> enhanced_pbr_read(const tfrag3::Level& level) {
  read_index();
  std::vector<EnhPbrPixels> out(level.textures.size());
  if (g_index.empty()) {
    return out;
  }
  int found = 0, wrong = 0;
  for (size_t i = 0; i < level.textures.size(); i++) {
    const auto& tex = level.textures[i];
    const auto it = g_index.find(tex.debug_tpage_name + "/" + tex.debug_name);
    if (it == g_index.end()) {
      continue;
    }
    int w = 0, h = 0, n = 0;
    u8* px = stbi_load((g_maps_dir / (it->second + ".png")).string().c_str(), &w, &h, &n, 4);
    if (!px) {
      continue;
    }
    // half the HD texture's size: a texture whose HD version the decompiler didn't take (its name
    // shared with another picture) is at the game's size, and the map isn't its
    if (tex.w == 2 * w && tex.h == 2 * h) {
      out[i].w = w;
      out[i].h = h;
      out[i].px.resize((size_t)w * h);
      std::memcpy(out[i].px.data(), px, out[i].px.size() * 4);
      found++;
    } else {
      wrong++;
    }
    stbi_image_free(px);
  }
  if (found || wrong) {
    lg::info("[enhanced] pbr {}: {} maps of {} textures ({} not matching)", level.level_name, found,
             level.textures.size(), wrong);
  }
  return out;
}

namespace {
GLuint upload(EnhPbrPixels& p) {
  if (p.px.empty()) {
    return 0;
  }
  GLuint gl = 0;
  glActiveTexture(GL_TEXTURE0);
  glGenTextures(1, &gl);
  glBindTexture(GL_TEXTURE_2D, gl);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, p.w, p.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, p.px.data());
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  float aniso = 0.0f;
  glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
  glBindTexture(GL_TEXTURE_2D, 0);
  std::vector<u32>().swap(p.px);
  return gl;
}
}  // namespace

int enhanced_pbr_upload_next(LevelData& lev) {
  const size_t i = lev.enh_pbr.size();
  if (i >= lev.textures.size()) {
    return 0;
  }
  GLuint gl = 0;
  int bytes = 0;
  if (i < lev.enh_pbr_pending.size()) {
    bytes = lev.enh_pbr_pending[i].w * lev.enh_pbr_pending[i].h * 4;
    gl = upload(lev.enh_pbr_pending[i]);
  }
  lev.enh_pbr.push_back(gl);
  if (lev.enh_pbr.size() == lev.textures.size() && lev.enh_pbr.size() >= lev.level->textures.size()) {
    std::vector<EnhPbrPixels>().swap(lev.enh_pbr_pending);
  }
  return bytes;
}

void enhanced_pbr_upload_all(LevelData& lev) {
  while (lev.enh_pbr.size() < lev.textures.size()) {
    enhanced_pbr_upload_next(lev);
  }
}
