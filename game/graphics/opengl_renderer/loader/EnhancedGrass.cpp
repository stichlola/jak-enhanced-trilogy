#include "EnhancedGrass.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <future>
#include <list>
#include <mutex>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"
#include "common/versions/versions.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/opengl_renderer/loader/EnhancedScenery.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/runtime.h"

float g_enhanced_grass = [] {
  const char* env = std::getenv("JAK_ENHANCED_GRASS");
  return env ? std::clamp((float)std::atof(env), 0.f, 1.f) : 1.f;
}();

namespace {

constexpr float kMeter = 4096.f;
constexpr float kStep = 0.5f;  // m between the baked floor heights
constexpr float kTile = 4.f;   // m, a tile's side
constexpr float kNoFloor = -1e7f;
// the levels of detail: blades per tile, segments per blade, reach (m)
constexpr std::array<int, 3> kBlades = {1536, 640, 160};
constexpr std::array<int, 3> kSegs = {6, 4, 2};
constexpr std::array<float, 3> kReach = {22.f, 48.f, 100.f};

struct Zone {
  float x = 0, z = 0, radius = 50;  // m
  float water = -1000.f;            // m
  float density = 1.f;
  std::string level;
  bool heightfield = false;  // its floor is the height field under everything (no level's own)
  // painted in photo mode (EnhancedNotes, kept in grass_paint/<name>.json): where it's lush, where
  // it has thinned away (x, z, the brush's radius, m). None: lush in the middle, thinning out
  // toward the radius
  std::string paint;
  std::vector<std::array<float, 3>> lush, edge;
  float fade = 12.f;  // m: past the lush strokes, how far it thins away where no edge stroke is near
  // made by the editor's brush (grass_paint/user_zones.json): baked from the kept floors, nearest
  // its height y (m), drawn with whichever shrub tree draws first
  bool user = false;
  float y = 0.f;
};

constexpr size_t kMaxZones = 512;  // (reserved: the zones never move while the loader reads them)
constexpr float kUserRadius = 40.f;

fs::path user_zones_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "grass_paint" / "user_zones.json";
}

// the painted grass's look (its strokes' own: one for all of them)
std::mutex g_style_mutex;
EnhancedGrassStyle g_style;
bool g_style_loaded = false;

void read_style(const nlohmann::json& j) {
  if (!j.contains("style") || g_style_loaded) {
    return;
  }
  const auto& s = j["style"];
  std::lock_guard<std::mutex> lock(g_style_mutex);
  g_style.height = s.value("height", 1.f);
  g_style.width = s.value("width", 1.f);
  g_style.density = s.value("density", 1.f);
  g_style.wind = s.value("wind", 1.f);
  for (auto [key, out] : {std::pair{"color", g_style.color}, std::pair{"dry", g_style.dry}}) {
    if (s.contains(key) && s[key].is_array() && s[key].size() == 3) {
      for (int c = 0; c < 3; c++) {
        out[c] = s[key][c].get<float>();
      }
    }
  }
  g_style_loaded = true;
}

fs::path paint_path(const std::string& name);

void read_paint(Zone& z) {
  if (z.paint.empty() || !file_util::file_exists(paint_path(z.paint).string())) {
    return;
  }
  const auto pp = paint_path(z.paint);
  auto pj = parse_commented_json(file_util::read_text_file(pp), pp.string());
  z.fade = pj.value("fade", z.fade);
  for (const char* key : {"lush", "edge"}) {
    for (const auto& q : pj.at(key)) {
      (key[0] == 'l' ? z.lush : z.edge)
          .push_back({q[0].get<float>(), q[1].get<float>(), q.size() > 2 ? q[2].get<float>() : 0.f});
    }
  }
  read_style(pj);
}

fs::path paint_path(const std::string& name) {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "grass_paint" /
         (name + ".json");
}

// game/assets/enhanced/grass.json, this game's zones (read once), then the brush's
std::vector<Zone>& zones() {
  static std::mutex mutex;
  static bool loaded = false;
  static std::vector<Zone> out;
  std::lock_guard<std::mutex> lock(mutex);
  if (loaded) {
    return out;
  }
  loaded = true;
  out.reserve(kMaxZones);
  const auto path = file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "grass.json";
  // (the brush's zones, after the listed ones)
  auto read_user = [&] {
    try {
      if (!file_util::file_exists(user_zones_path().string())) {
        return;
      }
      auto j = parse_commented_json(file_util::read_text_file(user_zones_path()), "user_zones.json");
      const std::string game = game_version_names[g_game_version];
      if (!j.contains(game)) {
        return;
      }
      for (const auto& e : j.at(game)) {
        if (out.size() >= kMaxZones) {
          break;
        }
        Zone z;
        z.user = true;
        z.x = e.value("x", 0.f);
        z.y = e.value("y", 0.f);
        z.z = e.value("z", 0.f);
        z.radius = e.value("radius", kUserRadius);
        z.paint = e.value("paint", std::string());
        read_paint(z);
        out.push_back(z);
      }
    } catch (const std::exception& e) {
      lg::error("[enhanced] user_zones.json: {}", e.what());
    }
  };
  if (!file_util::file_exists(path.string())) {
    read_user();
    return out;
  }
  try {
    auto j = parse_commented_json(file_util::read_text_file(path), path.string());
    const std::string game = game_version_names[g_game_version];
    if (j.contains(game)) {
      for (const auto& e : j.at(game)) {
        Zone z;
        z.x = e.value("x", z.x);
        z.z = e.value("z", z.z);
        z.radius = e.value("radius", z.radius);
        z.water = e.value("water", z.water);
        z.density = e.value("density", z.density);
        z.level = e.value("level", z.level);
        z.heightfield = e.value("floor", std::string()) == "heightfield";
        z.paint = e.value("paint", std::string());
        read_paint(z);
        out.push_back(z);
      }
    }
  } catch (const std::exception& e) {
    lg::error("[enhanced] grass.json: {}", e.what());
  }
  read_user();
  return out;
}

// a zone's field, kept with the shrub tree that draws it
struct GrassField {
  u64 id = 0;           // for its GL objects (render thread, keyed by this)
  float x0 = 0, z0 = 0;  // the first height's place (game units)
  int n = 0;            // heights per side
  std::vector<float> heights;  // game units, kNoFloor where there's none
  std::vector<float> lush;     // 0..1: how full and tall the grass is there (0: none)
  std::vector<float> tile_lush;  // the most of it in each 4 m tile (nt x nt, from the corner)
  int nt = 0;
  size_t zone = 0;
  float cx = 0, cz = 0, radius = 0, water = 0;  // game units
  float density = 1.f;
  int tod = 0;
  float base_rgb[3] = {0.5f, 0.5f, 0.5f};

  float height_at(float x, float z) const {
    int i = (int)std::lround((x - x0) / (kStep * kMeter));
    int k = (int)std::lround((z - z0) / (kStep * kMeter));
    if (i < 0 || k < 0 || i >= n || k >= n) {
      return kNoFloor;
    }
    return heights[k * n + i];
  }
};

std::atomic<u64> g_next_field = 1;

// how lush each of a field's spots (and the most of it in each tile): by the painting (full within
// the lush strokes, thinner and shorter toward the edge's, gone past them), else by the distance
// from the middle
struct Lush {
  std::vector<float> spots, tiles;
};
Lush bake_lush(const GrassField& f, const Zone& zone) {
  Lush out;
  out.spots.assign(f.n * f.n, 0.f);
  // (only the strokes near enough to matter)
  std::vector<std::array<float, 3>> lush, edge;
  for (const auto& q : zone.lush) {
    if (std::hypot(q[0] - zone.x, q[1] - zone.z) < zone.radius + 40.f) {
      lush.push_back(q);
    }
  }
  for (const auto& q : zone.edge) {
    if (std::hypot(q[0] - zone.x, q[1] - zone.z) < zone.radius + 40.f) {
      edge.push_back(q);
    }
  }
  const bool painted = !zone.paint.empty();
  // (a brush's zone: not where an earlier zone's field is, no grass twice)
  std::vector<std::array<float, 3>> before;
  if (zone.user) {
    const auto& all = zones();
    for (size_t zi = 0; zi < f.zone && zi < all.size(); zi++) {
      before.push_back({all[zi].x, all[zi].z, all[zi].radius});
    }
  }
  for (int k = 0; k < f.n; k++) {
    for (int i = 0; i < f.n; i++) {
      const float x = (f.x0 + i * kStep * kMeter) / kMeter, z = (f.z0 + k * kStep * kMeter) / kMeter;
      float v = 0.f;
      bool taken = false;
      for (const auto& b : before) {
        taken |= std::hypot(x - b[0], z - b[1]) < b[2] - 1.f;
      }
      if (taken) {
        continue;
      }
      if (!painted) {
        float t = std::clamp((std::hypot(x - zone.x, z - zone.z) / zone.radius - 0.7f) / 0.3f, 0.f, 1.f);
        v = 1.f - t * t * (3.f - 2.f * t);
      } else if (!lush.empty()) {
        float dl = 1e30f, de = 1e30f;
        for (const auto& q : lush) {
          dl = std::min(dl, std::hypot(q[0] - x, q[1] - z) - q[2]);
        }
        for (const auto& q : edge) {
          de = std::min(de, std::hypot(q[0] - x, q[1] - z) - q[2]);
        }
        dl = std::max(dl, 0.f);
        de = std::max(de, 0.f);
        // (the share of the way from the lush to the edge; and not much past the lush strokes
        // where no edge is near: over the fade)
        float t = std::clamp((dl / (dl + de + 1e-3f) - 0.3f) / 0.45f, 0.f, 1.f);
        // (an edge stroke near: as far as it, up to 60 m)
        float span = de < 60.f ? std::max(zone.fade, std::min(dl + de, 60.f)) : zone.fade;
        float beyond = std::clamp(dl / std::max(span, 0.5f), 0.f, 1.f);
        v = (1.f - t * t * (3.f - 2.f * t)) * (1.f - beyond * beyond * (3.f - 2.f * beyond));
      }
      out.spots[k * f.n + i] = v;
    }
  }
  out.tiles.assign(f.nt * f.nt, 0.f);
  for (int k = 0; k < f.n; k++) {
    for (int i = 0; i < f.n; i++) {
      // (each height counts for the tiles it touches)
      const float fx = i * kStep / kTile, fz = k * kStep / kTile;
      for (int dz = -1; dz <= 0; dz++) {
        for (int dx = -1; dx <= 0; dx++) {
          int ti = (int)std::floor(fx + dx * 0.01f), tk = (int)std::floor(fz + dz * 0.01f);
          if (ti >= 0 && tk >= 0 && ti < f.nt && tk < f.nt) {
            auto& m = out.tiles[tk * f.nt + ti];
            m = std::max(m, out.spots[k * f.n + i]);
          }
        }
      }
    }
  }
  return out;
}

// the painting changed in photo mode: the zones' strokes, a new version of them
std::mutex g_paint_mutex;
u64 g_paint_version = 0;
std::vector<std::array<float, 3>> g_paint_lush, g_paint_edge;
float g_paint_fade = 12.f;

}  // namespace
EnhancedGrassGround g_enhanced_grass_ground;
namespace {

// zones whose floor is a level without shrubs (desoasis: the oasis): baked there, drawn by the
// nearest shrub tree of any level (its time of day colors), by zone
struct Host {
  const tfrag3::ShrubTree* tree = nullptr;
  float dist = 1e30f;
  int tod = 0;
  float base_rgb[3] = {0.5f, 0.5f, 0.5f};
};
std::mutex g_orphan_mutex;
std::unordered_map<size_t, std::shared_ptr<GrassField>> g_orphans;
std::unordered_map<size_t, Host> g_hosts;
std::unordered_map<size_t, u64> g_user_drawn;  // a brush zone's field: the frame it was last drawn in

// the last few loaded levels' floors (for the brush's zones), and the bakes going on
std::mutex g_floor_mutex;
std::list<std::pair<std::string, GrassFloorNear>> g_floors;
constexpr size_t kKeptFloors = 10;
std::mutex g_bake_mutex;
std::list<std::future<void>> g_bakes;
std::unordered_map<size_t, u64> g_bake_asked;  // each brush zone's latest bake asked for (a count)
u64 g_bake_count = 0;

std::shared_ptr<GrassField> bake_user_field(size_t zi);

// a brush zone's field baked (again) on a worker: from all the floors kept now
void bake_user_zone(size_t zi) {
  std::lock_guard<std::mutex> lock(g_bake_mutex);
  const u64 ask = ++g_bake_count;
  g_bake_asked[zi] = ask;
  // (the finished ones dropped)
  for (auto it = g_bakes.begin(); it != g_bakes.end();) {
    if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      it = g_bakes.erase(it);
    } else {
      ++it;
    }
  }
  g_bakes.push_back(std::async(std::launch::async, [zi, ask] {
    auto field = bake_user_field(zi);
    {
      std::lock_guard<std::mutex> block(g_bake_mutex);
      if (g_bake_asked[zi] != ask) {
        return;  // (a newer one asked for)
      }
    }
    if (field) {
      std::lock_guard<std::mutex> olock(g_orphan_mutex);
      g_orphans[zi] = field;
    }
  }));
}

// the GL side (render thread): each field's heights texture, dropped when unused for a while
struct FieldGl {
  GLuint tex = 0;
  double last_used = 0;
  // repainted while playing: the new lushness being baked (on a worker), then in use
  u64 paint_version = 0;
  std::future<Lush> job;
  std::vector<float> tile_lush;
};
struct GrassGl {
  bool made = false;
  GLuint vao = 0, vbo = 0, white = 0;
  std::unordered_map<u64, FieldGl> fields;
  std::vector<math::Vector4f> tiles[3];
  std::vector<math::Vector4f> all;
};
GrassGl g_gl;

void make_gl() {
  g_gl.made = true;
  glGenVertexArrays(1, &g_gl.vao);
  glGenBuffers(1, &g_gl.vbo);
  glBindVertexArray(g_gl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(math::Vector4f), nullptr);
  glBindVertexArray(0);
  const u32 white = 0xffffffff;
  glGenTextures(1, &g_gl.white);
  glBindTexture(GL_TEXTURE_2D, g_gl.white);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void upload_field(GLuint tex, const GrassField& f, const std::vector<float>& lush) {
  // (r: the floor's height, g: how lush)
  std::vector<float> rg(f.heights.size() * 2);
  for (size_t i = 0; i < f.heights.size(); i++) {
    rg[i * 2] = f.heights[i];
    rg[i * 2 + 1] = lush.empty() ? 1.f : lush[i];
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, f.n, f.n, 0, GL_RG, GL_FLOAT, rg.data());
  glBindTexture(GL_TEXTURE_2D, 0);
}

FieldGl& field_gl(const std::shared_ptr<const GrassField>& fp, double now) {
  const auto& f = *fp;
  auto& gl = g_gl.fields[f.id];
  gl.last_used = now;
  // repainted: baked again on a worker (holding the field), swapped in when done
  if (!zones()[f.zone].paint.empty()) {
    if (gl.job.valid() && gl.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto l = gl.job.get();
      gl.tile_lush = std::move(l.tiles);
      if (gl.tex) {
        upload_field(gl.tex, f, l.spots);
      }
    }
    std::lock_guard<std::mutex> lock(g_paint_mutex);
    if (!gl.job.valid() && gl.paint_version != g_paint_version) {
      gl.paint_version = g_paint_version;
      Zone zone = zones()[f.zone];
      zone.lush = g_paint_lush;
      zone.edge = g_paint_edge;
      zone.fade = g_paint_fade;
      gl.job = std::async(std::launch::async, [fp, zone] { return bake_lush(*fp, zone); });
    }
  }
  if (!gl.tex) {
    glGenTextures(1, &gl.tex);
    upload_field(gl.tex, f, f.lush);
    glBindTexture(GL_TEXTURE_2D, gl.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  // (the fields of levels gone: their textures freed after 20 s unused)
  for (auto it = g_gl.fields.begin(); it != g_gl.fields.end();) {
    if (now - it->second.last_used > 20.0 && !it->second.job.valid()) {
      glDeleteTextures(1, &it->second.tex);
      it = g_gl.fields.erase(it);
    } else {
      ++it;
    }
  }
  return gl;
}
}  // namespace

bool enhanced_grass_painted(std::vector<std::array<float, 3>>* lush,
                            std::vector<std::array<float, 3>>* edge,
                            float* fade) {
  {
    std::lock_guard<std::mutex> lock(g_paint_mutex);
    if (g_paint_version > 0) {
      *lush = g_paint_lush;
      *edge = g_paint_edge;
      *fade = g_paint_fade;
      return true;
    }
  }
  // (every painted zone's strokes: each file has those near it, some twice)
  bool any = false;
  lush->clear();
  edge->clear();
  for (const auto& zone : zones()) {
    if (zone.paint.empty()) {
      continue;
    }
    for (int side = 0; side < 2; side++) {
      auto& out = side ? *edge : *lush;
      for (const auto& q : side ? zone.edge : zone.lush) {
        if (std::find(out.begin(), out.end(), q) == out.end()) {
          out.push_back(q);
        }
      }
    }
    if (!any) {
      *fade = zone.fade;
    }
    any = true;
  }
  return any;
}

void enhanced_grass_paint(const std::vector<std::array<float, 3>>& lush,
                          const std::vector<std::array<float, 3>>& edge,
                          float fade,
                          bool save) {
  {
    std::lock_guard<std::mutex> lock(g_paint_mutex);
    g_paint_lush = lush;
    g_paint_edge = edge;
    g_paint_fade = fade;
    g_paint_version++;
  }
  if (!save) {
    return;
  }
  // kept for the next start: each painted zone's strokes near it
  for (const auto& zone : zones()) {
    if (zone.paint.empty()) {
      continue;
    }
    nlohmann::json j;
    j["comment"] =
        "Painted in the in-game editor (EnhancedEditor): lush, the tall full grass; edge, where it has thinned "
        "away. Points x, z, the brush's radius (m, the game's world); fade: how far it thins away past "
        "the lush where no edge is near (m)";
    j["fade"] = std::round(fade * 10) / 10;
    {
      const auto s = enhanced_grass_style();
      j["style"] = {{"height", s.height},
                    {"width", s.width},
                    {"density", s.density},
                    {"wind", s.wind},
                    {"color", {s.color[0], s.color[1], s.color[2]}},
                    {"dry", {s.dry[0], s.dry[1], s.dry[2]}}};
    }
    for (const char* key : {"lush", "edge"}) {
      auto& arr = j[key] = nlohmann::json::array();
      for (const auto& q : key[0] == 'l' ? lush : edge) {
        if (std::hypot(q[0] - zone.x, q[1] - zone.z) < zone.radius + 40.f) {
          arr.push_back({std::round(q[0] * 10) / 10, std::round(q[1] * 10) / 10,
                         std::round(q[2] * 10) / 10});
        }
      }
    }
    try {
      file_util::create_dir_if_needed_for_file(paint_path(zone.paint));
      file_util::write_text_file(paint_path(zone.paint), j.dump());
    } catch (const std::exception& e) {
      lg::error("[enhanced] grass paint: {}", e.what());
    }
  }
}

bool enhanced_grass_zone_at(float x, float z) {
  for (const auto& zone : zones()) {
    // (as far as its painting is kept: grass_paint keeps strokes within 40 m of it)
    if (!zone.paint.empty() && std::hypot(x - zone.x, z - zone.z) < zone.radius + 40.f) {
      return true;
    }
  }
  return false;
}

bool enhanced_grass_has_zones() {
  return true;  // (the brush may make one anywhere)
}

bool enhanced_grass_field_at(float x, float z, float margin) {
  for (const auto& zone : zones()) {
    if (!zone.paint.empty() && std::hypot(x - zone.x, z - zone.z) < zone.radius - margin) {
      return true;
    }
  }
  return false;
}

namespace {
void save_user_zones() {
  nlohmann::json j = nlohmann::json::object();
  try {
    if (file_util::file_exists(user_zones_path().string())) {
      j = parse_commented_json(file_util::read_text_file(user_zones_path()), "user_zones.json");
    }
  } catch (const std::exception&) {
    j = nlohmann::json::object();
  }
  auto arr = nlohmann::json::array();
  for (const auto& z : zones()) {
    if (z.user) {
      arr.push_back({{"x", std::round(z.x * 10) / 10},
                     {"y", std::round(z.y * 10) / 10},
                     {"z", std::round(z.z * 10) / 10},
                     {"radius", z.radius},
                     {"paint", z.paint}});
    }
  }
  j[game_version_names[g_game_version]] = arr;
  try {
    file_util::create_dir_if_needed_for_file(user_zones_path());
    file_util::write_text_file(user_zones_path(), j.dump(1));
  } catch (const std::exception& e) {
    lg::error("[enhanced] user_zones.json: {}", e.what());
  }
}

std::shared_ptr<GrassField> bake_user_field(size_t zi) {
  const Zone zone = zones()[zi];
  std::vector<GrassFloorNear> floors;
  {
    std::lock_guard<std::mutex> lock(g_floor_mutex);
    for (const auto& f : g_floors) {
      floors.push_back(f.second);
    }
  }
  if (floors.empty()) {
    return nullptr;
  }
  const float cx = zone.x * kMeter, cz = zone.z * kMeter, r = zone.radius * kMeter;
  auto field = std::make_shared<GrassField>();
  field->id = g_next_field++;
  field->n = (int)std::ceil(2.f * zone.radius / kStep) + 1;
  field->x0 = cx - r;
  field->z0 = cz - r;
  field->heights.assign(field->n * field->n, kNoFloor);
  // (each height the floor nearest its neighbor's: up and down the slopes from the middle's)
  int filled = 0;
  std::vector<float> row_ref(field->n, zone.y * kMeter);
  for (int k = 0; k < field->n; k++) {
    float ref = row_ref[0];
    for (int i = 0; i < field->n; i++) {
      const float x = field->x0 + i * kStep * kMeter, z = field->z0 + k * kStep * kMeter;
      const float want = i > 0 ? ref : row_ref[i];
      float best = 0, y;
      bool found = false;
      for (const auto& fl : floors) {
        if (fl(x, z, want, &y) && (!found || std::abs(y - want) < std::abs(best - want))) {
          best = y;
          found = true;
        }
      }
      // (a jump of more than 3 m: another layer, not this floor)
      if (found && std::abs(best - want) < 3.f * kMeter) {
        field->heights[k * field->n + i] = best;
        ref = best;
        row_ref[i] = best;
        filled++;
      } else {
        ref = row_ref[i];
      }
    }
  }
  field->nt = (int)std::ceil(2.f * zone.radius / kTile);
  field->zone = zi;
  {
    Zone painted = zone;
    std::lock_guard<std::mutex> lock(g_paint_mutex);
    if (g_paint_version > 0) {
      painted.lush = g_paint_lush;
      painted.edge = g_paint_edge;
      painted.fade = g_paint_fade;
    }
    auto l = bake_lush(*field, painted);
    field->lush = std::move(l.spots);
    field->tile_lush = std::move(l.tiles);
  }
  field->cx = cx;
  field->cz = cz;
  field->radius = r;
  field->water = zone.water * kMeter;
  field->density = zone.density;
  lg::info("[enhanced] grass: the brush's zone {} at {:.0f}, {:.0f} baked ({} of {} heights on a floor, {} "
           "levels' floors)",
           zi, zone.x, zone.z, filled, field->n * field->n, floors.size());
  return field;
}
}  // namespace

void enhanced_grass_keep_floor(const std::string& level, GrassFloorNear floor) {
  {
    std::lock_guard<std::mutex> lock(g_floor_mutex);
    g_floors.remove_if([&](const auto& f) { return f.first == level; });
    g_floors.emplace_back(level, std::move(floor));
    while (g_floors.size() > kKeptFloors) {
      g_floors.pop_front();
    }
  }
  // (the brush's zones near enough: baked again with this floor too)
  auto& all = zones();
  const size_t n = all.size();
  for (size_t zi = 0; zi < n; zi++) {
    if (all[zi].user) {
      bake_user_zone(zi);
    }
  }
}

bool enhanced_grass_add_zone(float x, float y, float z) {
  auto& all = zones();
  if (all.size() >= kMaxZones) {
    return false;
  }
  Zone zone;
  zone.user = true;
  zone.x = std::round(x * 10) / 10;
  zone.y = y;
  zone.z = std::round(z * 10) / 10;
  zone.radius = kUserRadius;
  zone.paint = fmt::format("user_{}_{:.0f}_{:.0f}", game_version_names[g_game_version], zone.x, zone.z);
  all.push_back(zone);
  save_user_zones();
  lg::info("[enhanced] grass: a new zone for the brush at {:.0f}, {:.0f} ({} m around)", zone.x, zone.z, kUserRadius);
  bake_user_zone(all.size() - 1);
  return true;
}

void enhanced_grass_counts(int* zone_count, int* user_zones, int* fields) {
  *zone_count = (int)zones().size();
  *user_zones = 0;
  for (const auto& z : zones()) {
    *user_zones += z.user;
  }
  std::lock_guard<std::mutex> lock(g_orphan_mutex);
  *fields = (int)g_orphans.size();
}

EnhancedGrassStyle enhanced_grass_style() {
  std::lock_guard<std::mutex> lock(g_style_mutex);
  return g_style;
}

void enhanced_set_grass_style(const EnhancedGrassStyle& style) {
  std::lock_guard<std::mutex> lock(g_style_mutex);
  g_style = style;
  g_style_loaded = true;
}

void enhanced_prepare_grass(tfrag3::Level& level, const std::string& name, const GrassFloor& floor) {
  for (size_t zi = 0; zi < zones().size(); zi++) {
    const auto& zone = zones()[zi];
    if (zone.user) {
      continue;  // (baked from the kept floors: enhanced_grass_keep_floor)
    }
    const float cx = zone.x * kMeter, cz = zone.z * kMeter, r = zone.radius * kMeter;
    // (any level's shrubs may draw a field baked in a level without any: the nearest tree)
    Host mine;
    for (size_t t = 0; t < level.shrub_trees.size(); t++) {
      const auto& verts = level.shrub_trees[t].unpacked.vertices;
      for (size_t v = 0; v < verts.size(); v += 3) {
        float d = std::hypot(verts[v].x - cx, verts[v].z - cz);
        if (d < mine.dist) {
          mine.tree = &level.shrub_trees[t];
          mine.dist = d;
          mine.tod = verts[v].color_index;
          for (int c = 0; c < 3; c++) {
            mine.base_rgb[c] = verts[v].rgba_base[c] / 255.f;
          }
        }
      }
    }
    if (mine.tree) {
      std::lock_guard<std::mutex> lock(g_orphan_mutex);
      auto& host = g_hosts[zi];
      if (mine.dist < host.dist) {
        host = mine;
      }
    }
    if (!zone.level.empty() && zone.level != name) {
      continue;
    }
    // the level's own floor over the zone (not just the height field under everything): a few
    // hundred spots
    int inside = 0, own = 0, any = 0;
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < 24; i++) {
      for (int k = 0; k < 24; k++) {
        float x = cx + ((i + 0.5f) / 24.f * 2.f - 1.f) * r;
        float z = cz + ((k + 0.5f) / 24.f * 2.f - 1.f) * r;
        if (std::hypot(x - cx, z - cz) > r) {
          continue;
        }
        inside++;
        float y;
        bool mine = false;
        bool got = floor(x, z, &y, &mine);
        any += got;
        if (got && (mine || zone.heightfield)) {
          own++;
          lo = std::min(lo, y);
          hi = std::max(hi, y);
        }
      }
    }
    if (own == 0) {
      if (any > 0) {
        lg::info("[enhanced] grass: zone {} over {}, but not on its own floor ({} of {} spots on the "
                 "height field)",
                 zi, name, any, inside);
      }
      continue;
    }
    lg::info("[enhanced] grass: zone {} in {}: {}% of it the level's floor, {:.1f} to {:.1f} m high", zi,
             name, 100 * own / std::max(inside, 1), lo / kMeter, hi / kMeter);
    if (own < inside / 4) {
      continue;  // (only its edge: another level has it)
    }
    // the shrub tree with a vertex nearest the middle draws it (its time of day colors); none: a
    // nearby level's (an orphan field)
    int best_tree = -1;
    const tfrag3::ShrubGpuVertex* best_vert = nullptr;
    float best = 1e30f;
    for (size_t t = 0; t < level.shrub_trees.size(); t++) {
      const auto& verts = level.shrub_trees[t].unpacked.vertices;
      for (size_t v = 0; v < verts.size(); v += 3) {
        float d = std::hypot(verts[v].x - cx, verts[v].z - cz);
        if (d < best) {
          best = d;
          best_tree = (int)t;
          best_vert = &verts[v];
        }
      }
    }
    if (zone.heightfield) {
      best_tree = -1;
    }
    if (best_tree >= 0 && level.shrub_trees[best_tree].enh_grass) {
      lg::warn("[enhanced] grass: zone {} shares its shrub tree with another, skipped", zi);
      continue;
    }
    auto field = std::make_shared<GrassField>();
    field->id = g_next_field++;
    field->n = (int)std::ceil(2.f * zone.radius / kStep) + 1;
    field->x0 = cx - r;
    field->z0 = cz - r;
    field->heights.assign(field->n * field->n, kNoFloor);
    int filled = 0;
    for (int k = 0; k < field->n; k++) {
      for (int i = 0; i < field->n; i++) {
        float y;
        bool mine = false;
        if (floor(field->x0 + i * kStep * kMeter, field->z0 + k * kStep * kMeter, &y, &mine)) {
          field->heights[k * field->n + i] = y;
          filled++;
        }
      }
    }
    // (where its floor is lowest: a pond's bed, to set the zone's water by)
    {
      float low = 1e30f;
      for (float h : field->heights) {
        if (h > kNoFloor) {
          low = std::min(low, h);
        }
      }
      double sx = 0, sz = 0;
      int cnt = 0;
      for (int k = 0; k < field->n; k++) {
        for (int i = 0; i < field->n; i++) {
          if (field->heights[k * field->n + i] < low + 1.5f * kMeter &&
              field->heights[k * field->n + i] > kNoFloor) {
            sx += field->x0 + i * kStep * kMeter;
            sz += field->z0 + k * kStep * kMeter;
            cnt++;
          }
        }
      }
      if (cnt) {
        lg::info("[enhanced] grass: zone {}: lowest floor {:.1f} m, its hollow around {:.0f}, {:.0f} m "
                 "({} heights within 1.5 m of it)",
                 zi, low / kMeter, sx / cnt / kMeter, sz / cnt / kMeter, cnt);
      }
    }
    field->nt = (int)std::ceil(2.f * zone.radius / kTile);
    field->zone = zi;
    {
      auto l = bake_lush(*field, zone);
      field->lush = std::move(l.spots);
      field->tile_lush = std::move(l.tiles);
    }
    field->cx = cx;
    field->cz = cz;
    field->radius = r;
    field->water = zone.water * kMeter;
    field->density = zone.density;
    if (best_tree < 0) {
      std::lock_guard<std::mutex> lock(g_orphan_mutex);
      g_orphans[zi] = field;
      lg::info("[enhanced] grass: zone {} baked in {} ({}x{} heights, {} with floor), no shrubs: drawn "
               "with the nearest level's",
               zi, name, field->n, field->n, filled);
      continue;
    }
    field->tod = best_vert->color_index;
    for (int c = 0; c < 3; c++) {
      field->base_rgb[c] = best_vert->rgba_base[c] / 255.f;
    }
    level.shrub_trees[best_tree].enh_grass = field;
    lg::info("[enhanced] grass: zone {} baked in {} ({}x{} heights, {} with floor), drawn with shrub "
             "tree {} ({:.1f} m from the middle)",
             zi, name, field->n, field->n, filled, best_tree, best / kMeter);
  }
}

namespace {
void draw_field(const std::shared_ptr<const GrassField>& fp,
                int tod,
                const float* base_rgb,
                const tfrag3::ShrubTree& tree,
                const TfragRenderSettings& settings,
                SharedRenderState* render_state,
                ScopedProfilerNode& prof) {
  const auto& f = *fp;
  const auto& cam = settings.camera.trans;
  if (std::hypot(cam.x() - f.cx, cam.z() - f.cz) > f.radius + kReach[2] * kMeter) {
    return;
  }
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  const float now = std::chrono::duration<float>(clock::now() - start).count();
  auto& fgl = field_gl(fp, now);
  const auto& tile_lush = fgl.tile_lush.empty() ? f.tile_lush : fgl.tile_lush;
  // (its ground darkened under it by the composite, so gaps between blades don't show bare sand)
  auto& gg = g_enhanced_grass_ground;
  gg.zone[0] = f.cx / kMeter;
  gg.zone[1] = f.cz / kMeter;
  gg.zone[2] = f.radius / kMeter;
  gg.zone[3] = f.density * g_enhanced_grass;
  gg.water = f.water / kMeter;
  gg.frame = render_state->frame_idx;
  gg.tex = fgl.tex;
  const float step = kStep * kMeter;
  // (the texture's texels sit on the heights: its corner half a step before the first)
  gg.field[0] = (f.x0 - step * 0.5f) / kMeter;
  gg.field[1] = (f.z0 - step * 0.5f) / kMeter;
  gg.field[2] = f.n * kStep;

  // the tiles near and in view, each with its level of detail (by its nearest point)
  const float tile = kTile * kMeter;
  const int nt = f.nt;
  for (auto& t : g_gl.tiles) {
    t.clear();
  }
  for (int i = 0; i < nt; i++) {
    for (int k = 0; k < nt; k++) {
      float x0 = f.cx - f.radius + i * tile, z0 = f.cz - f.radius + k * tile;
      float mx = x0 + tile * 0.5f, mz = z0 + tile * 0.5f;
      // (none of it there)
      if (tile_lush[k * nt + i] < 0.02f) {
        continue;
      }
      float y = f.height_at(mx, mz);
      for (int c = 0; c < 4 && y <= kNoFloor; c++) {
        y = f.height_at(x0 + (c & 1) * tile, z0 + (c >> 1) * tile);
      }
      if (y <= kNoFloor) {
        continue;
      }
      float dx = std::max({x0 - cam.x(), 0.f, cam.x() - (x0 + tile)});
      float dz = std::max({z0 - cam.z(), 0.f, cam.z() - (z0 + tile)});
      float dy = std::max(std::abs(cam.y() - y) - 1.f * kMeter, 0.f);
      float d = std::sqrt(dx * dx + dz * dz + dy * dy);
      int lod = d < kReach[0] * kMeter ? 0 : d < kReach[1] * kMeter ? 1 : d < kReach[2] * kMeter ? 2 : 3;
      if (lod > 2) {
        continue;
      }
      math::Vector4f sphere(mx, y + 0.4f * kMeter, mz, tile * 0.71f + 1.f * kMeter);
      if (!sphere_in_view_ref(sphere, settings.camera.planes)) {
        continue;
      }
      g_gl.tiles[lod].emplace_back(x0, z0, (float)lod, 0.f);
    }
  }
  g_gl.all.clear();
  for (const auto& t : g_gl.tiles) {
    g_gl.all.insert(g_gl.all.end(), t.begin(), t.end());
  }
  if (g_gl.all.empty()) {
    return;
  }

  if (!g_gl.made) {
    make_gl();
  }
  GLuint heights = fgl.tex;
  auto& sh = render_state->shaders[ShaderId::SHRUB_GRASS];
  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::SHRUB_GRASS);
  const auto id = sh.id();
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, heights);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_gl.white);
  glUniform1i(glGetUniformLocation(id, "tex_T3"), 3);
  glUniform1i(glGetUniformLocation(id, "tex_T10"), 10);
  glUniform1f(glGetUniformLocation(id, "enh_tile"), tile);
  glUniform4f(glGetUniformLocation(id, "enh_field"), f.x0 - step * 0.5f, f.z0 - step * 0.5f,
              f.n * step, f.water);
  glUniform4f(glGetUniformLocation(id, "enh_zone"), f.cx, f.cz, f.radius,
              f.density * g_enhanced_grass);
  glUniform3f(glGetUniformLocation(id, "enh_counts"), (float)kBlades[0], (float)kBlades[1],
              (float)kBlades[2]);
  glUniform3f(glGetUniformLocation(id, "enh_reach"), kReach[0] * kMeter, kReach[1] * kMeter,
              kReach[2] * kMeter);
  glUniform1i(glGetUniformLocation(id, "enh_tod"), tod);
  glUniform3f(glGetUniformLocation(id, "enh_base_rgb"), base_rgb[0], base_rgb[1], base_rgb[2]);
  {
    // (the painted grass's look: the listed fields' too, they're painted the same way)
    const auto s = enhanced_grass_style();
    glUniform4f(glGetUniformLocation(id, "enh_style"), s.height, s.width, s.wind, 0.f);
    glUniform3f(glGetUniformLocation(id, "enh_grass_color"), s.color[0], s.color[1], s.color[2]);
    glUniform3f(glGetUniformLocation(id, "enh_grass_dry"), s.dry[0], s.dry[1], s.dry[2]);
    glUniform4f(glGetUniformLocation(id, "enh_zone"), f.cx, f.cz, f.radius,
                std::min(f.density * s.density, 1.f) * g_enhanced_grass);
  }
  glUniform1f(glGetUniformLocation(id, "enh_time"), now);
  glUniform1f(glGetUniformLocation(id, "enh_wind"),
              g_enhanced_wind.enabled ? g_enhanced_wind.strength : 0.f);
  const auto& pl = g_enhanced_player;
  glUniform4f(glGetUniformLocation(id, "enh_player"), pl.pos[0], pl.pos[1], pl.pos[2],
              pl.valid && g_enhanced_wind.push ? g_enhanced_wind.push_radius * kMeter : 0.f);
  const auto& ps = g_enhanced_pushers;
  const int pushers = g_enhanced_wind.push ? ps.count : 0;
  glUniform1i(glGetUniformLocation(id, "enh_pusher_count"), pushers);
  if (pushers > 0) {
    glUniform4fv(glGetUniformLocation(id, "enh_pushers"), pushers, &ps.pos[0][0]);
  }
  float crush[4] = {};
  GLuint crush_tex = g_enhanced_wind.push ? enhanced_crush_map(crush) : 0;
  glUniform4f(glGetUniformLocation(id, "enh_crush"), crush[0], crush[1], crush[2],
              crush_tex ? crush[3] : 0.f);
  glUniform1i(glGetUniformLocation(id, "tex_T4"), 4);
  glActiveTexture(GL_TEXTURE4);
  glBindTexture(GL_TEXTURE_2D, crush_tex);
  glActiveTexture(GL_TEXTURE0);
  // (drawn like the tree's grass: its first cutout draw's settings, then solid)
  DrawMode mode;
  bool found = false;
  for (const auto& draw : tree.static_draws) {
    if (draw.mode.get_at_enable()) {
      mode = draw.mode;
      found = true;
      break;
    }
  }
  if (!found && !tree.static_draws.empty()) {
    mode = tree.static_draws[0].mode;
  }
  setup_tfrag_shader(render_state, mode, ShaderId::SHRUB_GRASS);
  glUniform1f(glGetUniformLocation(id, "alpha_min"), 0.f);
  glUniform1f(glGetUniformLocation(id, "alpha_max"), 10.f);
  glUniform2f(glGetUniformLocation(id, "enh_lod"), 0.f, 0.f);
  glUniform1f(glGetUniformLocation(id, "enh_foliage"), 1.f);
  glUniform1f(glGetUniformLocation(id, "enh_stone"), 0.f);
  glUniform1f(glGetUniformLocation(id, "enh_wet"), g_enhanced_wind.wet);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_TRUE);
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  glBindVertexArray(g_gl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_gl.vbo);
  glBufferData(GL_ARRAY_BUFFER, g_gl.all.size() * sizeof(math::Vector4f), g_gl.all.data(),
               GL_STREAM_DRAW);
  glDisable(GL_PRIMITIVE_RESTART);
  size_t offset = 0;
  for (int lod = 0; lod < 3; lod++) {
    const size_t count = g_gl.tiles[lod].size();
    if (!count) {
      continue;
    }
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(math::Vector4f),
                          (void*)(offset * sizeof(math::Vector4f)));
    glVertexAttribDivisor(0, kBlades[lod]);
    glUniform1i(glGetUniformLocation(id, "enh_blades"), kBlades[lod]);
    glUniform1i(glGetUniformLocation(id, "enh_segs"), kSegs[lod]);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 2 * kSegs[lod] + 1, count * kBlades[lod]);
    prof.add_draw_call();
    prof.add_tri(count * kBlades[lod] * (2 * kSegs[lod] - 1));
    offset += count;
  }
  glEnable(GL_PRIMITIVE_RESTART);
  glBindVertexArray(0);
}
}  // namespace

void enhanced_draw_grass(const tfrag3::ShrubTree& tree,
                         const TfragRenderSettings& settings,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof) {
  if (g_enhanced_grass <= 0.f) {
    return;
  }
  if (tree.enh_grass) {
    auto f = std::static_pointer_cast<const GrassField>(tree.enh_grass);
    draw_field(f, f->tod, f->base_rgb, tree, settings, render_state, prof);
  }
  // (the orphan fields this tree hosts; the brush's fields: drawn with the first tree drawn this
  // frame, lit by its time of day colors)
  std::vector<std::pair<std::shared_ptr<GrassField>, Host>> orphans;
  {
    std::lock_guard<std::mutex> lock(g_orphan_mutex);
    const auto& all = zones();
    for (const auto& [zi, field] : g_orphans) {
      if (zi < all.size() && all[zi].user) {
        auto& last = g_user_drawn[zi];
        if (last == render_state->frame_idx) {
          continue;
        }
        last = render_state->frame_idx;
        Host h;
        h.tree = &tree;
        const auto& verts = tree.unpacked.vertices;
        if (!verts.empty()) {
          h.tod = verts[0].color_index;
          for (int c = 0; c < 3; c++) {
            h.base_rgb[c] = verts[0].rgba_base[c] / 255.f;
          }
        }
        orphans.emplace_back(field, h);
        continue;
      }
      auto it = g_hosts.find(zi);
      if (it != g_hosts.end() && it->second.tree == &tree) {
        orphans.emplace_back(field, it->second);
      }
    }
  }
  for (const auto& [field, host] : orphans) {
    draw_field(field, host.tod, host.base_rgb, tree, settings, render_state, prof);
  }
}
