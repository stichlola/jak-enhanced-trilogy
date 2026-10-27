#include "EnhancedPost.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <ctime>
#include <random>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/loader/EnhancedRayTrace.h"
#include "game/graphics/opengl_renderer/loader/EnhancedVegetation.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "game/graphics/pipelines/opengl.h"
#include "game/runtime.h"
#include "game/sound/common/enhanced_ambience.h"

#include "third-party/imgui/imgui.h"
#include "third-party/stb_image/stb_image.h"

EnhancedCamera g_enhanced_camera;
EnhancedSun g_enhanced_sun;
EnhancedPlayer g_enhanced_player;
bool g_enhanced_debug_toolbar = false;
EnhancedScene g_enhanced_scene;
EnhancedDust g_enhanced_dust;
std::atomic<int> g_enhanced_thunder{0};
EnhancedPushers g_enhanced_pushers;
EnhancedSmokeLights g_enhanced_smoke_lights;
EnhancedMerc g_enhanced_merc;
EnhancedWind g_enhanced_wind;
EnhancedCasters g_enhanced_casters;
EnhancedLights g_enhanced_lights;
EnhancedGameLights g_enhanced_game_lights;
EnhancedExplosions g_enhanced_explosions;
EnhancedHeadlights g_enhanced_headlights;
EnhancedFires g_enhanced_fires;
EnhancedMenu g_enhanced_menu;
EnhancedTimeOfDay g_enhanced_tod;
EnhancedNormals g_enhanced_normals;

bool enhanced_has_piazza() {
  static const bool has =
      fs::exists(file_util::get_jak_project_dir() / "out" / "jak3" / "iso" / "PZZ.DGO");
  return has;
}

EnhancedNormalTarget::EnhancedNormalTarget() {
  if (!g_enhanced_normals.fbo || !g_enhanced_normals.enabled) {
    return;
  }
  GLint bound = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
  if ((GLuint)bound != g_enhanced_normals.fbo) {
    return;  // drawing somewhere else (texture animation, offscreen)
  }
  const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(2, both);
  m_on = true;
}

EnhancedNormalTarget::~EnhancedNormalTarget() {
  if (m_on) {
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }
}

namespace {
// The world's look and weather for the three games: game/assets/enhanced/world.json, reloaded while
// the game runs whenever it's saved (see the file for what each value does)
struct ZoneLook {
  float lift[3], gain[3], saturation, contrast, warmth, clouds;
};
// the weathers a zone's spells pick from ("spells" in world.json), and the seasons that make some
// likelier than others
enum SpellKind { kFair, kOvercast, kShower, kThunder, kFogSpell, kSandstorm, kSnowSpell, kSpellKinds };
const char* const kSpellNames[kSpellKinds] = {"fair", "overcast", "shower", "thunder", "fog", "sandstorm", "snow"};
const char* const kSeasonNames[4] = {"spring", "summer", "autumn", "winter"};
// what a season does everywhere: the spells' odds (times), the grade (plus), the animals (times)
struct Season {
  std::array<float, kSpellKinds> odds;
  float warmth = 0.f, saturation = 0.f;
  float crickets = 1.f, birds = 1.f, frogs = 1.f;
  Season() { odds.fill(1.f); }
};
struct ZoneWeather {
  bool outdoors = true;
  float rain = 0.f, mist = 0.f, snow = 0.f, heat = 0.f, dust = 0.f, sunny = 0.f, clouds = 0.f;
  float wet = -1.f;  // < 0: as wet as it rains
  // added for a while in every four-minute cycle
  float harder_rain = 0.f, harder_mist = 0.f, harder_snow = 0.f, harder_dust = 0.f, harder_wet = 0.f;
  float harder_from = 0.f, harder_to = 0.f;
  // spells of weather (period 0: none), each a weather picked by these odds
  float spell_period = 0.f;
  std::array<float, kSpellKinds> spells{};
  // the zone's own season odds, times the world's (1: as everywhere)
  std::array<std::array<float, kSpellKinds>, 4> season_odds;
  ZoneWeather() {
    for (auto& s : season_odds) {
      s.fill(1.f);
    }
  }
  float morning_mist = 0.f;
  bool game_dust = false;
  // the distance fog and the volumetric mist: a fixed amount, or spells (period > 0) that are
  // clear, light or full
  float fog = 1.f;
  float fog_period = 0.f, fog_clear = 0.f, fog_light = 0.f, fog_light_amount = 0.35f;
};
// the zone's ambient sound loops (0..1): crickets and wind_night at night, birds by day, frogs
// mostly at night, wind, waves and city always
struct ZoneAmbience {
  float crickets = 0.f, wind_night = 0.f, wind = 0.f, birds = 0.f, frogs = 0.f, waves = 0.f, city = 0.f;
};
struct WorldZone {
  std::string name;
  ZoneLook look[2];  // day, night
  ZoneWeather weather;
  ZoneAmbience ambience;
};
struct World {
  float gi[3] = {0.6f, 0.6f, 1.f};  // Jak 1, 2, 3
  float lamps[3] = {1.f, 0.45f, 1.f};
  std::vector<WorldZone> zones;  // [0]: "other", for the levels in no zone
  std::vector<std::pair<std::string, int>> prefixes[3];
  Season seasons[4];
};
World g_world;

// the weather presets of the menu (the order is kept: saved settings and test scripts use the
// index). Values as in Settings::weather_mix; "Changing" has none (the zones decide)
struct WeatherPreset {
  const char* name;
  std::array<float, 9> mix;  // rain, storm, fog, mist, snow, dust, clouds, after, wet
};
// cutscene close-ups: nearer than this (game units) the screen-space shadows and occlusion and the
// sun's shadow map fade out. At a face's scale a pixel step is a millimeter: Daxter's fur and Jak's
// hair shadow themselves into specks and stripes, and the map's texels show as blocks
constexpr float kCloseUp = 3.f * 4096.f;  // 3 m

const WeatherPreset kWeatherPresets[] = {
    {"Clear", {0, 0, 0, 0, 0, 0, 0, 0, -1}},
    {"Changing (by area)", {0, 0, 0, 0, 0, 0, 0, 0, -1}},
    {"Rain", {1, 0, 0.6f, 0, 0, 0, 0.95f, 0, -1}},
    {"Sandstorm", {0, 0, 0.3f, 0, 0, 1, 0.3f, 0, -1}},
    {"Thunderstorm", {1, 1, 0.7f, 0, 0, 0, 1, 0, -1}},
    {"After the rain", {0, 0, 0.3f, 0, 0, 0, 0.55f, 1, 0.85f}},
    {"Morning fog", {0, 0, 0.6f, 1, 0, 0, 0.4f, 0, 0.25f}},
    {"Clear, a few clouds", {0, 0, 0, 0, 0, 0, 0.25f, 0, -1}},
    {"Snow", {0, 0, 0.6f, 0.4f, 1, 0, 0.8f, 0, -1}},
    {"Overcast", {0, 0, 0.2f, 0, 0, 0, 0.95f, 0, -1}},
    {"Drizzle", {0.3f, 0, 0.3f, 0, 0, 0, 0.9f, 0, -1}},
    {"Downpour", {1.3f, 0.3f, 0.8f, 0, 0, 0, 1, 0, -1}},
    {"Storm in the fog", {1, 1, 1, 0.6f, 0, 0, 1, 0, -1}},
    {"Light fog", {0, 0, 0.4f, 0, 0, 0, 0.5f, 0, -1}},
    {"Fog", {0, 0, 1, 0.3f, 0, 0, 0.7f, 0, -1}},
    {"Dense fog", {0, 0, 1, 1, 0, 0, 0.9f, 0, 0.3f}},
    {"Rain and fog", {0.6f, 0, 1, 0.5f, 0, 0, 0.95f, 0, -1}},
    {"Light snow", {0, 0, 0.3f, 0, 0.4f, 0, 0.7f, 0, -1}},
    {"Blizzard", {0, 0, 1, 0.8f, 1.4f, 0, 1, 0, -1}},
    {"Dusty haze", {0, 0, 0.4f, 0, 0, 0.35f, 0.3f, 0, -1}},
    {"Sun after the storm", {0, 0, 0, 0.2f, 0, 0, 0.4f, 0.8f, 0.7f}},
    {"Custom", {0, 0, 0, 0, 0, 0, 0.3f, 0, -1}},
};
constexpr int kWeatherCount = sizeof(kWeatherPresets) / sizeof(kWeatherPresets[0]);
constexpr int kWeatherCustom = kWeatherCount - 1;

// photo mode's filters: a grade over the area's (saturation times, contrast and warmth plus, the
// highlights tinted, the shadows lifted)
struct PhotoFilter {
  const char* name;
  float saturation, contrast, warmth;
  float gain[3], lift[3];
};
const PhotoFilter kPhotoFilters[] = {
    {"None", 1.f, 0.f, 0.f, {1, 1, 1}, {0, 0, 0}},
    {"Warm", 1.1f, 0.02f, 0.08f, {1.04f, 1.f, 0.94f}, {0, 0, 0}},
    {"Cool", 0.95f, 0.02f, -0.08f, {0.95f, 1.f, 1.06f}, {0, 0, 0.01f}},
    {"Vivid", 1.45f, 0.08f, 0.02f, {1, 1, 1}, {0, 0, 0}},
    {"Faded", 0.72f, -0.06f, 0.02f, {0.97f, 0.97f, 0.95f}, {0.04f, 0.04f, 0.045f}},
    {"Black and white", 0.f, 0.1f, 0.f, {1, 1, 1}, {0, 0, 0}},
    {"Sepia", 0.05f, 0.06f, 0.32f, {1.f, 1.f, 1.f}, {0.02f, 0.012f, 0.f}},
    {"Noir", 0.f, 0.32f, 0.f, {1.05f, 1.05f, 1.05f}, {-0.01f, -0.01f, -0.01f}},
    {"Sunset", 1.15f, 0.06f, 0.12f, {1.1f, 0.95f, 0.85f}, {0.02f, 0.f, 0.02f}},
};
constexpr int kPhotoFilterCount = sizeof(kPhotoFilters) / sizeof(kPhotoFilters[0]);

// the weather in a menu: the presets, and under them the mix (moving a slider makes it "Custom")
void weather_menu(EnhancedPost::Settings& s, const char* id) {
  ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo(id, kWeatherPresets[std::clamp(s.weather, 0, kWeatherCount - 1)].name)) {
    for (int i = 0; i < kWeatherCount; i++) {
      if (ImGui::Selectable(kWeatherPresets[i].name, s.weather == i)) {
        s.weather = i;
        if (i != 1 && i != kWeatherCustom) {
          s.weather_mix = kWeatherPresets[i].mix;
        }
      }
    }
    ImGui::EndCombo();
  }
  if (s.weather == 1) {
    // the changing weather follows the season: the calendar's, or one picked here
    ImGui::Indent();
    ImGui::SetNextItemWidth(160);
    ImGui::Combo(fmt::format("Season##{}", id).c_str(), &s.season,
                 "By the calendar\0Spring\0Summer\0Autumn\0Winter\0");
    ImGui::Unindent();
    return;
  }
  const char* names[9] = {"Rain", "Thunderstorm", "Fog", "Ground mist", "Snow",
                          "Dust", "Clouds", "Sun after rain", "Wet ground"};
  const float top[9] = {1.5f, 1.f, 1.f, 1.f, 1.5f, 1.f, 1.f, 1.f, 1.f};
  ImGui::Indent();
  for (int i = 0; i < 9; i++) {
    ImGui::SetNextItemWidth(160);
    float v = s.weather_mix[i];
    const float lo = i == 8 ? -0.01f : 0.f;  // wet ground below 0: as wet as it rains
    if (ImGui::SliderFloat(fmt::format("{}##{}", names[i], id).c_str(), &v, lo, top[i],
                           i == 8 && v < 0.f ? "with the rain" : "%.2f")) {
      s.weather_mix[i] = v;
      s.weather = kWeatherCustom;
    }
  }
  ImGui::Unindent();
}

int game_index() {
  return g_game_version == GameVersion::Jak1 ? 0 : g_game_version == GameVersion::Jak2 ? 1 : 2;
}

float game_gi_scale() {
  return g_world.gi[game_index()];
}
float game_lamp_scale() {
  return g_world.lamps[game_index()];
}

fs::path world_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "world.json";
}

WorldZone neutral_zone() {
  WorldZone z;
  z.name = "other";
  for (auto& l : z.look) {
    l = {{0, 0, 0}, {1, 1, 1}, 1.f, 0.f, 0.f, 0.4f};
  }
  return z;
}

// reads world.json; on an error the world stays as it was (the log says why)
void load_world() {
  try {
    auto j = parse_commented_json(file_util::read_text_file(world_path()), "world.json");
    World w;
    w.zones.push_back(neutral_zone());
    const char* games[3] = {"jak1", "jak2", "jak3"};
    for (int g = 0; g < 3; g++) {
      if (j.contains("tuning") && j["tuning"].contains(games[g])) {
        const auto& t = j["tuning"][games[g]];
        w.gi[g] = t.value("gi", w.gi[g]);
        w.lamps[g] = t.value("lamps", w.lamps[g]);
      }
    }
    auto read_look = [](const nlohmann::json& l, ZoneLook& out) {
      for (int i = 0; i < 3; i++) {
        out.lift[i] = l.contains("lift") ? l["lift"][i].get<float>() : 0.f;
        out.gain[i] = l.contains("gain") ? l["gain"][i].get<float>() : 1.f;
      }
      out.saturation = l.value("saturation", 1.f);
      out.contrast = l.value("contrast", 0.f);
      out.warmth = l.value("warmth", 0.f);
      out.clouds = l.value("clouds", 0.4f);
    };
    for (const auto& item : j.at("zones").items()) {
      const std::string name = item.key();
      const auto& z = item.value();
      WorldZone zone = neutral_zone();
      zone.name = name;
      if (z.contains("day")) {
        read_look(z["day"], zone.look[0]);
      }
      zone.look[1] = zone.look[0];
      if (z.contains("night")) {
        read_look(z["night"], zone.look[1]);
      }
      if (z.contains("weather")) {
        const auto& wj = z["weather"];
        auto& we = zone.weather;
        we.outdoors = wj.value("outdoors", true);
        we.rain = wj.value("rain", 0.f);
        we.mist = wj.value("mist", 0.f);
        we.snow = wj.value("snow", 0.f);
        we.heat = wj.value("heat", 0.f);
        we.dust = wj.value("dust", 0.f);
        we.sunny = wj.value("sunny", 0.f);
        we.clouds = wj.value("clouds", 0.f);
        we.wet = wj.value("wet", -1.f);
        if (wj.contains("harder")) {
          const auto& h = wj["harder"];
          we.harder_rain = h.value("rain", 0.f);
          we.harder_mist = h.value("mist", 0.f);
          we.harder_snow = h.value("snow", 0.f);
          we.harder_dust = h.value("dust", 0.f);
          we.harder_wet = h.value("wet", 0.f);
        }
        we.harder_from = wj.value("harder_from", 0.f);
        we.harder_to = wj.value("harder_to", 0.f);
        if (wj.contains("spells")) {
          const auto& sp = wj["spells"];
          we.spell_period = sp.value("period", 480.f);
          for (int k = 0; k < kSpellKinds; k++) {
            we.spells[k] = sp.value(kSpellNames[k], 0.f);
          }
        } else if (wj.contains("showers")) {
          // the older form: a chance of rain, some of it thunder, fair the rest
          const auto& sh = wj["showers"];
          const float chance = sh.value("chance", 1.f), thunder = sh.value("thunder", 0.f);
          we.spell_period = sh.value("period", 240.f);
          we.spells[kFair] = 1.f - chance;
          we.spells[kShower] = chance * (1.f - thunder);
          we.spells[kThunder] = chance * thunder;
        }
        if (wj.contains("seasons")) {
          for (int s = 0; s < 4; s++) {
            if (wj["seasons"].contains(kSeasonNames[s])) {
              for (int k = 0; k < kSpellKinds; k++) {
                we.season_odds[s][k] = wj["seasons"][kSeasonNames[s]].value(kSpellNames[k], 1.f);
              }
            }
          }
        }
        we.morning_mist = wj.value("morning_mist", 0.f);
        we.game_dust = wj.value("game_dust", false);
        if (wj.contains("fog")) {
          const auto& f = wj["fog"];
          if (f.is_number()) {
            we.fog = f.get<float>();
          } else {
            we.fog_period = f.value("period", 480.f);
            we.fog_clear = f.value("clear", 0.f);
            we.fog_light = f.value("light", 0.f);
            we.fog_light_amount = f.value("light_amount", 0.35f);
          }
        }
      }
      if (z.contains("ambience")) {
        const auto& aj = z["ambience"];
        zone.ambience.crickets = aj.value("crickets", 0.f);
        zone.ambience.wind_night = aj.value("wind_night", 0.f);
        zone.ambience.wind = aj.value("wind", 0.f);
        zone.ambience.birds = aj.value("birds", 0.f);
        zone.ambience.frogs = aj.value("frogs", 0.f);
        zone.ambience.waves = aj.value("waves", 0.f);
        zone.ambience.city = aj.value("city", 0.f);
      }
      if (name == "other") {
        w.zones[0] = zone;
      } else {
        w.zones.push_back(zone);
      }
    }
    if (j.contains("seasons")) {
      for (int s = 0; s < 4; s++) {
        if (!j["seasons"].contains(kSeasonNames[s])) {
          continue;
        }
        const auto& sj = j["seasons"][kSeasonNames[s]];
        Season& out = w.seasons[s];
        for (int k = 0; k < kSpellKinds; k++) {
          out.odds[k] = sj.value(kSpellNames[k], 1.f);
        }
        out.warmth = sj.value("warmth", 0.f);
        out.saturation = sj.value("saturation", 0.f);
        out.crickets = sj.value("crickets", 1.f);
        out.birds = sj.value("birds", 1.f);
        out.frogs = sj.value("frogs", 1.f);
      }
    }
    for (int g = 0; g < 3; g++) {
      if (!j.contains("levels") || !j["levels"].contains(games[g])) {
        continue;
      }
      for (const auto& entry : j["levels"][games[g]]) {
        const std::string zone = entry.at("zone").get<std::string>();
        int idx = -1;
        for (size_t i = 0; i < w.zones.size(); i++) {
          if (w.zones[i].name == zone) {
            idx = (int)i;
          }
        }
        if (idx < 0) {
          lg::warn("[enhanced] world.json: {} levels in the unknown zone '{}'", games[g], zone);
          continue;
        }
        for (const auto& prefix : entry.at("prefixes")) {
          w.prefixes[g].emplace_back(prefix.get<std::string>(), idx);
        }
      }
    }
    g_world = std::move(w);
    lg::info("[enhanced] world.json: {} zones", g_world.zones.size());
  } catch (const std::exception& e) {
    lg::error("[enhanced] world.json not loaded: {}", e.what());
  }
  if (g_world.zones.empty()) {
    g_world.zones.push_back(neutral_zone());
  }
}

// lights.json: lights placed by hand where the game has none (a dark street, a camp in the
// desert), per game. World position and radius in meters, a color, whether it flickers like a
// fire and whether it's lit at night only. The Enhanced menu adds one where Jak stands.
struct ManualLight {
  std::string note;
  float pos[3] = {};  // game units
  float radius = 8.f * 4096.f;
  float color[3] = {1.f, 0.7f, 0.4f};
  bool flicker = false;
  bool night = true;
  bool bulb = true;  // drawn as a point of light (off: the light alone, where the scene has its lamp)
};
std::vector<ManualLight> g_manual_lights[3];

fs::path manual_lights_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "lights.json";
}

void load_manual_lights() {
  for (auto& l : g_manual_lights) {
    l.clear();
  }
  std::error_code ec;
  if (!fs::exists(manual_lights_path(), ec)) {
    return;
  }
  try {
    auto j = parse_commented_json(file_util::read_text_file(manual_lights_path()), "lights.json");
    const char* games[3] = {"jak1", "jak2", "jak3"};
    for (int g = 0; g < 3; g++) {
      if (!j.contains(games[g])) {
        continue;
      }
      for (const auto& e : j[games[g]]) {
        ManualLight l;
        l.note = e.value("note", "");
        for (int k = 0; k < 3; k++) {
          l.pos[k] = e.at("pos")[k].get<float>() * 4096.f;
          if (e.contains("color")) {
            l.color[k] = e["color"][k].get<float>();
          }
        }
        l.radius = e.value("radius", 8.f) * 4096.f;
        l.flicker = e.value("flicker", false);
        l.night = e.value("night", true);
        l.bulb = e.value("bulb", true);
        g_manual_lights[g].push_back(l);
      }
    }
    lg::info("[enhanced] lights.json: {} lights", g_manual_lights[game_index()].size());
  } catch (const std::exception& e) {
    lg::error("[enhanced] lights.json not loaded: {}", e.what());
  }
}

void save_manual_lights() {
  nlohmann::json j = nlohmann::json::object();
  const char* games[3] = {"jak1", "jak2", "jak3"};
  for (int g = 0; g < 3; g++) {
    auto arr = nlohmann::json::array();
    for (const auto& l : g_manual_lights[g]) {
      auto r = [](float v) { return std::round(v * 100.f) / 100.f; };
      arr.push_back({{"note", l.note},
                     {"pos", {r(l.pos[0] / 4096.f), r(l.pos[1] / 4096.f), r(l.pos[2] / 4096.f)}},
                     {"radius", r(l.radius / 4096.f)},
                     {"color", {r(l.color[0]), r(l.color[1]), r(l.color[2])}},
                     {"flicker", l.flicker},
                     {"night", l.night},
                     {"bulb", l.bulb}});
    }
    if (!arr.empty()) {
      j[games[g]] = arr;
    }
  }
  file_util::write_text_file(manual_lights_path(), j.dump(2));
}

// once a second: load world.json again if it was saved since
void poll_world() {
  static int frames = 0;
  static fs::file_time_type last{};
  static bool loaded = false;
  if (loaded && ++frames < 60) {
    return;
  }
  frames = 0;
  std::error_code ec;
  const auto t = fs::last_write_time(world_path(), ec);
  if (!loaded || (!ec && t != last)) {
    loaded = true;
    if (!ec) {
      last = t;
    }
    load_world();
    load_manual_lights();
  }
}

// the zone of the level Jak is in, from the start of its name (the current game's list)
int zone_of(const std::string& level) {
  for (const auto& [prefix, zone] : g_world.prefixes[game_index()]) {
    if (level.rfind(prefix, 0) == 0) {
      return zone;
    }
  }
  return 0;
}

GLuint make_1x1(u32 rgba) {
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &rgba);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);
  return tex;
}

// the game's vertex shaders scale y by SCISSOR_ADJUST * HEIGHT_SCALE (jak 2/3 values, Shader.cpp)
constexpr float kScissorHeightScale = (512.f / 416.f) * 0.5f;
// GOAL distance units per meter
constexpr float kMeter = 4096.f;

// the key light in world space; test switch: JAK_ENHANCED_SUN_YAW=<degrees> turns it around the
// world's up axis
void sun_world(float out[3]) {
  static const float yaw = [] {
    const char* env = std::getenv("JAK_ENHANCED_SUN_YAW");
    return env ? (float)std::atof(env) * 3.14159265f / 180.f : 0.f;
  }();
  const float* d = g_enhanced_sun.dir;
  out[0] = std::cos(yaw) * d[0] + std::sin(yaw) * d[2];
  out[1] = d[1];
  out[2] = -std::sin(yaw) * d[0] + std::cos(yaw) * d[2];
}
}  // namespace

EnhancedPost::EnhancedPost() {
  lg::info("[enhanced] Jak Enhanced Trilogy by stichlola - https://github.com/stichlola/jak-enhanced-trilogy");
  // the menu's choices from the last session first, the test switches below win over them
  try {
    auto path = file_util::get_user_settings_dir(g_game_version) / "enhanced-settings.json";
    if (file_util::file_exists(path.string())) {
      m_saved_settings = file_util::read_text_file(path);
      apply_menu_settings(m_saved_settings);
      lg::info("[enhanced] settings loaded from {}", path.string());
    }
  } catch (std::exception& e) {
    lg::error("[enhanced] could not load the saved settings: {}", e.what());
  }
  if (const char* preset = std::getenv("JAK_ENHANCED_PRESET")) {
    // test boots: 0 Classic, 1 Medium, 2 Maximum, 3 Performance
    apply_preset(std::clamp(std::atoi(preset), 0, 3));
  }
  if (const char* pop = std::getenv("JAK_ENHANCED_POPULATION")) {
    // the game reads the same variable at boot
    m_population = std::clamp(std::atoi(pop), 0, 3);
  } else if (m_population != 2) {
    g_enhanced_menu.ask(6, std::to_string(m_population));
  }
  if (const char* map = std::getenv("JAK_ENHANCED_MINIMAP")) {
    g_enhanced_menu.minimap = std::strcmp(map, "0") != 0;
  }
  const char* env = std::getenv("JAK_ENHANCED");
  if (env && std::strcmp(env, "0") == 0) {
    m_settings.enabled = false;
  }
  // test switch: JAK_ENHANCED_RIM=<strength> overrides the character rim light
  if (const char* rim = std::getenv("JAK_ENHANCED_RIM")) {
    g_enhanced_merc.rim = (float)std::atof(rim);
  }
  if (const char* relight = std::getenv("JAK_ENHANCED_RELIGHT")) {
    m_settings.relight = (float)std::atof(relight);
  }
  if (const char* gi = std::getenv("JAK_ENHANCED_GI")) {
    m_settings.gi_strength = (float)std::atof(gi);
  }
  if (const char* wind = std::getenv("JAK_ENHANCED_WIND")) {
    g_enhanced_wind.strength = (float)std::atof(wind);
  }
  if (const char* vol = std::getenv("JAK_ENHANCED_MIST")) {
    // 0 = off, otherwise the density at the ground (/m)
    m_settings.volume = std::atof(vol) > 0.0;
    if (m_settings.volume) {
      m_settings.mist_density = (float)std::atof(vol);
    }
  }
  if (const char* push = std::getenv("JAK_ENHANCED_GRASS_PUSH")) {
    // the radius (m) the grass bends away from Jak in, 0 = off
    g_enhanced_wind.push_radius = (float)std::atof(push);
    g_enhanced_wind.push = g_enhanced_wind.push_radius > 0.f;
  }
  if (const char* normals = std::getenv("JAK_ENHANCED_NORMALS")) {
    // 0 = normals rebuilt from depth only, 2 = show them
    m_settings.geometry_normals = std::atoi(normals) != 0;
    m_settings.normal_debug = std::atoi(normals) == 2;
  }
  if (const char* menu = std::getenv("JAK_ENHANCED_MENU")) {
    // test runs: the in-game menu open from the start; "<kind>:<value>" also queues one request
    // (e.g. 2:ctysluma-alley, 1:23)
    g_enhanced_menu.open = true;
    const char* colon = std::strchr(menu, ':');
    if (colon) {
      g_enhanced_menu.ask(std::atoi(menu), colon + 1);
    }
  }
  if (const char* ls = std::getenv("JAK_ENHANCED_LIGHT_SHADOWS")) {
    // how dark the dynamic lights' shadows are, 0 = none
    m_settings.light_shadow = (float)std::atof(ls);
  }
  if (const char* relief = std::getenv("JAK_ENHANCED_RELIEF")) {
    // the textures' small bumps under the sun, 0 = off, 2 = left half only (A/B)
    m_settings.relief = (float)std::atof(relief);
  }
  if (const char* mat = std::getenv("JAK_ENHANCED_MATERIALS")) {
    // metal and glossy surfaces' reflections and highlights, 0 = off
    m_settings.materials = (float)std::atof(mat);
  }
  if (const char* rt = std::getenv("JAK_ENHANCED_RT_REFLECTIONS")) {
    // reflections traced through the levels: 0 off, 2 traced only (no screen-space)
    m_settings.ray_traced = std::clamp(std::atoi(rt), 0, 3);
  }
  if (const char* rt_ao = std::getenv("JAK_ENHANCED_RT_AO")) {
    // ambient occlusion traced through the levels, 0 = off
    m_settings.rt_ao = std::atoi(rt_ao) != 0;
  }
  if (const char* len = std::getenv("JAK_ENHANCED_RT_AO_LENGTH")) {
    m_settings.rt_ao_length = (float)std::atof(len);  // meters
  }
  if (const char* far_view = std::getenv("JAK_ENHANCED_FAR_VIEW")) {
    // 0 = the game's occlusion culling (precomputed visibility) as it was
    m_settings.far_view = std::atoi(far_view) != 0;
  }
  if (const char* smoke = std::getenv("JAK_ENHANCED_LIT_SMOKE")) {
    // how much the dynamic lights brighten and tint the smoke and dust near them, 0 = off
    m_settings.lit_smoke = (float)std::atof(smoke);
  }
  if (const char* prints = std::getenv("JAK_ENHANCED_PRINTS")) {
    // footprints and ruts in the sand, 0 = off
    m_settings.prints = (float)std::atof(prints);
  }
  if (const char* wake = std::getenv("JAK_ENHANCED_WAKE")) {
    // rings on the water where Jak swims or rides, 0 = off
    m_settings.wake = (float)std::atof(wake);
  }
  if (const char* blast = std::getenv("JAK_ENHANCED_BLAST")) {
    // explosions' flash and shockwave strength, 0 = off
    m_settings.blast = (float)std::atof(blast);
  }
  if (const char* fire = std::getenv("JAK_ENHANCED_FIRE")) {
    // 0 = the game's fires as they are; else a scale for the hot air and the embers
    float k = (float)std::atof(fire);
    m_settings.fire = k > 0.f;
    m_settings.fire_haze *= k;
    m_settings.fire_embers *= k;
  }
  if (const char* ws = std::getenv("JAK_ENHANCED_WATER_SIM")) {
    // 0 = the water without swell, refraction and caustics
    if (std::atof(ws) <= 0.0) {
      m_settings.water_swell = m_settings.water_refraction = m_settings.water_caustics = 0.f;
    }
  }
  if (const char* lights = std::getenv("JAK_ENHANCED_LIGHTS")) {
    // 0 = off, otherwise the strength
    m_settings.lights = std::atof(lights) > 0.0;
    if (m_settings.lights) {
      m_settings.light_strength = (float)std::atof(lights);
    }
  }
  if (const char* sm = std::getenv("JAK_ENHANCED_SHADOWS")) {
    // 0 = off (screen-space shadows only), otherwise the strength
    m_settings.shadow_map = std::atof(sm) > 0.0;
    if (m_settings.shadow_map) {
      m_settings.shadow_strength = (float)std::atof(sm);
    }
  }
  if (const char* casters = std::getenv("JAK_ENHANCED_SHADOW_CASTERS")) {
    // 0 = only the static level geometry casts sun shadows
    m_settings.shadow_casters = std::atoi(casters) != 0;
  }
  if (const char* mist_sun = std::getenv("JAK_ENHANCED_MIST_SUN")) {
    m_settings.mist_sun = (float)std::atof(mist_sun);
  }
  if (const char* taa = std::getenv("JAK_ENHANCED_TAA")) {
    m_settings.taa = std::strcmp(taa, "0") != 0;
  }
  const char* ao_debug = std::getenv("JAK_ENHANCED_AO_DEBUG");
  if (ao_debug && std::strcmp(ao_debug, "1") == 0) {
    m_settings.ssao_debug = true;
  }
  if (const char* weather = std::getenv("JAK_ENHANCED_WEATHER")) {
    // 0 clear, 1 by area, 2 rain, 3 sandstorm
    m_settings.weather = std::clamp(std::atoi(weather), 0, kWeatherCount - 1);
  }
  if (const char* clouds = std::getenv("JAK_ENHANCED_CLOUDS")) {
    // the cloud layer's strength, 0 = off
    m_settings.cloud_amount = (float)std::atof(clouds);
    m_settings.clouds = m_settings.cloud_amount > 0.f;
  }
  if (const char* grade = std::getenv("JAK_ENHANCED_GRADE")) {
    // 0 = one grade everywhere
    m_settings.area_grading = std::atoi(grade) != 0;
  }
  if (const char* soft = std::getenv("JAK_ENHANCED_SOFT_SHADOWS")) {
    // penumbra per meter from the caster, 0 = hard (4x4 filtered) shadows
    m_settings.shadow_softness = (float)std::atof(soft);
    m_settings.soft_shadows = m_settings.shadow_softness > 0.f;
  }
  if (const char* sparks = std::getenv("JAK_ENHANCED_SPARKS")) {
    m_settings.blast_sparks = (float)std::atof(sparks);
  }
  // what's set now (saved choices plus test switches) counts as saved: the file is written only
  // when the player changes something in the menu, so a test run's switches don't stick
  m_saved_settings = menu_settings_json();

  // bound in place of the bloom / occlusion when they're off, so the composite always samples
  m_black_tex = make_1x1(0);
  m_white_tex = make_1x1(0xffffffff);
  // stands in for the sun shadow map until it's drawn: a shadow sampler must always see a depth
  // texture with compare mode on (depth 1 = nothing in the way)
  glGenTextures(1, &m_no_shadow_tex);
  glBindTexture(GL_TEXTURE_2D, m_no_shadow_tex);
  const float one = 1.f;
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glBindTexture(GL_TEXTURE_2D, 0);
}

EnhancedPost::~EnhancedPost() {
  free_levels();
  free_ssao();
  if (m_focus_pbo[0]) {
    glDeleteBuffers(2, m_focus_pbo);
  }
  if (m_shadow_raw_sampler) {
    glDeleteSamplers(1, &m_shadow_raw_sampler);
  }
  if (m_shaft_fbo) {
    glDeleteFramebuffers(1, &m_shaft_fbo);
    glDeleteTextures(1, &m_shaft_tex);
  }
  if (m_scene_fbo) {
    glDeleteFramebuffers(1, &m_scene_fbo);
    glDeleteTextures(1, &m_scene_tex);
  }
  if (m_ocean_fbo[0]) {
    glDeleteFramebuffers(3, m_ocean_fbo);
    glDeleteTextures(3, m_ocean_depth);
  }
  if (m_gi_fbo[0]) {
    glDeleteFramebuffers(2, m_gi_fbo);
    glDeleteTextures(2, m_gi_tex);
  }
  if (m_taa_fbo[0]) {
    glDeleteFramebuffers(2, m_taa_fbo);
    glDeleteTextures(2, m_taa_tex);
  }
  if (m_vol_fbo) {
    glDeleteFramebuffers(1, &m_vol_fbo);
    glDeleteTextures(1, &m_vol_tex);
  }
  if (m_shadow_fbo) {
    glDeleteFramebuffers(1, &m_shadow_fbo);
    glDeleteTextures(1, &m_shadow_tex);
    glDeleteVertexArrays(1, &m_shadow_vao);
  }
  for (auto& [id, draws] : m_shadow_draws) {
    for (auto& d : draws) {
      if (d.own_indices) {
        glDeleteBuffers(1, &d.indices);
      }
    }
  }
  glDeleteTextures(1, &m_black_tex);
  glDeleteTextures(1, &m_no_shadow_tex);
  glDeleteTextures(1, &m_white_tex);
}

namespace {
float halton(u32 i, u32 base) {
  float f = 1.f;
  float r = 0.f;
  while (i > 0) {
    f /= (float)base;
    r += f * (float)(i % base);
    i /= base;
  }
  return r;
}
}  // namespace

void EnhancedPost::begin_frame(ShaderLibrary& shaders, int w, int h) {
  g_enhanced_lights.count = 0;  // the glows of this frame add themselves
  // the characters of this frame add themselves (Merc2), for the sun shadow map
  g_enhanced_casters.bones.clear();
  g_enhanced_casters.draws.clear();
  g_enhanced_casters.next_mod_copy = 0;
  g_enhanced_casters.capture =
      m_settings.enabled && m_settings.shadow_map && m_settings.shadow_casters;
  g_enhanced_normals.enabled = m_settings.enabled && m_settings.geometry_normals;
  if (g_enhanced_normals.enabled && g_enhanced_normals.fbo) {
    // a = 0: no normal (the post pass rebuilds one from depth there)
    GLint prev = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_enhanced_normals.fbo);
    const GLenum both[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, both);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    glClearBufferfv(GL_COLOR, 1, zero);
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev);
  }
  const bool on = m_settings.enabled && m_settings.taa && w > 0 && h > 0;
  if (!on) {
    if (m_jittered) {
      shaders.set_jitter(0.f, 0.f);
      m_jittered = false;
    }
    m_taa_history = false;
    return;
  }
  m_frame++;
  const u32 i = (m_frame % 8) + 1;
  // pixels in [-0.5, 0.5] -> NDC offset
  shaders.set_jitter((halton(i, 2) - 0.5f) * 2.f / w, (halton(i, 3) - 0.5f) * 2.f / h);
  m_jittered = true;
}

void EnhancedPost::begin_ocean(GLuint render_fbo,
                               int w,
                               int h,
                               SharedRenderState* render_state) {
  m_ocean_ready = false;
  if (!m_settings.enabled || !m_settings.water_reflections) {
    return;
  }
  ensure_ocean_buffers(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ocean_fbo[0]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  render_state->enhanced_ocean_depth_write = true;
}

void EnhancedPost::capture_far_ocean(GLuint render_fbo, int w, int h) {
  m_far_ocean_ready = false;
  if (!m_settings.enabled || !m_settings.water_reflections) {
    return;
  }
  ensure_ocean_buffers(w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ocean_fbo[2]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  m_far_ocean_ready = true;
}

void EnhancedPost::ensure_ocean_buffers(int w, int h) {
  if (w != m_ocean_w || h != m_ocean_h) {
    if (m_ocean_fbo[0]) {
      glDeleteFramebuffers(3, m_ocean_fbo);
      glDeleteTextures(3, m_ocean_depth);
    }
    m_ocean_w = w;
    m_ocean_h = h;
    glGenFramebuffers(3, m_ocean_fbo);
    glGenTextures(3, m_ocean_depth);
    for (int i = 0; i < 3; i++) {
      glBindTexture(GL_TEXTURE_2D, m_ocean_depth[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                   GL_UNSIGNED_INT_24_8, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ocean_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                             m_ocean_depth[i], 0);
      glDrawBuffer(GL_NONE);
      glReadBuffer(GL_NONE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
}

void EnhancedPost::end_ocean(GLuint render_fbo,
                             int w,
                             int h,
                             SharedRenderState* render_state) {
  if (!render_state->enhanced_ocean_depth_write) {
    return;
  }
  render_state->enhanced_ocean_depth_write = false;
  // keep the depth with the ocean surface, then put the original back
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_ocean_fbo[1]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ocean_fbo[0]);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, render_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  m_ocean_ready = true;
}

void EnhancedPost::apply_to_scene(GLuint render_fbo,
                                  int w,
                                  int h,
                                  GLuint screen_vao,
                                  SharedRenderState* render_state,
                                  ScopedProfilerNode& prof) {
  if (!m_settings.enabled) {
    g_enhanced_camera.valid = false;
    // the ambient sounds go quiet with the rest of Enhanced
    static const float silent[snd::ambience::kLayers] = {};
    snd::ambience::set_targets(silent);
    return;
  }
  if (w != m_scene_w || h != m_scene_h) {
    if (m_scene_fbo) {
      glDeleteFramebuffers(1, &m_scene_fbo);
      glDeleteTextures(1, &m_scene_tex);
    }
    m_scene_w = w;
    m_scene_h = h;
    glGenFramebuffers(1, &m_scene_fbo);
    glGenTextures(1, &m_scene_tex);
    glBindTexture(GL_TEXTURE_2D, m_scene_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scene_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // copy (resolving msaa) the scene so we can read it while drawing over it
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scene_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  const EnhancedCamera cam = g_enhanced_camera;  // prepare() clears the global
  prepare(m_scene_tex, render_fbo, w, h, screen_vao, render_state, prof);
  const bool have_scene = m_ssao_ready;
  update_scene();
  if (have_scene && m_cine > 0.f) {
    read_focus(w, h);
  } else if (m_cine <= 0.f) {
    m_focus = 0.f;
  }

  // view -> previous view is R^T Rp, with the camera's move as an offset (motion blur, TAA)
  float to_prev[3][3];  // [k][c]: column k of the GLSL mat3
  float offset[3];
  bool reproject = false;
  if (have_scene && m_prev_camera.valid && cam.valid) {
    float moved = 0.f;
    for (int k = 0; k < 3; k++) {
      for (int c = 0; c < 3; c++) {
        to_prev[k][c] = 0.f;
        for (int r = 0; r < 3; r++) {
          to_prev[k][c] += cam.rot[r][k] * m_prev_camera.rot[r][c];
        }
      }
    }
    for (int c = 0; c < 3; c++) {
      offset[c] = 0.f;
      for (int r = 0; r < 3; r++) {
        offset[c] += (cam.trans[r] - m_prev_camera.trans[r]) * m_prev_camera.rot[r][c];
      }
      moved += (cam.trans[c] - m_prev_camera.trans[c]) * (cam.trans[c] - m_prev_camera.trans[c]);
    }
    // a cut or a teleport isn't motion
    reproject = std::sqrt(moved) < 8.f * kMeter;
  }

  // temporal anti-aliasing: this frame blended into the reprojected history
  GLuint scene_tex = m_scene_tex;
  const bool taa = m_settings.taa && m_jittered && have_scene;
  if (taa) {
    if (w != m_taa_w || h != m_taa_h) {
      if (m_taa_fbo[0]) {
        glDeleteFramebuffers(2, m_taa_fbo);
        glDeleteTextures(2, m_taa_tex);
      }
      m_taa_w = w;
      m_taa_h = h;
      glGenFramebuffers(2, m_taa_fbo);
      glGenTextures(2, m_taa_tex);
      for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, m_taa_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_taa_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_taa_tex[i],
                               0);
      }
      glBindTexture(GL_TEXTURE_2D, 0);
      m_taa_history = false;
    }
    const int cur = m_taa_cur;
    glBindFramebuffer(GL_FRAMEBUFFER, m_taa_fbo[cur]);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glBindVertexArray(screen_vao);
    auto& resolve = render_state->shaders[ShaderId::ENH_TAA];
    resolve.activate();
    auto rid = resolve.id();
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_taa_tex[1 - cur]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_scene_tex);
    glUniform1i(glGetUniformLocation(rid, "tex_T0"), 0);
    glUniform1i(glGetUniformLocation(rid, "tex_T1"), 1);
    glUniform1i(glGetUniformLocation(rid, "tex_T3"), 3);
    glUniform4f(glGetUniformLocation(rid, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
                m_last_proj[3]);
    glUniform2f(glGetUniformLocation(rid, "texel"), 1.f / w, 1.f / h);
    if (reproject) {
      glUniformMatrix3fv(glGetUniformLocation(rid, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
      glUniform3f(glGetUniformLocation(rid, "to_prev_offset"), offset[0], offset[1], offset[2]);
    }
    glUniform2f(glGetUniformLocation(rid, "prev_f"), m_prev_camera.fx, m_prev_fy);
    glUniform2f(glGetUniformLocation(rid, "keep"), m_settings.taa_history,
                m_settings.taa_history_fast);
    glUniform1i(glGetUniformLocation(rid, "has_history"), m_taa_history && reproject);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
    scene_tex = m_taa_tex[cur];
    m_taa_cur = 1 - cur;
    m_taa_history = true;
  } else {
    m_taa_history = false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glBindVertexArray(screen_vao);
  auto& shader = activate_composite(render_state, w, h);
  auto id = shader.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  // the game's brightness/contrast is applied later, on the whole frame
  glUniform4f(glGetUniformLocation(id, "color_mult"), 1.f, 1.f, 1.f, 1.f);
  glUniform4f(glGetUniformLocation(id, "color_add"), 0.f, 0.f, 0.f, 0.f);

  // world up in view space, for the fog and the heat haze
  glUniform3f(glGetUniformLocation(id, "up_view"), cam.rot[1][0], cam.rot[1][1], cam.rot[1][2]);
  glUniform4f(glGetUniformLocation(id, "fog"), m_settings.fog_density / kMeter,
              m_settings.fog_height / kMeter, have_scene ? m_settings.fog_amount * m_fog : 0.f, 0.f);
  glUniform3f(glGetUniformLocation(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f);
  m_time += 1.0 / 60.0;
  glUniform4f(glGetUniformLocation(id, "haze"), have_scene ? m_settings.haze_strength : 0.f,
              120.f * kMeter, 400.f * kMeter, (float)m_time);
  glUniform1i(glGetUniformLocation(id, "fxaa"), m_settings.fxaa && !taa);

  // water reflections: the ocean surface depth from begin/end_ocean this frame
  const bool near_water = have_scene && m_ocean_ready && m_settings.water_reflections;
  const bool far_water = have_scene && m_far_ocean_ready && m_settings.water_reflections;
  const bool water = near_water || far_water;
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, near_water ? m_ocean_depth[1] : m_black_tex);
  glActiveTexture(GL_TEXTURE6);
  glBindTexture(GL_TEXTURE_2D, far_water ? m_ocean_depth[2] : m_black_tex);
  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D, near_water ? m_ocean_depth[0] : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T5"), 5);
  glUniform1i(glGetUniformLocation(id, "tex_T6"), 6);
  glUniform1i(glGetUniformLocation(id, "tex_T14"), 14);
  m_far_ocean_ready = false;
  glUniform1f(glGetUniformLocation(id, "water_strength"),
              water ? m_settings.water_reflection_strength : 0.f);
  // the camera's right and forward in view space, and its position, for the ripples
  glUniform3f(glGetUniformLocation(id, "right_view"), cam.rot[0][0], cam.rot[0][1], cam.rot[0][2]);
  glUniform3f(glGetUniformLocation(id, "fwd_view"), cam.rot[2][0], cam.rot[2][1], cam.rot[2][2]);
  glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  {
    // sunny: Jak 1's turquoise sea
    const float deep[3] = {0.02f, 0.3f, 0.4f}, shallow[3] = {0.12f, 0.68f, 0.66f};
    float d[3], sh[3];
    for (int i = 0; i < 3; i++) {
      d[i] = m_settings.water_deep[i] + (deep[i] - m_settings.water_deep[i]) * m_sunny;
      sh[i] = m_settings.water_shallow[i] + (shallow[i] - m_settings.water_shallow[i]) * m_sunny;
    }
    glUniform3fv(glGetUniformLocation(id, "water_deep_color"), 1, d);
    glUniform3fv(glGetUniformLocation(id, "water_shallow_color"), 1, sh);
  }
  glUniform1f(glGetUniformLocation(id, "water_unify"), m_settings.water_unify);
  glUniform1f(glGetUniformLocation(id, "water_swell_strength"), m_settings.water_swell);
  glUniform1f(glGetUniformLocation(id, "water_refraction"), m_settings.water_refraction);
  glUniform1f(glGetUniformLocation(id, "water_caustics"), m_settings.water_caustics);
  // in Jak 3's desert the ocean renderer draws the oases: ponds, murky, their rough bed hidden
  // as in the game
  glUniform1i(glGetUniformLocation(id, "pond"), m_pond ? 1 : 0);
  glUniform1f(glGetUniformLocation(id, "relight"), m_settings.relight);
  {
    // dynamic lights: the glows in view space, the brightest and nearest first, at most kLights
    constexpr int kLights = 32;
    struct Candidate {
      float pos[4];  // view space, radius
      float color[4];
      float score;
    };
    std::vector<Candidate> candidates;
    struct Fire {
      float pos[4];  // view space, flame size
      float seed;
      float dist;
    };
    std::vector<Fire> fires;
    // the glows too far to light anything: drawn as points of light that carry across the dark
    // (a camp's fire, a town's lamps over the night desert)
    struct Beacon {
      float pos[4];    // view space, glow size
      float color[4];  // rgb, how far in it is (fades in past 40 m)
      float score;
    };
    std::vector<Beacon> beacons;
    static int debug_beacons = 0;
    const bool on = m_settings.lights && have_scene && cam.valid;
    g_enhanced_smoke_lights.count = 0;
    for (int i = 0; on && i < g_enhanced_lights.count; i++) {
      const auto& l = g_enhanced_lights.lights[i];
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (l.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      float radius = std::clamp(l.size * m_settings.light_reach, 4.f * kMeter, 30.f * kMeter);
      c.pos[3] = radius;
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // far ones in front: beacons (not the moon or the sky's glows: huge or kilometers away)
      // (not a fire's sparks and embers either: tiny glows)
      if (dist > 40.f * kMeter && dist < 2500.f * kMeter && c.pos[2] > 0.f &&
          l.size > 0.6f * kMeter && l.size < 12.f * kMeter) {
        float peak = std::max(l.color[0], std::max(l.color[1], l.color[2]));
        if (peak > 0.f) {
          Beacon b;
          std::memcpy(b.pos, c.pos, sizeof(float) * 3);
          b.pos[3] = l.size;
          for (int k = 0; k < 3; k++) {
            b.color[k] = l.color[k] / peak;
          }
          b.color[3] = std::clamp((dist / kMeter - 40.f) / 40.f, 0.f, 1.f);
          b.score = (l.color[0] + l.color[1] + l.color[2]) / (dist / kMeter);
          beacons.push_back(b);
        }
      }
      // behind the camera beyond its reach, or far away (the moon is a glow too)
      if (c.pos[2] < -radius || dist > 150.f * kMeter) {
        continue;
      }
      // the glow's hue at full value; its brightness (glows are faint, 0.1-0.3) scaled up
      float peak = std::max(l.color[0], std::max(l.color[1], l.color[2]));
      float bright = l.color[0] + l.color[1] + l.color[2];
      if (peak <= 0.f) {
        continue;
      }
      float intensity = std::min(1.5f, bright * 4.f) * m_settings.light_strength;
      // a fire's glow: orange to yellow, no blue, flame sized (shots are pure red or purple, the
      // far beacons and the sky's glows are bigger), and staying put
      const bool warm = m_settings.fire && l.color[2] < 0.25f * l.color[0] &&
                        l.color[1] > 0.1f * l.color[0] && l.color[1] < 0.9f * l.color[0] &&
                        l.size > 0.3f * kMeter && l.size < 6.f * kMeter && dist < 60.f * kMeter;
      if (const auto* f = warm ? note_fire_glow(l.pos, l.size) : nullptr) {
        // its light flickers on top of the game's own (a seed from where it stands)
        float seed = std::fmod(std::abs(f->pos[0] * 0.013f + f->pos[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        float n = 0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                  0.2f * std::sin(t * 37.f + 2.3f * seed);
        intensity *= 0.82f + 0.18f * n;
      }
      // a glow of one pure hue (a red warning lamp, an indicator): its light stays small and
      // soft, or a few of them flood a whole district in red (Haven's industrial district)
      {
        float lo = std::min(l.color[0], std::min(l.color[1], l.color[2]));
        float second = bright - peak - lo;
        float pure = std::clamp((0.3f - second / peak) / 0.2f, 0.f, 1.f);
        intensity *= 1.f - 0.65f * pure;
        c.pos[3] = radius * (1.f - 0.45f * pure);
      }
      for (int k = 0; k < 3; k++) {
        c.color[k] = l.color[k] / peak * intensity;
      }
      c.color[3] = 0.f;
      c.score = bright * c.pos[3] / (dist + c.pos[3]);
      // a light is often drawn as two glows at the same spot: keep the one
      bool dup = false;
      for (auto& o : candidates) {
        float dx = o.pos[0] - c.pos[0], dy = o.pos[1] - c.pos[1], dz = o.pos[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz < 0.25f * kMeter * kMeter) {
          dup = true;
          if (c.score > o.score) {
            o = c;
          }
          break;
        }
      }
      if (!dup) {
        candidates.push_back(c);
      }
    }
    // explosions: a bright flash fading in half a second (sorted first: the score is huge)
    {
      std::lock_guard<std::mutex> lock(g_enhanced_explosions.mutex);
      for (int i = 0; i < g_enhanced_explosions.count; i++) {
        const float* e = g_enhanced_explosions.pending[i];
        if (m_blasts.size() < 16) {
          Blast b{{e[0], e[1], e[2]}, std::max(e[3], 1.f * kMeter), m_time};
          b.dir[0] = e[4];
          b.dir[1] = e[5];
          b.dir[2] = e[6];
          b.kind = (int)e[7];
          m_blasts.push_back(b);
        }
      }
      g_enhanced_explosions.count = 0;
    }
    m_blasts.erase(std::remove_if(m_blasts.begin(), m_blasts.end(),
                                  [&](const Blast& b) {
                                    // gun blasts are short: free their slots for the next ones
                                    const double life = b.kind == 2 ? 0.3 : b.kind != 0 ? 0.7 : 1.6;
                                    return m_time - b.start > life;
                                  }),
                   m_blasts.end());
    for (const auto& b : m_blasts) {
      if (!on || m_settings.blast <= 0.f) {
        break;
      }
      float age = (float)(m_time - b.start);
      if (age > 0.6f) {
        continue;  // the flash is over (the sparks fly on)
      }
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (b.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      c.pos[3] = std::clamp(b.radius * 4.f, 8.f * kMeter, 30.f * kMeter);
      float k = 4.f * std::exp(-age * 5.f) * m_settings.blast * m_settings.light_strength;
      // the red gun: a short red-orange flash carried ahead of the muzzle with its wave
      // (the charged shot: red-fuchsia)
      const float tint[3] = {1.f, b.kind == 1 ? 0.32f : b.kind == 3 ? 0.2f : 0.62f,
                             b.kind == 1 ? 0.1f : b.kind == 3 ? 0.5f : 0.3f};
      if (b.kind == 1 || b.kind == 3) {
        if (age > 0.35f) {
          continue;
        }
        const float ahead = (1.f + 20.f * age) * kMeter;
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            c.pos[col] += b.dir[row] * ahead * cam.rot[row][col];
          }
        }
        c.pos[3] = std::max(11.f * kMeter, b.radius * 7.f);
        k = 5.f * std::exp(-age * 9.f) * m_settings.blast * m_settings.light_strength;
      } else if (b.kind == 2) {
        // charging: a small flicker on the gun, stronger as it charges (radius 0.5-1.5 m)
        if (age > 0.2f) {
          continue;
        }
        c.pos[3] = b.radius * 4.f;
        k = 1.5f * (b.radius / kMeter) * std::exp(-age * 12.f) * m_settings.blast *
            m_settings.light_strength;
      }
      c.color[0] = tint[0] * k;
      c.color[1] = tint[1] * k;
      c.color[2] = tint[2] * k;
      c.color[3] = 0.f;
      c.score = 1e6f;
      candidates.push_back(c);
    }
    // the levels' own lamps (light spheres), sent by the game with their time-of-day palette
    int lamps = 0;
    if (on) {
      std::lock_guard<std::mutex> lock(g_enhanced_game_lights.mutex);
      lamps = g_enhanced_game_lights.count;
      for (int i = 0; i < lamps; i++) {
        const float* lp = g_enhanced_game_lights.pos[i];
        const float* lc = g_enhanced_game_lights.color[i];
        Candidate c;
        for (int col = 0; col < 3; col++) {
          c.pos[col] = 0.f;
          for (int row = 0; row < 3; row++) {
            c.pos[col] += (lp[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        float radius = std::clamp(lp[3] * m_settings.lamp_reach, 2.f * kMeter, 30.f * kMeter);
        c.pos[3] = radius;
        float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
        if (c.pos[2] < -radius || dist > 150.f * kMeter) {
          continue;
        }
        float bright = lc[0] + lc[1] + lc[2];
        for (int k = 0; k < 3; k++) {
          c.color[k] =
              lc[k] * m_settings.light_strength * m_settings.lamp_strength * game_lamp_scale();
        }
        c.color[3] = 0.f;
        c.score = bright * radius / (dist + radius);
        candidates.push_back(c);
      }
    }
    // the fires and lamps the levels place (torches, braziers, gas lamps): each its own light,
    // stronger as the night falls, reaching further than the glows (a town's torches light its
    // gate from across the desert). Not where a glow already lights the same spot.
    {
      std::lock_guard<std::mutex> lock(g_enhanced_fires.mutex);
      if (g_enhanced_fires.count > 0) {
        m_fire_spawners.clear();
        for (int i = 0; i < g_enhanced_fires.count; i++) {
          const float* fp = g_enhanced_fires.pending[i];
          m_fire_spawners.push_back({fp[0], fp[1], fp[2], (float)g_enhanced_fires.kind[i]});
        }
        g_enhanced_fires.count = 0;
        m_fire_spawners_time = m_time;
      } else if (m_time - m_fire_spawners_time > 0.3) {
        m_fire_spawners.clear();
      }
    }
    const float fire_night = std::max(m_night, night_darkness());
    const size_t glow_lights = candidates.size();
    for (const auto& fs : m_fire_spawners) {
      if (!on || !m_settings.fire) {
        break;
      }
      const int kind = (int)fs[3];
      // a little above the spawner: the flame, not its base; a fire's light wanders with its
      // flames (the shadows around it tremble)
      const float ft = (float)m_time + std::fmod(std::abs(fs[0] * 0.011f + fs[2] * 0.017f), 6.2832f);
      const float wander = kind == EnhancedFires::FIRE ? 0.12f * kMeter : 0.f;
      const float wp[3] = {fs[0] + wander * std::sin(ft * 7.3f) * std::sin(ft * 2.1f),
                           fs[1] + (kind == EnhancedFires::CANDLE ? 0.2f : 0.6f) * kMeter +
                               wander * 0.6f * std::sin(ft * 9.7f),
                           fs[2] + wander * std::cos(ft * 6.1f) * std::sin(ft * 1.7f)};
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (wp[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      const float reach = kind == EnhancedFires::CANDLE ? 4.f : (kind == EnhancedFires::FIRE ? 13.f : 11.f);
      c.pos[3] = reach * kMeter * (0.75f + 0.25f * m_settings.light_reach / 2.f);
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // a placed fire close by also gets the embers and hot air (a torch's flame is often too
      // small a glow to be told a fire by its light)
      if (kind == EnhancedFires::FIRE && dist < 45.f * kMeter && c.pos[2] > -1.f * kMeter) {
        Fire v;
        std::memcpy(v.pos, c.pos, sizeof(float) * 3);
        v.pos[3] = 0.45f * kMeter;
        v.dist = dist;
        v.seed = std::fmod(std::abs(fs[0] * 0.013f + fs[2] * 0.007f), 6.2832f);
        fires.push_back(v);
      }
      bool lit_already = false;
      for (size_t j = 0; j < glow_lights && !lit_already; j++) {
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) {
          float dk = candidates[j].pos[k] - c.pos[k];
          d2 += dk * dk;
        }
        lit_already = d2 < 3.f * kMeter * 3.f * kMeter;
      }
      static const float kColors[4][3] = {
          {1.f, 0.55f, 0.22f}, {1.f, 0.82f, 0.55f}, {0.45f, 0.65f, 1.f}, {1.f, 0.7f, 0.35f}};
      const float* col3 = kColors[kind & 3];
      if (c.pos[2] > 0.f && dist > 40.f * kMeter && dist < 2500.f * kMeter) {
        Beacon b;
        std::memcpy(b.pos, c.pos, sizeof(float) * 3);
        b.pos[3] = 1.f * kMeter;
        std::memcpy(b.color, col3, sizeof(float) * 3);
        b.color[3] = std::clamp((dist / kMeter - 40.f) / 40.f, 0.f, 1.f);
        b.score = 2.f / (dist / kMeter);
        beacons.push_back(b);
      }
      if (lit_already || c.pos[2] < -c.pos[3] || dist > 400.f * kMeter) {
        continue;
      }
      float k = (0.35f + 0.9f * fire_night) * m_settings.light_strength;
      if (kind == EnhancedFires::FIRE || kind == EnhancedFires::CANDLE) {
        float seed = std::fmod(std::abs(fs[0] * 0.013f + fs[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        k *= 0.8f + 0.2f * (0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                            0.2f * std::sin(t * 37.f + 2.3f * seed));
      }
      for (int i = 0; i < 3; i++) {
        c.color[i] = col3[i] * k * (kind == EnhancedFires::CANDLE ? 0.5f : 1.2f);
      }
      c.color[3] = 0.f;
      // far ones still count (their light on a gate across the sand), near ones first
      c.score = 1.5f * c.pos[3] / (dist + c.pos[3]);
      candidates.push_back(c);
    }
    // Jak's lantern: on foot or on the board in the desert as the night darkens, a Wastelander's
    // lantern bulb (Damas's people cross the sands by night with them), a little machine of its own:
    // it rises off Jak's shoulder as the dark comes, hovers up to his right and follows him on a
    // spring, lagging, swaying and settling, so its warm light shifts as he moves
    m_lantern[3] = 0.f;
    {
      const auto& pl = g_enhanced_player;
      const bool on_foot = pl.valid && pl.state != 2 && pl.state != 5 && pl.state != 7;
      // the first time Jak stands in the desert (once per install): night falls and the bulb rises,
      // with a word on it on the screen (request 10: the hour, the hint)
      if (!m_settings.lantern_intro_done && m_settings.desert_lamp && m_pond && pl.valid &&
          (pl.state == 1 || pl.state == 3) && !g_enhanced_scene.movie) {
        g_enhanced_menu.ask(10, "22");
        m_lamp_mode = 0;
        m_settings.lantern_intro_done = true;
        save_menu_settings();
      }
      // on by itself in the desert at night; holding triangle switches it on anywhere, any hour,
      // or off (switched off it stays off until the next desert night)
      const float auto_on = (m_settings.desert_lamp && m_pond)
                                ? std::clamp((std::max(m_night, night_darkness()) - 0.25f) / 0.4f, 0.f, 1.f)
                                : 0.f;
      if (pl.valid && pl.lamp_toggles != m_lamp_toggles_seen) {
        if (m_lamp_toggles_seen >= 0) {
          const bool lit = m_lamp_mode == 1 || (m_lamp_mode == 0 && auto_on > 0.5f);
          m_lamp_mode = lit ? 2 : 1;
        }
        m_lamp_toggles_seen = pl.lamp_toggles;
      }
      if (m_lamp_mode == 2 && auto_on <= 0.f) {
        m_lamp_mode = 0;
      }
      const float lit = m_lamp_mode == 1 ? 1.f : (m_lamp_mode == 2 ? 0.f : auto_on);
      const float want = on_foot ? lit : 0.f;
      m_lamp_on += (want - m_lamp_on) * 0.05f;
      const float dt = std::clamp((float)(m_time - m_bulb_time), 0.f, 0.1f);
      m_bulb_time = m_time;
      if (on && pl.valid && m_lamp_on > 0.01f) {
        float fx = cam.rot[0][2], fz = cam.rot[2][2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1e-3f);
        fx /= len;
        fz /= len;
        // the camera's right, flat
        const float rx = -fz, rz = fx;
        const float t = (float)m_time;
        // its spot: up to Jak's right and a little behind, drifting on its own
        const float drift_side = 0.18f * std::sin(t * 0.63f) + 0.07f * std::sin(t * 1.9f + 0.4f);
        const float drift_up = 0.14f * std::sin(t * 0.91f + 1.1f) + 0.05f * std::sin(t * 2.7f);
        const float drift_fwd = 0.15f * std::sin(t * 0.47f + 2.3f);
        const float side = 1.0f + drift_side, up = 2.35f + drift_up, fwd = -0.3f + drift_fwd;
        // its place next to Jak (m), where it heads
        const float target[3] = {rx * side + fx * fwd, up, rz * side + fz * fwd};
        // Jak's velocity (m/s): the bulb keeps up with him, but his starts, stops and turns jolt it
        float jv[3] = {0.f, 0.f, 0.f};
        if (m_bulb_out && dt > 0.f) {
          for (int k = 0; k < 3; k++) {
            jv[k] = (pl.pos[k] - m_bulb_jak[k]) / kMeter / dt;
          }
        }
        const float jump2 = jv[0] * jv[0] + jv[1] * jv[1] + jv[2] * jv[2];
        if (!m_bulb_out || jump2 > 40.f * 40.f) {
          // it starts on Jak's shoulder (or catches up after a teleport)
          m_bulb_off[0] = rx * 0.3f;
          m_bulb_off[1] = 1.5f;
          m_bulb_off[2] = rz * 0.3f;
          for (int k = 0; k < 3; k++) {
            m_bulb_vel[k] = 0.f;
            m_bulb_jv[k] = 0.f;
            jv[k] = 0.f;
          }
          m_bulb_out = true;
        }
        // a soft spring, near critical damping: it trails Jak a little as he moves (half a meter at a
        // run), sways a touch when he starts, stops or turns, settles without bouncing
        const float kSpring = 10.f, kDamp = 5.6f;
        for (int k = 0; k < 3; k++) {
          m_bulb_vel[k] -= (jv[k] - m_bulb_jv[k]) * 0.12f;
          m_bulb_jv[k] += (jv[k] - m_bulb_jv[k]) * std::min(1.f, dt * 6.f);
          const float trail = std::clamp(-m_bulb_jv[k] * 0.08f, -0.8f, 0.8f);
          float a = kSpring * (target[k] + trail - m_bulb_off[k]) - kDamp * m_bulb_vel[k];
          m_bulb_vel[k] += a * dt;
          m_bulb_off[k] += m_bulb_vel[k] * dt;
          m_bulb_jak[k] = pl.pos[k];
          m_bulb_pos[k] = pl.pos[k] + m_bulb_off[k] * kMeter;
        }
        Candidate c;
        for (int col = 0; col < 3; col++) {
          c.pos[col] = 0.f;
          for (int row = 0; row < 3; row++) {
            c.pos[col] += (m_bulb_pos[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        c.pos[3] = 14.f * kMeter;
        // a steady bulb with a faint hum in it
        const float hum = 0.94f + 0.04f * std::sin(t * 17.f) * std::sin(t * 3.3f) +
                          0.02f * std::sin(t * 41.f);
        const float k = m_lamp_on * hum * m_settings.light_strength;
        c.color[0] = 1.25f * k;
        c.color[1] = 0.62f * k;
        c.color[2] = 0.22f * k;
        c.color[3] = 1.f;  // with Jak: no shadow of him from it
        c.score = 100.f;  // always among the lights
        candidates.push_back(c);
        std::memcpy(m_lantern, c.pos, sizeof(float) * 3);
        m_lantern[3] = m_lamp_on * hum;
      } else if (m_lamp_on <= 0.01f) {
        m_bulb_out = false;  // next time it rises off the shoulder again
      }
    }
    // the lights placed by hand (lights.json), the night ones lit as the night falls
    for (const auto& ml : g_manual_lights[game_index()]) {
      const float lit = ml.night ? std::max(m_night, night_darkness()) : 1.f;
      if (!on || lit <= 0.01f) {
        break;
      }
      Candidate c;
      for (int col = 0; col < 3; col++) {
        c.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          c.pos[col] += (ml.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      c.pos[3] = std::min(ml.radius, 40.f * kMeter);
      float dist = std::sqrt(c.pos[0] * c.pos[0] + c.pos[1] * c.pos[1] + c.pos[2] * c.pos[2]);
      // it has no bulb of its own in the game: its beacon is the lamp, near or far
      if (ml.bulb && c.pos[2] > 0.f && dist < 2500.f * kMeter) {
        float peak = std::max(ml.color[0], std::max(ml.color[1], ml.color[2]));
        if (peak > 0.f) {
          Beacon b;
          std::memcpy(b.pos, c.pos, sizeof(float) * 3);
          b.pos[3] = 0.4f * kMeter;
          for (int k = 0; k < 3; k++) {
            b.color[k] = ml.color[k] / peak;
          }
          b.color[3] = lit;
          b.score = 3.f * peak / std::max(dist / kMeter, 1.f);
          beacons.push_back(b);
        }
      }
      if (c.pos[2] < -c.pos[3] || dist > 150.f * kMeter) {
        continue;
      }
      float k = lit * m_settings.light_strength * m_settings.lamp_strength;
      if (ml.flicker) {
        float seed = std::fmod(std::abs(ml.pos[0] * 0.013f + ml.pos[2] * 0.007f), 6.2832f);
        float t = (float)m_time;
        k *= 0.82f + 0.18f * (0.5f * std::sin(t * 11.f + seed) + 0.3f * std::sin(t * 23.f + 1.7f * seed) +
                              0.2f * std::sin(t * 37.f + 2.3f * seed));
      }
      for (int i = 0; i < 3; i++) {
        c.color[i] = ml.color[i] * k;
      }
      c.color[3] = 0.f;
      c.score = (ml.color[0] + ml.color[1] + ml.color[2]) * c.pos[3] / (dist + c.pos[3]);
      candidates.push_back(c);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    const int n = std::min((int)candidates.size(), kLights);
    float pos[kLights * 4] = {};
    float color[kLights * 4] = {};
    for (int i = 0; i < n; i++) {
      std::memcpy(pos + i * 4, candidates[i].pos, sizeof(float) * 4);
      std::memcpy(color + i * 4, candidates[i].color, sizeof(float) * 4);
    }
    {
      // the strongest few back in world space for the smoke particles, drawn next frame
      auto& sl = g_enhanced_smoke_lights;
      sl.count = std::min(n, EnhancedSmokeLights::kMax);
      for (int i = 0; i < sl.count; i++) {
        const auto& c = candidates[i];
        for (int row = 0; row < 3; row++) {
          sl.pos[i][row] = cam.trans[row];
          for (int col = 0; col < 3; col++) {
            sl.pos[i][row] += cam.rot[row][col] * c.pos[col];
          }
        }
        sl.pos[i][3] = c.pos[3];
        std::memcpy(sl.color[i], c.color, sizeof(float) * 4);
      }
      sl.strength = m_settings.lit_smoke;
    }
    // the fires: forget the ones not seen for a second, the nearest few for the hot air and embers
    m_fires.erase(std::remove_if(m_fires.begin(), m_fires.end(),
                                 [&](const FireMemory& f) { return m_time - f.last > 1.0; }),
                  m_fires.end());
    for (const auto& f : m_fires) {
      if (!on || !is_fire(f)) {
        continue;
      }
      Fire v;
      for (int col = 0; col < 3; col++) {
        v.pos[col] = 0.f;
        for (int row = 0; row < 3; row++) {
          v.pos[col] += (f.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      v.pos[3] = f.size;
      v.dist = std::sqrt(v.pos[0] * v.pos[0] + v.pos[1] * v.pos[1] + v.pos[2] * v.pos[2]);
      v.seed = std::fmod(std::abs(f.pos[0] * 0.013f + f.pos[2] * 0.007f), 6.2832f);
      if (v.dist < 45.f * kMeter && v.pos[2] > -f.size) {
        // (the glow of a placed fire: that one, sized by its glow)
        bool same = false;
        for (auto& o : fires) {
          float d2 = 0.f;
          for (int k = 0; k < 3; k++) {
            d2 += (o.pos[k] - v.pos[k]) * (o.pos[k] - v.pos[k]);
          }
          if (d2 < 2.f * kMeter * 2.f * kMeter) {
            o = v;
            same = true;
            break;
          }
        }
        if (!same) {
          fires.push_back(v);
        }
      }
    }
    std::sort(fires.begin(), fires.end(), [](const Fire& a, const Fire& b) { return a.dist < b.dist; });
    constexpr int kFires = 4;
    float fire_pos[kFires * 4] = {};
    float fire_seed[kFires] = {};
    int fire_count = 0;
    for (const auto& f : fires) {
      if (fire_count == kFires) {
        break;
      }
      std::memcpy(fire_pos + fire_count * 4, f.pos, sizeof(float) * 4);
      fire_seed[fire_count++] = f.seed;
    }
    glUniform1f(glGetUniformLocation(id, "light_shadow"), m_settings.light_shadow);
    glUniform1f(glGetUniformLocation(id, "materials"), m_settings.materials);
    glUniform1f(glGetUniformLocation(id, "relief"), m_settings.relief);
    {
      // Jak's recent path: a point every 0.1 s while he moves (every 0.5 s standing), kept 2 s;
      // the shader starts a ring on the water from each one that lies at the water's height
      // (riding over the water: a point every 0.05 s, foam churned along them)
      constexpr int kWake = 32;
      const auto& pl = g_enhanced_player;
      const bool riding_water = pl.valid && pl.state == 5;
      if (pl.valid && m_settings.wake > 0.f) {
        double since = m_wake.empty() ? 1e9 : m_time - m_wake.back().time;
        float moved = 1e9f;
        if (!m_wake.empty()) {
          const float* q = m_wake.back().pos;
          float dx = pl.pos[0] - q[0], dz = pl.pos[2] - q[2];
          moved = std::sqrt(dx * dx + dz * dz);
        }
        if ((since >= (riding_water ? 0.05 : 0.1) && moved > 0.1f * kMeter) || since >= 0.5) {
          m_wake.push_back({{pl.pos[0], pl.pos[1], pl.pos[2]}, m_time, riding_water});
        }
      }
      while (!m_wake.empty() && (m_time - m_wake.front().time > 2.0 || m_wake.size() > kWake)) {
        m_wake.erase(m_wake.begin());
      }
      float wake[kWake * 4] = {};
      int wake_count = 0;
      for (const auto& w : m_wake) {
        if (m_settings.wake <= 0.f) {
          break;
        }
        std::memcpy(wake + wake_count * 4, w.pos, sizeof(float) * 3);
        // age, + 10 behind a vehicle or the board
        wake[wake_count * 4 + 3] = (float)(m_time - w.time) + (w.ride ? 10.f : 0.f);
        wake_count++;
      }
      glUniform1i(glGetUniformLocation(id, "wake_count"), wake_count);
      glUniform4fv(glGetUniformLocation(id, "wake"), kWake, wake);
      glUniform1f(glGetUniformLocation(id, "wake_strength"), m_settings.wake);
    }
    {
      // footprints where Jak walks on soft ground (the game knows the ground's material), ruts where
      // he drives; kept 30 s
      constexpr int kPrints = 48;
      const auto& pl = g_enhanced_player;
      static bool test_prints = std::getenv("JAK_ENHANCED_PRINTS_TEST") != nullptr;
      static double test_seeded = -1e9;
      if (test_prints && pl.valid && m_time - test_seeded > 10.0) {
        // test switch: footprints walking away from the camera beside Jak, ruts on his other side
        // (again every 10 s, wherever he is by then)
        test_seeded = m_time;
        m_prints.clear();
        float fx = pl.pos[0] - cam.trans[0], fz = pl.pos[2] - cam.trans[2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1.f);
        fx /= len;
        fz /= len;
        for (int i = 0; i < 24; i++) {
          bool rut = i >= 12;
          float along = (-2.f + (i % 12) * (rut ? 0.5f : 0.7f)) * kMeter;
          float side = (rut ? 2.5f : ((i % 2) ? -1.32f : -1.08f)) * kMeter;
          Print p;
          p.pos[0] = pl.pos[0] + fx * along + fz * side;
          p.pos[1] = pl.pos[1] + (rut ? 1.f * kMeter : 0.f);
          p.pos[2] = pl.pos[2] + fz * along - fx * side;
          p.heading = std::atan2(fx, fz);
          p.kind = rut ? 2 : (i % 2);
          p.time = m_time;
          m_prints.push_back(p);
        }
      }
      // a landing from a jump: a big splash where he comes down
      const bool on_foot = pl.state == 1 || pl.state == 3;
      if (pl.valid && on_foot && m_last_player_state == 0) {
        m_steps.push_back({{pl.pos[0], pl.pos[1], pl.pos[2]}, 2.f, m_time});
      }
      m_last_player_state = pl.valid ? pl.state : 0;
      if (pl.valid && pl.state > 0 && pl.state <= 3) {
        bool rut = pl.state == 2;
        const bool soft = pl.state == 1 || pl.state == 2;
        // (ruts: a piece every meter, each one reaching back to the one before: a solid track)
        float stride = (rut ? 1.f : 0.7f) * kMeter;
        float dx = 0.f, dz = 0.f, moved = 1e9f;
        if (m_last_print_valid) {
          dx = pl.pos[0] - m_last_print[0];
          dz = pl.pos[2] - m_last_print[2];
          moved = std::sqrt(dx * dx + dz * dz);
        }
        if (moved > stride) {
          if (moved < 4.f * stride) {  // a step, not a jump or a teleport
            float fx = dx / moved, fz = dz / moved;
            float side = rut ? 0.f : (m_print_left ? 0.12f : -0.12f) * kMeter;
            Print p;
            p.pos[0] = pl.pos[0] + fz * side;
            p.pos[1] = pl.pos[1];
            p.pos[2] = pl.pos[2] - fx * side;
            p.heading = std::atan2(fx, fz);
            p.kind = rut ? 2 : (m_print_left ? 0 : 1);
            p.time = m_time;
            p.seg = moved;
            if (soft && m_settings.prints > 0.f) {
              m_prints.push_back(p);
            }
            if (!rut) {
              m_steps.push_back({{p.pos[0], p.pos[1], p.pos[2]}, 1.f, m_time});
            }
            m_print_left = !m_print_left;
          }
          std::memcpy(m_last_print, pl.pos, sizeof(m_last_print));
          m_last_print_valid = true;
        }
      }
      while (!m_prints.empty() &&
             (m_time - m_prints.front().time > 30.0 || m_prints.size() > (size_t)kPrints)) {
        m_prints.erase(m_prints.begin());
      }
      float prints[kPrints * 4] = {};
      float print_info[kPrints * 2] = {};
      int print_count = 0;
      for (const auto& p : m_prints) {
        if (m_settings.prints <= 0.f) {
          break;
        }
        std::memcpy(prints + print_count * 4, p.pos, sizeof(float) * 3);
        prints[print_count * 4 + 3] = p.heading;
        print_info[print_count * 2] = (float)(m_time - p.time);
        // (ruts: 2 + the piece's length in meters)
        print_info[print_count * 2 + 1] = (float)p.kind + (p.kind == 2 ? p.seg / kMeter : 0.f);
        print_count++;
      }
      glUniform1i(glGetUniformLocation(id, "print_count"), print_count);
      glUniform4fv(glGetUniformLocation(id, "prints"), kPrints, prints);
      glUniform2fv(glGetUniformLocation(id, "print_info"), kPrints, print_info);
      glUniform1f(glGetUniformLocation(id, "prints_strength"), m_settings.prints);

      // splashes: the steps of the last second (the shader checks for a puddle under them)
      constexpr int kSplashes = 16;
      static bool test_splash = std::getenv("JAK_ENHANCED_SPLASH_TEST") != nullptr;
      static double test_splash_t = 0;
      if (test_splash && pl.valid && m_time - test_splash_t > 0.15) {
        // test switch: steps landing one after another on a line ahead of Jak, away from the camera
        test_splash_t = m_time;
        static int n = 0;
        float fx = pl.pos[0] - cam.trans[0], fz = pl.pos[2] - cam.trans[2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1.f);
        const float along = (-1.5f + (n % 6) * 0.6f) * kMeter;
        const float side = ((n % 3) - 1) * 1.2f * kMeter;
        m_steps.push_back({{pl.pos[0] + fx / len * along + fz / len * side, pl.pos[1],
                            pl.pos[2] + fz / len * along - fx / len * side},
                           (n % 4) == 0 ? 2.f : 1.f, m_time});
        n++;
      }
      while (!m_steps.empty() &&
             (m_time - m_steps.front().time > 1.2 || m_steps.size() > (size_t)kSplashes / 2)) {
        m_steps.erase(m_steps.begin());
      }
      // in the rain, drops bounce off the ground around Jak (2-6 m, on his level ground): a few
      // droplets each, hopping a few centimeters
      if (m_rain > 0.1f && pl.valid && (pl.state == 1 || pl.state == 3)) {
        static double bounce_t = 0;
        const double every = 0.11 / m_rain;
        if (m_time - bounce_t > every) {
          bounce_t = m_time;
          const float a = (float)(std::rand() % 10000) / 10000.f * 6.2832f;
          const float r = (2.f + (float)(std::rand() % 1000) / 250.f) * kMeter;
          m_bounces.push_back({{pl.pos[0] + std::cos(a) * r, pl.pos[1], pl.pos[2] + std::sin(a) * r},
                               0.f, m_time});
        }
      }
      while (!m_bounces.empty() &&
             (m_time - m_bounces.front().time > 0.45 || m_bounces.size() > (size_t)kSplashes / 2)) {
        m_bounces.erase(m_bounces.begin());
      }
      float splash[kSplashes * 4] = {};
      int splash_count = 0;
      for (const auto* list : {&m_steps, &m_bounces})
      for (const auto& st : *list) {
        std::memcpy(splash + splash_count * 4, st.pos, sizeof(float) * 3);
        // age, with the strength in the integer part above 10 s: 1 step, 2 landing
        splash[splash_count * 4 + 3] = (float)(m_time - st.time) + 10.f * st.strength;
        splash_count++;
      }
      glUniform1i(glGetUniformLocation(id, "splash_count"),
                  std::max(m_wet, m_puddle) > 0.05f ? splash_count : 0);
      glUniform4fv(glGetUniformLocation(id, "splash"), kSplashes, splash);

      // dust behind a vehicle or the board crossing the desert (two plumes, off the back wheels),
      // spray where they cross water, and a burst where they hit it
      constexpr int kPuffs = 32;
      const bool riding = pl.valid && (pl.state == 2 || pl.state >= 4);
      if (riding && m_ride_last_time >= 0.0 && m_time > m_ride_last_time) {
        float dx = pl.pos[0] - m_ride_last[0], dz = pl.pos[2] - m_ride_last[2];
        float dist = std::sqrt(dx * dx + dz * dz) / kMeter;
        float v = dist / (float)(m_time - m_ride_last_time);
        if (v < 80.f) {  // not a teleport
          m_ride_speed += (v - m_ride_speed) * 0.2f;
          if (dist > 0.02f) {
            m_ride_dir[0] = dx / (dist * kMeter);
            m_ride_dir[1] = dz / (dist * kMeter);
          }
        }
      } else if (!riding) {
        m_ride_speed = 0.f;
      }
      std::memcpy(m_ride_last, pl.pos, sizeof(m_ride_last));
      m_ride_last_time = m_time;
      auto add_puff = [&](float back, float side, float down, float size, int kind, float life) {
        Puff f;
        const float fx = m_ride_dir[0], fz = m_ride_dir[1];
        f.pos[0] = pl.pos[0] - fx * back * kMeter + fz * side * kMeter;
        f.pos[1] = pl.pos[1] - down * kMeter;
        f.pos[2] = pl.pos[2] - fz * back * kMeter - fx * side * kMeter;
        f.size = size;
        f.kind = kind;
        f.seed = (float)(std::rand() % 1000) / 1000.f;
        f.life = life;
        f.time = m_time;
        m_puffs.push_back(f);
      };
      // test switch: puffs behind Jak at any speed, anywhere (dust; spray with the value "water")
      static const char* puff_test = std::getenv("JAK_ENHANCED_PUFF_TEST");
      static bool test_puffs = puff_test != nullptr;
      static bool test_spray = puff_test && std::strcmp(puff_test, "water") == 0;
      if (pl.state == 2 || pl.state == 4 || pl.state == 6 || pl.state == 7) {
        m_last_ride_state = pl.state == 6 ? 4 : pl.state == 7 ? 2 : pl.state;
      }
      const bool board = pl.state == 4 || pl.state == 6 || (pl.state == 5 && m_last_ride_state == 4);
      const bool on_water = pl.state == 5 || test_spray;
      // (none while the board or the vehicle is in the air: a jump leaves no dust)
      if ((riding || test_puffs) && pl.valid && pl.state < 6 && m_time - m_puff_time > 0.06) {
        const float sp = test_puffs ? 20.f : m_ride_speed;
        const float side = (m_puff_left ? 1.f : -1.f) * (board ? 0.2f : 1.1f);
        // (the test: as if moving, along a 12 m trail)
        // (Jak sits ~1.4 m over the ground in a vehicle: the dust starts on the ground behind the
        // back wheels, not up by the seat)
        const float back = (board ? 0.6f : 2.2f) + (test_puffs ? (float)std::fmod(m_time * 6.0, 12.0) : 0.f);
        if (on_water && sp > 3.f && m_settings.wake > 0.f) {
          add_puff(back, side, board ? 0.2f : 0.6f, std::min(0.5f + sp * 0.04f, 2.f), 1, 1.1f);
        } else if (!on_water && (m_pond || test_puffs) && sp > 5.f && m_settings.prints > 0.f) {
          add_puff(back, side, board ? 0.1f : 1.4f, std::min(0.6f + sp * 0.05f, 2.4f), 0, 2.2f);
        }
        m_puff_left = !m_puff_left;
        m_puff_time = m_time;
      }
      // hitting the water at speed: a burst all around
      static int last_state = 0;
      if (pl.valid && pl.state == 5 && (last_state == 2 || last_state == 4) && m_ride_speed > 5.f &&
          m_settings.wake > 0.f) {
        for (int k = 0; k < 4; k++) {
          add_puff((k - 1.5f) * 0.8f, (k % 2 ? 1.f : -1.f) * 1.2f, 0.4f, 2.6f, 1, 1.4f);
        }
      }
      last_state = pl.valid ? pl.state : 0;
      while (!m_puffs.empty() && (m_time - m_puffs.front().time > m_puffs.front().life ||
                                  m_puffs.size() > (size_t)kPuffs)) {
        m_puffs.erase(m_puffs.begin());
      }
      float puff[kPuffs * 4] = {};
      float puff_info[kPuffs * 4] = {};
      int puff_count = 0;
      for (const auto& f : m_puffs) {
        if (m_time - f.time > f.life) {
          continue;
        }
        std::memcpy(puff + puff_count * 4, f.pos, sizeof(float) * 3);
        puff[puff_count * 4 + 3] = (float)(m_time - f.time);
        puff_info[puff_count * 4] = f.size;
        puff_info[puff_count * 4 + 1] = (float)f.kind;
        puff_info[puff_count * 4 + 2] = f.seed;
        puff_info[puff_count * 4 + 3] = f.life;
        puff_count++;
      }
      glUniform1i(glGetUniformLocation(id, "puff_count"), cam.valid ? puff_count : 0);
      glUniform4fv(glGetUniformLocation(id, "puff"), kPuffs, puff);
      glUniform4fv(glGetUniformLocation(id, "puff_info"), kPuffs, puff_info);
    }
    // the nearest explosions in view for the shockwave: view space + radius, age
    {
      constexpr int kBlasts = 4;
      float blast_pos[kBlasts * 4] = {};
      float blast_age[kBlasts] = {};
      float blast_seed[kBlasts] = {};
      float blast_dir[kBlasts * 4] = {};
      int blast_count = 0;
      for (const auto& b : m_blasts) {
        if (blast_count == kBlasts || !on || m_settings.blast <= 0.f) {
          break;
        }
        float v[3] = {0.f, 0.f, 0.f};
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            v[col] += (b.pos[row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        if (v[2] < 1.f * kMeter) {
          continue;
        }
        std::memcpy(blast_pos + blast_count * 4, v, sizeof(v));
        blast_pos[blast_count * 4 + 3] = b.radius;
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            blast_dir[blast_count * 4 + col] += b.dir[row] * cam.rot[row][col];
          }
        }
        blast_dir[blast_count * 4 + 3] = (float)b.kind;
        blast_seed[blast_count] = (float)std::fmod(b.start * 7.31, 100.0);
        blast_age[blast_count++] = (float)(m_time - b.start);
      }
      glUniform1i(glGetUniformLocation(id, "blast_count"), blast_count);
      glUniform4fv(glGetUniformLocation(id, "blast_pos"), kBlasts, blast_pos);
      glUniform1fv(glGetUniformLocation(id, "blast_age"), kBlasts, blast_age);
      glUniform1fv(glGetUniformLocation(id, "blast_seed"), kBlasts, blast_seed);
      glUniform4fv(glGetUniformLocation(id, "blast_dir"), kBlasts, blast_dir);
      glUniform1f(glGetUniformLocation(id, "blast_strength"), m_settings.blast);
    }
    glUniform1i(glGetUniformLocation(id, "fire_count"), fire_count);
    glUniform4fv(glGetUniformLocation(id, "fire_pos"), kFires, fire_pos);
    glUniform1fv(glGetUniformLocation(id, "fire_seed"), kFires, fire_seed);
    glUniform3f(glGetUniformLocation(id, "fire"), m_settings.fire_haze, m_settings.fire_embers,
                (float)std::fmod(m_time, 1000.0));
    // dark nights: how dark (the desert darkest, a city least), and the lights reach further and
    // shine stronger into it
    const float dark = night_darkness();
    glUniform2f(glGetUniformLocation(id, "night_dark"), dark, 1.f + 1.6f * dark);
    glUniform1f(glGetUniformLocation(id, "light_spill"), m_settings.light_spill);
    {
      // Spargus lights the sky over it at night, a warm glow on the horizon seen from the desert
      // (out of town, fading with the distance)
      const float sx = 2050.f * kMeter, sz = -230.f * kMeter;
      const float dx = cam.trans[0] - sx, dz = cam.trans[2] - sz;
      const float dist_m = std::sqrt(dx * dx + dz * dz) / kMeter;
      const float k = (g_game_version == GameVersion::Jak3 && m_pond)
                          ? dark * std::clamp((dist_m - 350.f) / 400.f, 0.f, 1.f) *
                                std::exp(-dist_m / 4000.f) * m_settings.light_strength
                          : 0.f;
      glUniform4f(glGetUniformLocation(id, "town_glow"), sx, sz, k, 0.f);
    }
    glUniform1f(glGetUniformLocation(id, "cloud_shadows"), m_settings.cloud_shadows ? 1.f : 0.f);
    glUniform1f(glGetUniformLocation(id, "lens_flare"),
                g_enhanced_scene.movie ? 0.f : m_settings.lens_flare);
    {
      // Jak in view space: the dark spares a little around him (he stays readable)
      const auto& pl = g_enhanced_player;
      float v[3] = {0.f, 0.f, 0.f};
      for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
          v[col] += (pl.pos[row] - cam.trans[row]) * cam.rot[row][col];
        }
      }
      glUniform4f(glGetUniformLocation(id, "player_view"), v[0], v[1] + 1.f * kMeter, v[2],
                  pl.valid && cam.valid ? 1.f : 0.f);
    }
    for (int i = 0; i < n; i++) {
      pos[i * 4 + 3] *= 1.f + 0.6f * dark;
    }
    {
      // headlights: the nearest few, in view space, shown in the dark only
      {
        std::lock_guard<std::mutex> lock(g_enhanced_headlights.mutex);
        if (g_enhanced_headlights.count > 0) {
          m_headlights.clear();
          for (int i = 0; i < g_enhanced_headlights.count; i++) {
            std::array<float, 8> h;
            std::memcpy(h.data(), g_enhanced_headlights.pending[i], sizeof(float) * 8);
            m_headlights.push_back(h);
          }
          g_enhanced_headlights.count = 0;
          m_headlights_time = m_time;
        } else if (m_time - m_headlights_time > 0.2) {
          m_headlights.clear();
        }
      }
      // test switch: two headlights on Jak, pointing where the camera looks
      static bool test_headlights = std::getenv("JAK_ENHANCED_HEADLIGHT_TEST") != nullptr;
      if (test_headlights && g_enhanced_player.valid) {
        const auto& pl = g_enhanced_player;
        float fx = cam.rot[0][2], fz = cam.rot[2][2];
        float len = std::max(std::sqrt(fx * fx + fz * fz), 1e-3f);
        fx /= len;
        fz /= len;
        m_headlights.clear();
        for (int k = 0; k < 2; k++) {
          float side = (k ? 0.7f : -0.7f) * kMeter;
          m_headlights.push_back({pl.pos[0] + fz * side + fx * kMeter, pl.pos[1] + 0.9f * kMeter,
                                  pl.pos[2] - fx * side + fz * kMeter, 1.f, fx * 0.99f, -0.07f,
                                  fz * 0.99f, 0.f});
        }
      }
      constexpr int kSpots = 8;
      const auto& spots_src = m_headlights;
      std::vector<std::pair<float, int>> order;
      for (int i = 0; i < (int)spots_src.size(); i++) {
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) {
          float dk = spots_src[i][k] - cam.trans[k];
          d2 += dk * dk;
        }
        order.push_back({d2, i});
      }
      std::sort(order.begin(), order.end());
      float spot_pos[kSpots * 4] = {};
      float spot_dir[kSpots * 4] = {};
      int spots = 0;
      const float shown = std::max(m_night, dark);
      for (const auto& [d2, i] : order) {
        if (spots == kSpots || shown <= 0.01f || !m_settings.lights) {
          break;
        }
        const auto& h = spots_src[i];
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            spot_pos[spots * 4 + col] += (h[row] - cam.trans[row]) * cam.rot[row][col];
            spot_dir[spots * 4 + col] += h[4 + row] * cam.rot[row][col];
          }
        }
        spot_pos[spots * 4 + 3] = h[3] * shown;
        spots++;
      }
      glUniform1i(glGetUniformLocation(id, "spot_count"), spots);
      glUniform4fv(glGetUniformLocation(id, "spot_pos"), kSpots, spot_pos);
      glUniform4fv(glGetUniformLocation(id, "spot_dir"), kSpots, spot_dir);
    }
    glUniform1i(glGetUniformLocation(id, "light_count"), n);
    glUniform4fv(glGetUniformLocation(id, "lantern"), 1, m_lantern);
    glUniform4fv(glGetUniformLocation(id, "light_pos"), kLights, pos);
    glUniform4fv(glGetUniformLocation(id, "light_color"), kLights, color);
    {
      // the beacons, the brightest and nearest first, shown as the night falls
      constexpr int kBeacons = 24;
      std::sort(beacons.begin(), beacons.end(),
                [](const Beacon& a, const Beacon& b) { return a.score > b.score; });
      const float shown = std::max(m_night, night_darkness());
      float bpos[kBeacons * 4] = {};
      float bcol[kBeacons * 4] = {};
      int nb = 0;
      for (const auto& b : beacons) {
        if (shown <= 0.01f || nb == kBeacons) {
          break;
        }
        // one point per lamp: a glow next to a brighter one already taken (a flame is several
        // glows) is part of it
        bool merged = false;
        for (int j = 0; j < nb && !merged; j++) {
          float d2 = 0.f;
          for (int k = 0; k < 3; k++) {
            float dk = bpos[j * 4 + k] - b.pos[k];
            d2 += dk * dk;
          }
          merged = d2 < 4.f * kMeter * 4.f * kMeter;
        }
        if (merged) {
          continue;
        }
        std::memcpy(bpos + nb * 4, b.pos, sizeof(float) * 4);
        std::memcpy(bcol + nb * 4, b.color, sizeof(float) * 4);
        bcol[nb * 4 + 3] *= shown * m_settings.light_strength;
        nb++;
      }
      glUniform1i(glGetUniformLocation(id, "beacon_count"), nb);
      glUniform4fv(glGetUniformLocation(id, "beacon_pos"), kBeacons, bpos);
      glUniform4fv(glGetUniformLocation(id, "beacon_color"), kBeacons, bcol);
      if (m_settings.ssao_debug && (debug_beacons++ % 300) == 0) {
        lg::info("[enhanced] beacons: {} far glows, {} shown", beacons.size(), nb);
      }
    }
    static int debug_counter = 0;
    if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
      lg::info("[enhanced] lights: {} glows, {} lamps, {} used", g_enhanced_lights.count, lamps, n);
      for (int i = 0; i < std::min(n, 6); i++) {
        const auto& c = candidates[i];
        lg::info("[enhanced]   light at {:.1f} {:.1f} {:.1f} m (view), radius {:.1f} m, color {:.2f} {:.2f} {:.2f}",
                 c.pos[0] / kMeter, c.pos[1] / kMeter, c.pos[2] / kMeter, c.pos[3] / kMeter,
                 c.color[0], c.color[1], c.color[2]);
      }
    }
  }
  {
    // the sun for the glints, only when it's above the horizon
    float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                          m_sun_view[2] * m_sun_view[2]);
    float up = len > 0.f ? (m_sun_view[0] * cam.rot[1][0] + m_sun_view[1] * cam.rot[1][1] +
                            m_sun_view[2] * cam.rot[1][2]) / len
                         : 0.f;
    const bool sun = g_enhanced_sun.valid && len > 0.f && up > 0.f;
    glUniform4f(glGetUniformLocation(id, "sun_dir"), sun ? m_sun_view[0] / len : 0.f,
                sun ? m_sun_view[1] / len : 0.f, sun ? m_sun_view[2] / len : 0.f,
                sun ? m_settings.water_glint * std::min(1.f, up * 4.f) : 0.f);
  }
  m_ocean_ready = false;

  // camera motion blur: view -> previous view is R^T Rp, with the camera's move as an offset
  float strength = 0.f;
  if (reproject && m_settings.motion_blur > 0.f) {
    strength = m_settings.motion_blur;
    glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
    glUniform3f(glGetUniformLocation(id, "to_prev_offset"), offset[0], offset[1], offset[2]);
  }
  glUniform4f(glGetUniformLocation(id, "motion"), m_prev_camera.fx, m_prev_fy, strength, 0.02f);
  glUniform2f(glGetUniformLocation(id, "motion_keep"), 10.f * kMeter, 25.f * kMeter);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindSampler(13, 0);

  m_prev_camera = cam;
  m_prev_fy = cam.fy * kScissorHeightScale;

  // leave the state the way the bucket renderers expect it
  glBindVertexArray(0);
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
}


// once a frame: the area Jak is in and the hour pick the grade and the weather, eased in over a
// few seconds so a level change or nightfall doesn't snap
float EnhancedPost::night_darkness() const {
  if (m_settings.dark_nights <= 0.f || !m_ssao_ready) {
    return 0.f;
  }
  const float zone = m_city ? 0.68f : (m_pond ? 0.9f : 0.78f);
  return m_night * zone * m_settings.dark_nights;
}

void EnhancedPost::update_scene() {
  std::string level;
  float hour = 12.f;
  bool movie = false;
  {
    std::lock_guard<std::mutex> lock(g_enhanced_scene.mutex);
    level = g_enhanced_scene.level;
    hour = g_enhanced_scene.hour;
    movie = g_enhanced_scene.movie;
  }
  static const bool cine_test = std::getenv("JAK_ENHANCED_CINE_TEST") != nullptr;
  movie = movie || cine_test;
  static std::string last_level;
  if (level != last_level) {
    lg::info("[enhanced] scene {}", level);
    last_level = level;
  }
  poll_world();
  // test switch: JAK_ENHANCED_NAP=<hours> asks for a nap 8 s into the first level
  {
    static const char* nap = std::getenv("JAK_ENHANCED_NAP");
    static double first_level_t = -1;
    if (nap && first_level_t < 0 && !level.empty() && level != "none") {
      first_level_t = m_time;
    }
    if (nap && first_level_t >= 0 && m_time - first_level_t > 8.0) {
      // ("window": the nap window opens instead)
      g_enhanced_menu.ask(std::strcmp(nap, "window") == 0 ? 12 : 11, nap);
      nap = nullptr;
    }
  }
  m_zone = zone_of(level);
  m_pond = g_game_version == GameVersion::Jak3 && level.rfind("des", 0) == 0;
  m_city = g_game_version != GameVersion::Jak1 &&
           ((m_zone > 0 && m_zone < (int)g_world.zones.size() &&
             g_world.zones[m_zone].name.rfind("haven", 0) == 0) ||
            level.rfind("was", 0) == 0);
  // every visit to a zone picks its weather anew: the spells start from a random point of their
  // cycle (the first visit of a session too). A district ("haven-industrial") is a visit of its
  // town: walking into it doesn't change the weather
  const std::string& zone_name = g_world.zones[m_zone].name;
  const std::string visit = zone_name.substr(0, zone_name.find('-'));
  if (visit != m_visit_zone && !level.empty() && level != "none") {
    m_visit_zone = visit;
    // (std::rand is never seeded: it would pick the same each run)
    static std::mt19937 rng{std::random_device{}()};
    m_visit_offset = std::uniform_real_distribution<double>(0.0, 100000.0)(rng);
  }
  const WorldZone& zone = g_world.zones[m_zone];
  // night from 19 to 21 in, from 5 to 7 out
  auto ramp = [](float a, float b, float x) { return std::clamp((x - a) / (b - a), 0.f, 1.f); };
  m_night = std::max(ramp(19.f, 21.f, hour), 1.f - ramp(5.f, 7.f, hour));

  const auto& s = m_settings;
  const ZoneLook& day = (s.area_grading ? zone : g_world.zones[0]).look[0];
  const ZoneLook& night = (s.area_grading ? zone : g_world.zones[0]).look[1];
  auto mix = [&](float a, float b) { return a + (b - a) * m_night; };
  Grade want;
  for (int i = 0; i < 3; i++) {
    want.lift[i] = s.area_grading ? mix(day.lift[i], night.lift[i]) : 0.f;
    want.gain[i] = s.area_grading ? mix(day.gain[i], night.gain[i]) : 1.f;
  }
  want.saturation = s.area_grading ? mix(day.saturation, night.saturation) : 1.f;
  want.contrast = s.area_grading ? mix(day.contrast, night.contrast) : 0.f;
  want.warmth = s.area_grading ? mix(day.warmth, night.warmth) : 0.f;
  want.clouds = mix(day.clouds, night.clouds);

  // the season: the computer's calendar (northern hemisphere) or the menu's pick
  {
    int pick = s.season - 1;
    if (pick < 0) {
      const std::time_t now = std::time(nullptr);
      const int month = std::localtime(&now)->tm_mon;  // 0 = January
      pick = month >= 2 && month <= 4 ? 0 : month >= 5 && month <= 7 ? 1 : month >= 8 && month <= 10 ? 2 : 3;
    }
    m_season = pick;
  }
  const Season& season = g_world.seasons[m_season];
  if (s.area_grading && zone.weather.outdoors) {
    want.warmth += season.warmth;
    want.saturation += season.saturation;
  }
  if (g_enhanced_menu.photo_active) {
    const auto& pf = kPhotoFilters[std::clamp(s.photo_filter, 0, kPhotoFilterCount - 1)];
    want.saturation *= pf.saturation;
    want.contrast += pf.contrast;
    want.warmth += pf.warmth;
    for (int i = 0; i < 3; i++) {
      want.gain[i] *= pf.gain[i];
      want.lift[i] += pf.lift[i];
    }
  }

  // the game's own sandstorm, while its process runs (desert-dust-storm.gc)
  auto& gd = g_enhanced_dust;
  const int frame = gd.now.load() + 1;
  gd.now = frame;
  const bool game_storm = frame - gd.frame.load() < 30;
  const float game_dust = game_storm ? std::clamp(gd.intensity.load(), 0.f, 1.f) : 0.f;

  // what the weather wants: rain, dust, thunderstorm, ground fog, the sun out after the rain, and
  // whether the ground is wet
  float rain = 0.f, dust = 0.f, storm = 0.f, mist = 0.f, clear = 0.f, wet = -1.f, sunny = 0.f;
  float snow = 0.f;
  float fog = 1.f;
  // lava levels: the air over it shimmers whatever the weather
  const float heat = zone.weather.heat;
  const bool outdoors = zone.weather.outdoors;
  // a scripted sequence for videos: JAK_ENHANCED_WEATHER_SCRIPT=<s>:<mode>,<s>:<mode>,... (seconds
  // since Jak is first in a level), e.g. "0:0,10:4,40:5": clear, a thunderstorm at 10 s, after the
  // rain at 40 s
  static const std::vector<std::pair<double, int>> script = [] {
    std::vector<std::pair<double, int>> steps;
    if (const char* e = std::getenv("JAK_ENHANCED_WEATHER_SCRIPT")) {
      std::string str(e);
      size_t pos = 0;
      while (pos < str.size()) {
        size_t end = str.find(',', pos);
        std::string item = str.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
          steps.emplace_back(std::atof(item.c_str()),
                             std::clamp(std::atoi(item.c_str() + colon + 1), 0, kWeatherCount - 1));
        }
        if (end == std::string::npos) {
          break;
        }
        pos = end + 1;
      }
    }
    return steps;
  }();
  static double script_t0 = -1;
  if (script_t0 < 0 && !level.empty() && level != "none") {
    script_t0 = m_time;
  }
  int weather_mode = s.weather;
  for (const auto& step : script) {
    if (script_t0 >= 0 && m_time - script_t0 >= step.first) {
      weather_mode = step.second;
    }
  }
  // the title screen's intro (Jak 2, Jak 3: game levels behind the logo) as in the original
  if (level == "title") {
    weather_mode = 0;
  }
  switch (weather_mode) {
    case 1: {
      // changing: a four-minute cycle per area. Rain comes in Haven and the forest (clouds build,
      // rain, a thunderstorm, the rain stops and the sun comes out on the wet streets, they dry),
      // sand blows up in the desert on top of the game's own storms, fog on early mornings
      static const double phase0 = [] {
        const char* e = std::getenv("JAK_ENHANCED_WEATHER_PHASE");
        return e ? std::atof(e) : 0.0;
      }();
      const float ph = (float)std::fmod(m_time + phase0, 240.0);
      auto in = [&](float a, float b) {
        return std::clamp((ph - a) / 12.f, 0.f, 1.f) * std::clamp((b - ph) / 12.f, 0.f, 1.f);
      };
      // the zone's weather (world.json): what's always there, what comes harder for a while
      // in every cycle, spells of fair and wet weather, mist on early mornings
      const auto& w = zone.weather;
      const float harder = w.harder_to > w.harder_from ? in(w.harder_from, w.harder_to) : 0.f;
      rain = w.rain + w.harder_rain * harder;
      mist = w.mist + w.harder_mist * harder;
      snow = w.snow + w.harder_snow * harder;
      dust = w.dust + w.harder_dust * harder;
      if (w.wet >= 0.f) {
        wet = w.wet + w.harder_wet * harder;
      }
      want.clouds = std::max(want.clouds, w.clouds);
      if (w.game_dust) {
        dust = std::max(dust, game_dust * 1.3f);
      }
      m_weather_state = rain > 0.3f ? 2 : snow > 0.f || mist > 0.5f ? 1 : dust > 0.f ? 5 : 0;
      float fair = 1.f;
      if (w.spell_period > 0.f) {
        // each spell its own (and a new one on every visit): a roll picks its weather by the zone's
        // odds times the season's. A wet spell: clouds build, rain (the storm in the middle of it),
        // the sun back out on the wet ground, drying; the others rise and fall over the spell
        const double tt = m_time + phase0 + m_visit_offset;
        const double period = std::max(30.0, (double)w.spell_period);
        const float p = (float)(std::fmod(tt, period) / period);
        const uint32_t index = (uint32_t)(tt / period);
        const uint32_t spell = index * 2654435761u + 977u;
        const float roll = (float)((spell >> 8) % 1000) / 1000.f;
        // how strong this one is (a drizzle or a downpour, a haze or a storm)
        const float strength = (float)(((spell * 2246822519u) >> 12) % 1000) / 1000.f;
        auto win = [&](float a, float b) {
          return std::clamp((p - a) / 0.05f, 0.f, 1.f) * std::clamp((b - p) / 0.05f, 0.f, 1.f);
        };
        std::array<float, kSpellKinds> odds;
        float total = 0.f;
        for (int k = 0; k < kSpellKinds; k++) {
          odds[k] = std::max(0.f, w.spells[k] * season.odds[k] * w.season_odds[m_season][k]);
          total += odds[k];
        }
        int kind = kFair;
        float acc = 0.f;
        for (int k = 0; k < kSpellKinds && total > 0.f; k++) {
          acc += odds[k] / total;
          if (roll < acc) {
            kind = k;
            break;
          }
        }
        m_spell = kind;
        switch (kind) {
          case kShower:
          case kThunder: {
            const bool thunder = kind == kThunder;
            const float amount = thunder ? 1.f : 0.3f + 0.6f * strength;
            rain = std::max(rain, win(0.333f, 0.667f) * amount);
            storm = thunder ? win(0.458f, 0.625f) : 0.f;
            clear = win(0.667f, 0.925f);
            if (p > 0.333f && p < 0.925f) {
              wet = 1.f;
            }
            want.clouds = std::max(want.clouds, 0.95f * win(0.25f, 0.7f));
            fair = 1.f - win(0.25f, 0.7f);
            m_weather_state = p < 0.25f ? 6 : p < 0.333f ? 1 : p < 0.667f ? (storm > 0.f ? 3 : 2)
                            : p < 0.925f ? 4 : 6;
            break;
          }
          case kOvercast:
            want.clouds = std::max(want.clouds, 0.92f * win(0.1f, 0.9f));
            fair = 1.f - win(0.1f, 0.9f);
            m_weather_state = 1;
            break;
          case kFogSpell:
            // a fog bank rolls in and lies there, the ground damp under it
            mist = std::max(mist, (0.55f + 0.45f * strength) * win(0.15f, 0.85f));
            want.clouds = std::max(want.clouds, 0.7f * win(0.15f, 0.85f));
            wet = std::max(wet, 0.3f * win(0.15f, 0.85f));
            fair = 1.f - win(0.15f, 0.85f);
            m_weather_state = 1;
            break;
          case kSandstorm:
            dust = std::max(dust, (0.6f + 0.6f * strength) * win(0.2f, 0.8f));
            m_weather_state = p > 0.2f && p < 0.8f ? 5 : 6;
            break;
          case kSnowSpell:
            snow = std::max(snow, (0.35f + 0.75f * strength) * win(0.2f, 0.85f));
            mist = std::max(mist, 0.3f * win(0.2f, 0.85f));
            want.clouds = std::max(want.clouds, 0.88f * win(0.15f, 0.9f));
            fair = 1.f - win(0.15f, 0.9f);
            m_weather_state = 1;
            break;
          default:
            // fair weather clouds drift over now and then
            want.clouds = std::max(want.clouds, 0.2f + 0.4f * strength * win(0.25f, 0.67f));
            m_weather_state = 6;
            break;
        }
      }
      sunny = std::max(sunny, w.sunny * fair * (1.f - m_night));
      // the fog: fixed, or its own spells (clear, a little, full), each its own
      fog = w.fog;
      if (w.fog_period > 0.f) {
        const double tt = m_time + phase0 + m_visit_offset;
        const double period = std::max(30.0, (double)w.fog_period);
        const uint32_t index = (uint32_t)(tt / period);
        const uint32_t spell = index * 2246822519u + 3266489917u;
        const float roll = (float)((spell >> 8) % 1000) / 1000.f;
        fog = roll < w.fog_clear ? 0.f : roll < w.fog_clear + w.fog_light ? w.fog_light_amount : 1.f;
      }
      // rain and morning mist bring some of it back
      fog = std::max(fog, std::max(rain * 0.7f, mist));
      mist = std::max(mist, w.morning_mist * std::clamp(hour - 4.5f, 0.f, 1.f) *
                                std::clamp((9.5f - hour) / 1.5f, 0.f, 1.f));
      if (!outdoors) {
        rain = storm = clear = mist = dust = sunny = snow = 0.f;
        wet = 0.f;
      }
      break;
    }
    default: {
      // a preset or the custom mix, the same everywhere
      const auto& m = weather_mode == kWeatherCustom
                          ? s.weather_mix
                          : kWeatherPresets[std::clamp(weather_mode, 0, kWeatherCount - 1)].mix;
      rain = m[0];
      storm = m[1];
      fog = m[2];
      mist = m[3];
      snow = m[4];
      dust = m[5];
      want.clouds = std::max(want.clouds * 0.5f, m[6]);
      clear = m[7];
      wet = m[8];
      break;
    }
  }
  sunny = std::max(sunny, s.vivid * (1.f - m_night) * (1.f - std::min(1.f, rain)));
  // rain brings its clouds; after it, broken clouds with the sun through
  want.clouds = std::max(want.clouds, rain * 0.95f);
  want.clouds += (0.55f - want.clouds) * clear;
  if (wet < 0.f) {
    wet = rain;
  }
  // sunny: Jak 1's Sandover. Saturated greens and blues, warm highlights, shadows lifted toward
  // blue (the sky fills them), a few white clouds
  if (sunny > 0.f) {
    const float gain[3] = {1.13f, 1.08f, 0.96f};
    const float lift[3] = {0.012f, 0.02f, 0.045f};
    auto toward = [&](float& v, float t) { v += (t - v) * std::min(sunny, 1.f); };
    for (int i = 0; i < 3; i++) {
      toward(want.gain[i], gain[i]);
      toward(want.lift[i], lift[i]);
    }
    toward(want.saturation, 1.6f);
    toward(want.contrast, 0.08f);
    toward(want.warmth, 0.09f);
    toward(want.clouds, 0.32f);
  }

  const float dt = 1.f / 60.f;
  const float k_grade = 1.f - std::exp(-dt / 1.5f);
  const float k_weather = 1.f - std::exp(-dt / 2.5f);
  auto ease = [](float& v, float target, float k) { v += (target - v) * k; };
  for (int i = 0; i < 3; i++) {
    ease(m_grade.lift[i], want.lift[i], k_grade);
    ease(m_grade.gain[i], want.gain[i], k_grade);
  }
  ease(m_grade.saturation, want.saturation, k_grade);
  ease(m_grade.contrast, want.contrast, k_grade);
  ease(m_grade.warmth, want.warmth, k_grade);
  ease(m_grade.clouds, want.clouds, k_weather);
  ease(m_rain, rain * s.rain_strength, k_weather);
  ease(m_dust, dust * s.dust_strength, k_weather);
  ease(m_storm, storm, k_weather);
  ease(m_mist, mist, k_weather);
  // fog comes and goes slowly (half a minute)
  ease(m_fog, fog, 1.f - std::exp(-dt / 10.f));
  ease(m_clear, clear, k_weather);
  ease(m_sunny, sunny, k_weather);
  ease(m_snow, snow, k_weather);
  ease(m_heat, heat, k_grade);
  // the ground soaks in a few seconds and takes a minute and a half to dry
  ease(m_wet, wet, 1.f - std::exp(-dt / (wet > m_wet ? 5.f : 40.f)));
  // the puddles fill over a minute of rain and dry from their rims in, for minutes after
  ease(m_puddle, wet, 1.f - std::exp(-dt / (wet > m_puddle ? 20.f : 220.f)));
  g_enhanced_merc.wet = std::max(m_rain, m_wet * 0.5f);

  // the ambient sound loops (game/sound/common/enhanced_ambience): rain, light and then heavy, the
  // dripping once it stops while the puddles are still there, the storm's (and sandstorm's) wind,
  // the zone's night wind and crickets (quiet in the rain); turned down in cutscenes
  {
    using namespace snd::ambience;
    const ZoneAmbience& a = zone.ambience;
    const float r = std::clamp(m_rain, 0.f, 1.f);
    const float heavy = std::clamp((r - 0.45f) / 0.45f, 0.f, 1.f);
    float g[kLayers];
    // under a roof: the rain around muffled, drumming on the roof above instead
    ease(m_cover, outdoors ? m_cover_now : 0.f, 1.f - std::exp(-dt / 0.6f));
    const float roofed = 1.f - 0.65f * m_cover;
    g[RainLight] = std::min(1.f, r * 1.6f) * (1.f - 0.5f * heavy) * roofed;
    g[RainHeavy] = std::max(heavy, m_storm * r) * roofed;
    g[RainRoof] = m_cover * std::min(1.f, r * 1.4f);
    g[Drips] = outdoors ? std::clamp(m_puddle * 1.5f - r * 3.f, 0.f, 1.f) : 0.f;
    g[WindNight] = std::max(a.wind, a.wind_night * m_night);
    g[WindStorm] = std::clamp(std::max(m_storm * 0.8f, std::max(m_dust, game_dust) * 1.2f), 0.f, 1.f);
    const float calm = (1.f - std::clamp(r * 3.f, 0.f, 1.f)) * (1.f - std::clamp(m_storm + m_snow, 0.f, 1.f));
    g[Crickets] = a.crickets * season.crickets * m_night * calm;
    g[Birds] = a.birds * season.birds * (1.f - m_night) * calm;
    // frogs: loudest at night and when it's wet, quiet in a downpour
    g[Frogs] = a.frogs * season.frogs * (0.25f + 0.75f * std::max(m_night, std::clamp(m_wet, 0.f, 1.f))) * (1.f - 0.6f * heavy);
    g[Waves] = a.waves;
    g[City] = a.city * (1.f - 0.3f * m_night);
    for (float& v : g) {
      v *= 1.f - 0.6f * m_movie;
    }
    set_targets(g);
    set_volume(s.ambience);
  }

  // the dust's wind: the game storm's when it blows, else a slow drift that wanders
  const float wa = 0.7f + 0.5f * (float)std::sin(m_time * 0.013);
  float wx = std::sin(wa), wz = std::cos(wa);
  float speed = 7.f + 3.f * m_storm;
  if (game_storm) {
    const float gx = gd.wind_x.load(), gz = gd.wind_z.load();
    const float gl = std::sqrt(gx * gx + gz * gz);
    if (gl > 0.01f) {
      wx = gx / gl;
      wz = gz / gl;
    }
    speed = std::max(6.f, gd.wind_vel.load() / 4096.f * 2.5f);
  }
  const float k_wind = 1.f - std::exp(-dt / 2.f);
  ease(m_wind[0], wx, k_wind);
  ease(m_wind[1], wz, k_wind);
  ease(m_wind_speed, speed, k_wind);
  const float wl = std::max(1e-3f, std::sqrt(m_wind[0] * m_wind[0] + m_wind[1] * m_wind[1]));
  // (wrapped now and then: a pop every few hours of the same wind)
  for (int i = 0; i < 2; i++) {
    m_drift[i] = std::fmod(m_drift[i] + m_wind[i] / wl * m_wind_speed * dt, 1e5);
  }

  // lightning: a bolt every few seconds in a thunderstorm, a bright flash that flickers
  if (m_next_bolt == 0) {
    m_next_bolt = m_time + 3.0;
  }
  if (m_storm > 0.3f && m_time >= m_next_bolt) {
    auto rnd = [] { return (float)(std::rand() % 10000) / 10000.f; };
    m_bolt[0] = rnd() * 6.2832f;
    m_bolt[1] = rnd() * 100.f;
    m_bolt[2] = 0.f;
    m_bolt[3] = 0.6f + 0.4f * rnd();
    // a big storm: one every couple of seconds
    m_next_bolt = m_time + 1.5 + 3.5 * rnd() / std::max(0.3f, m_storm);
    // the thunder follows, later the farther the bolt (the brighter, the nearer)
    m_thunder_at = m_time + 0.4 + 3.0 * (1.f - m_bolt[3]) / 0.4f * rnd();
    m_thunder_kind = 1 + std::rand() % 3;
  }
  if (m_thunder_at >= 0 && m_time >= m_thunder_at) {
    m_thunder_at = -1;
    int deg = (int)(m_bolt[0] * 57.2958f) % 360;
    g_enhanced_thunder = m_thunder_kind * 1000 + deg;
  }
  m_bolt[2] += dt;
  // sheet lightning: every few seconds the whole sky flickers from inside the clouds, two or three
  // pulses
  if (m_next_sheet == 0) {
    m_next_sheet = m_time + 2.0;
  }
  if (m_storm > 0.3f && m_time >= m_next_sheet) {
    m_sheet_age = 0.f;
    m_sheet_strength = 0.5f + 0.5f * (float)(std::rand() % 1000) / 1000.f;
    m_next_sheet = m_time + 2.0 + 4.0 * (std::rand() % 1000) / 1000.0;
  }
  m_sheet_age += dt;
  // test: a sheet flash held at the age JAK_ENHANCED_SHEET_TEST says (s; screenshots)
  if (const char* st = std::getenv("JAK_ENHANCED_SHEET_TEST")) {
    m_sheet_age = (float)std::atof(st);
    m_sheet_strength = 1.f;
  }
  {
    const float a = m_sheet_age;
    const float f = a < 0.6f ? std::exp(-a * 10.f) + 0.8f * std::exp(-(a - 0.14f) * (a - 0.14f) * 1500.f) +
                                   0.5f * std::exp(-(a - 0.32f) * (a - 0.32f) * 1200.f)
                             : 0.f;
    m_sheet = std::min(1.f, f) * m_sheet_strength * std::min(1.f, m_storm * 1.5f);
  }
  // test: a bolt held at its brightest (screenshots)
  static const bool bolt_test = std::getenv("JAK_ENHANCED_BOLT_TEST") != nullptr;
  if (bolt_test && m_storm > 0.3f) {
    // straight ahead, at the age JAK_ENHANCED_BOLT_TEST says (s)
    m_bolt[0] = std::atan2(m_prev_camera.rot[0][2], m_prev_camera.rot[2][2]) + 0.25f;
    m_bolt[1] = 7.f;
    m_bolt[2] = (float)std::atof(std::getenv("JAK_ENHANCED_BOLT_TEST"));
    m_bolt[3] = 1.f;
  }
  {
    // as the bolt in the shader: a faint leader, the return stroke, two restrikes
    const float a = m_bolt[2];
    const float t = a - 0.09f;
    const float f = a < 0.09f ? 0.1f
                    : a < 0.42f ? std::exp(-t * 25.f) + 0.7f * std::exp(-(t - 0.08f) * (t - 0.08f) * 3600.f) +
                                      0.45f * std::exp(-(t - 0.18f) * (t - 0.18f) * 3600.f)
                                : 0.f;
    m_flash = std::min(1.f, f * m_bolt[3]) * std::min(1.f, m_storm * 1.5f);
  }
  ease(m_cine, movie && s.cine_dof ? 1.f : 0.f, 1.f - std::exp(-dt / 0.4f));
  ease(m_movie, movie ? 1.f : 0.f, 1.f - std::exp(-dt / 0.4f));
}

// cutscene autofocus: nine depth samples around the middle of the screen go into a pixel buffer;
// the ones from the frame before are read (no stall) and the focus eases toward the nearest
// thing near the middle (who's talking, usually)
void EnhancedPost::read_focus(int w, int h) {
  if (!m_focus_pbo[0]) {
    glGenBuffers(2, m_focus_pbo);
    for (int i = 0; i < 2; i++) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, m_focus_pbo[i]);
      glBufferData(GL_PIXEL_PACK_BUFFER, 9 * sizeof(float), nullptr, GL_STREAM_READ);
    }
  }
  const int prev = 1 - m_focus_cur;
  if (m_focus_pending[prev]) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_focus_pbo[prev]);
    if (auto* d = (const float*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 9 * sizeof(float),
                                                 GL_MAP_READ_BIT)) {
      float nearest = 1e12f;
      for (int i = 0; i < 9; i++) {
        // sky (0) is endless; nothing under half a meter (the camera brushing a wall)
        float z = d[i] > 1e-7f ? m_last_proj[1] / (d[i] * 2.f - 1.f - m_last_proj[0]) : 1e12f;
        z = std::abs(z);
        if (z > 0.5f * kMeter) {
          // the middle counts a little more
          nearest = std::min(nearest, i == 4 ? z * 0.8f : z);
        }
      }
      nearest = std::clamp(nearest, 0.5f * kMeter, 400.f * kMeter);
      m_focus = m_focus <= 0.f ? nearest
                               : std::exp(std::log(m_focus) +
                                          (std::log(nearest) - std::log(m_focus)) * 0.1f);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    m_focus_pending[prev] = false;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_depth_fbo);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, m_focus_pbo[m_focus_cur]);
  int i = 0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      glReadPixels(w / 2 + x * w / 12, h / 2 + y * h / 10, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT,
                   (void*)(i++ * sizeof(float)));
    }
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  m_focus_pending[m_focus_cur] = true;
  m_focus_cur = prev;
}

void EnhancedPost::do_shafts(GLuint scene_tex,
                             GLuint screen_vao,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  // the sun on screen: in view space the camera looks down +z (see enh_ssao.frag)
  float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                        m_sun_view[2] * m_sun_view[2]);
  if (m_settings.ssao_debug) {
    static int behind_counter = 0;
    if ((behind_counter++ % 300) == 0) {
      lg::info("[enhanced] sun view {} {} {}", m_sun_view[0], m_sun_view[1], m_sun_view[2]);
    }
  }
  if (len < 1e-6f || m_sun_view[2] / len < 0.05f) {
    return;  // behind the camera
  }
  float sx = 0.5f * m_last_proj[2] * m_sun_view[0] / m_sun_view[2] + 0.5f;
  float sy = 0.5f * m_last_proj[3] * m_sun_view[1] / m_sun_view[2] + 0.5f;
  // fade out as the sun leaves the screen
  float off = std::max(std::max(-sx, sx - 1.f), std::max(-sy, sy - 1.f));
  float fade = 1.f - std::clamp(off / 0.6f, 0.f, 1.f);
  static int debug_counter = 0;
  if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
    lg::info("[enhanced] sun on screen {} {} fade {}", sx, sy, fade);
  }
  if (fade <= 0.f) {
    return;
  }

  int w = std::max(1, m_ssao_w / 2);
  int h = std::max(1, m_ssao_h / 2);
  if (w != m_shaft_w || h != m_shaft_h) {
    if (m_shaft_fbo) {
      glDeleteFramebuffers(1, &m_shaft_fbo);
      glDeleteTextures(1, &m_shaft_tex);
    }
    m_shaft_w = w;
    m_shaft_h = h;
    glGenFramebuffers(1, &m_shaft_fbo);
    glGenTextures(1, &m_shaft_tex);
    glBindTexture(GL_TEXTURE_2D, m_shaft_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shaft_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_shaft_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_shaft_fbo);
  glViewport(0, 0, w, h);
  auto& shader = render_state->shaders[ShaderId::ENH_SHAFTS];
  shader.activate();
  auto id = shader.id();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform2f(glGetUniformLocation(id, "sun_uv"), sx, sy);
  glUniform1f(glGetUniformLocation(id, "threshold"), m_settings.shafts_threshold);
  glUniform1f(glGetUniformLocation(id, "fade"), fade);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindVertexArray(0);
  m_shafts_ready = true;
}

void EnhancedPost::do_gi(GLuint scene_tex,
                         GLuint screen_vao,
                         SharedRenderState* render_state,
                         ScopedProfilerNode& prof) {
  int w = std::max(1, m_ssao_w / 2);
  int h = std::max(1, m_ssao_h / 2);
  if (w != m_gi_w || h != m_gi_h) {
    if (m_gi_fbo[0]) {
      glDeleteFramebuffers(2, m_gi_fbo);
      glDeleteTextures(2, m_gi_tex);
    }
    m_gi_w = w;
    m_gi_h = h;
    glGenFramebuffers(2, m_gi_fbo);
    glGenTextures(2, m_gi_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_gi_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gi_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glViewport(0, 0, w, h);

  auto& gi = render_state->shaders[ShaderId::ENH_GI];
  gi.activate();
  auto id = gi.id();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, m_normals_ready ? m_normal_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T11"), 11);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &g_enhanced_camera.rot[0][0]);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / m_ssao_w, 1.f / m_ssao_h);
  glUniform1f(glGetUniformLocation(id, "radius"), m_settings.gi_radius * kMeter);
  glUniform1f(glGetUniformLocation(id, "max_distance"), 150.f * kMeter);
  glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[0]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();

  auto& blur = render_state->shaders[ShaderId::ENH_GI_BLUR];
  blur.activate();
  id = blur.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  for (int pass = 0; pass < 2; pass++) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[1 - pass]);
    glBindTexture(GL_TEXTURE_2D, m_gi_tex[pass]);
    glUniform2f(glGetUniformLocation(id, "step"), pass == 0 ? 1.f / w : 0.f,
                pass == 0 ? 0.f : 1.f / h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
  }
  glBindVertexArray(0);
  m_gi_ready = true;
}

namespace {
// texture alpha under this doesn't cast a shadow (leaf cards, cutouts)
constexpr float kShadowAlphaRef = 0.25f;
}  // namespace

void EnhancedPost::do_shadow_map(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_shadow_ready = false;
  float sun[3];
  sun_world(sun);
  const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  const auto& cam = g_enhanced_camera;
  if (!g_enhanced_sun.valid || len < 1e-6f || sun[1] / len < 0.05f || !cam.valid) {
    return;  // no sun, or below the horizon
  }

  constexpr int kRes = 4096;
  if (m_shadow_res != kRes) {
    m_shadow_res = kRes;
    glGenFramebuffers(1, &m_shadow_fbo);
    glGenTextures(1, &m_shadow_tex);
    glBindTexture(GL_TEXTURE_2D, m_shadow_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kRes, kRes, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // hardware comparison: sampler2DShadow, 2x2 filtered
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadow_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenVertexArrays(1, &m_shadow_vao);
  }

  // what to draw: geometry 0 (the most detailed) of every tfrag and tie tree of the levels in use;
  // tfrag keeps its indices on the CPU (they're streamed after culling), so they get a buffer here
  auto levels = render_state->loader->get_in_use_levels();
  std::vector<u64> in_use;
  for (auto* lev : levels) {
    in_use.push_back(lev->load_id);
    if (m_shadow_draws.count(lev->load_id)) {
      continue;
    }
    auto& draws = m_shadow_draws[lev->load_id];
    const auto& name = lev->level->level_name;
    const bool cut = (name == "mhcitya" || name == "ctyport") && enhanced_has_piazza();
    const auto& tfrags = lev->level->tfrag_trees[0];
    for (size_t i = 0; i < tfrags.size() && i < lev->tfrag_vertex_data[0].size(); i++) {
      const auto& tree = tfrags[i];
      if (tree.kind == tfrag3::TFragmentTreeKind::WATER ||
          tree.kind == tfrag3::TFragmentTreeKind::LOWRES ||
          tree.kind == tfrag3::TFragmentTreeKind::LOWRES_TRANS || tree.unpacked.indices.empty()) {
        continue;
      }
      ShadowDraw d;
      d.vertices = lev->tfrag_vertex_data[0][i];
      glGenBuffers(1, &d.indices);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.unpacked.indices.size() * sizeof(u32),
                   tree.unpacked.indices.data(), GL_STATIC_DRAW);
      d.own_indices = true;
      d.count = tree.unpacked.indices.size();
      d.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      draws.push_back(d);
    }
    const auto& ties = lev->level->tie_trees[0];
    for (size_t i = 0; i < ties.size() && i < lev->tie_data[0].size(); i++) {
      if (ties[i].unpacked.indices.empty()) {
        continue;
      }
      ShadowDraw d;
      d.vertices = lev->tie_data[0][i].vertex_buffer;
      d.indices = lev->tie_data[0][i].index_buffer;
      d.count = ties[i].unpacked.indices.size();
      d.mode = ties[i].use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      draws.push_back(d);
    }
    add_wind_shadow_draws(lev, cut, &draws);
  }
  for (auto it = m_shadow_draws.begin(); it != m_shadow_draws.end();) {
    if (std::find(in_use.begin(), in_use.end(), it->first) == in_use.end()) {
      for (auto& d : it->second) {
        if (d.own_indices) {
          glDeleteBuffers(1, &d.indices);
        }
        if (d.own_vertices) {
          glDeleteBuffers(1, &d.vertices);
        }
      }
      it = m_shadow_draws.erase(it);
    } else {
      ++it;
    }
  }

  // the light's frame: f from the sun down, right and up across; a square of 2 * range around a
  // point ahead of the camera, snapped to whole texels so the edges don't crawl as it moves
  float f[3] = {-sun[0] / len, -sun[1] / len, -sun[2] / len};
  float hint[3] = {0.f, 0.f, 1.f};
  if (std::abs(f[2]) > 0.9f) {
    hint[0] = 1.f;
    hint[2] = 0.f;
  }
  float right[3] = {hint[1] * f[2] - hint[2] * f[1], hint[2] * f[0] - hint[0] * f[2],
                    hint[0] * f[1] - hint[1] * f[0]};
  float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  for (auto& r : right) {
    r /= rl;
  }
  float up[3] = {f[1] * right[2] - f[2] * right[1], f[2] * right[0] - f[0] * right[2],
                 f[0] * right[1] - f[1] * right[0]};
  const float range = m_settings.shadow_range * kMeter;
  const float depth = 300.f * kMeter;
  float center[3];
  for (int i = 0; i < 3; i++) {
    // the camera looks down view +z: its forward in world is column 2 of the rotation
    center[i] = cam.trans[i] + cam.rot[i][2] * range * 0.5f;
  }
  const float texel = 2.f * range / kRes;
  float cr = right[0] * center[0] + right[1] * center[1] + right[2] * center[2];
  float cu = up[0] * center[0] + up[1] * center[1] + up[2] * center[2];
  float cf = f[0] * center[0] + f[1] * center[1] + f[2] * center[2];
  cr = std::floor(cr / texel) * texel;
  cu = std::floor(cu / texel) * texel;
  const float rows[3][4] = {{right[0] / range, right[1] / range, right[2] / range, -cr / range},
                            {up[0] / range, up[1] / range, up[2] / range, -cu / range},
                            {f[0] / depth, f[1] / depth, f[2] / depth, -cf / depth}};
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 4; c++) {
      m_light_mvp[r * 4 + c] = rows[r][c];
    }
  }
  m_light_mvp[12] = m_light_mvp[13] = m_light_mvp[14] = 0.f;
  m_light_mvp[15] = 1.f;

  GLfloat old_clear_depth = 0.f;
  GLint old_depth_func = GL_GEQUAL;
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
  glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
  glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_fbo);
  glViewport(0, 0, kRes, kRes);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5f, 3.f);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  auto& shader = render_state->shaders[ShaderId::ENH_SHADOW];
  shader.activate();
  auto id = shader.id();
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  GLint u_cut = glGetUniformLocation(id, "enh_cut");
  GLint u_alpha = glGetUniformLocation(id, "alpha_ref");
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(m_shadow_vao);
  glEnableVertexAttribArray(0);
  for (auto& [lid, draws] : m_shadow_draws) {
    for (auto& d : draws) {
      if (d.wind && !m_settings.shadow_casters) {
        continue;
      }
      glUniform1i(u_cut, d.cut);
      glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
      if (d.own_vertices || d.wind) {
        // x, y, z, s, t
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
      } else {
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex), (void*)0);
        glDisableVertexAttribArray(1);
      }
      glUniform1f(u_alpha, d.tex ? kShadowAlphaRef : -1.f);
      if (d.tex) {
        glBindTexture(GL_TEXTURE_2D, d.tex);
      }
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      glDrawElements(d.mode, d.count, GL_UNSIGNED_INT, (void*)(d.first * sizeof(u32)));
      prof.add_draw_call();
    }
  }
  glDisableVertexAttribArray(1);
  glBindVertexArray(0);
  // characters only with the sun (or moon) above ~20 degrees: lower, a 2 m character casts a band
  // tens of meters long that follows it around (the game's own short shadow stays)
  if (m_settings.shadow_casters && sun[1] / len > 0.34f) {
    draw_shadow_casters(render_state, prof);
  }
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_PRIMITIVE_RESTART);
  glClearDepth(old_clear_depth);
  glDepthFunc(old_depth_func);
  m_shadow_ready = true;
}

// The wind-blown ties (trees) of a level for the shadow map: the game draws them per instance with
// a matrix it sways each frame; the shadow uses the still matrices, baked into world positions once.
void EnhancedPost::add_wind_shadow_draws(const LevelData* lev,
                                         bool cut,
                                         std::vector<ShadowDraw>* draws) {
  const auto& ties = lev->level->tie_trees[0];
  size_t instances = 0;
  for (const auto& tree : ties) {
    instances += tree.wind_instance_info.size();
  }
  if (instances) {
    lg::info("[enhanced] shadow map {}: {} wind-blown tie instances", lev->level->level_name,
             instances);
  }
  for (const auto& tree : ties) {
    if (tree.instanced_wind_draws.empty()) {
      continue;
    }
    std::vector<float> verts;  // x, y, z, s, t
    std::vector<u32> indices;
    std::vector<ShadowDraw> tree_draws;
    for (const auto& draw : tree.instanced_wind_draws) {
      if (!draw.mode.get_depth_write_enable()) {
        continue;
      }
      ShadowDraw d;
      d.first = indices.size();
      d.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      d.wind = true;
      if (draw.mode.get_at_enable() && draw.tree_tex_id >= 0 &&
          draw.tree_tex_id < (int)lev->textures.size()) {
        d.tex = lev->textures[draw.tree_tex_id];
      }
      u32 off = 0;
      for (const auto& grp : draw.instance_groups) {
        const auto& m = tree.wind_instance_info.at(grp.instance_idx).matrix;
        for (u32 k = off; k < off + grp.num && k < draw.vertex_index_stream.size(); k++) {
          u32 vi = draw.vertex_index_stream[k];
          if (vi == UINT32_MAX || vi >= tree.unpacked.vertices.size()) {
            indices.push_back(UINT32_MAX);
            continue;
          }
          const auto& v = tree.unpacked.vertices[vi];
          for (int c = 0; c < 3; c++) {
            verts.push_back(m[0][c] * v.x + m[1][c] * v.y + m[2][c] * v.z + m[3][c]);
          }
          verts.push_back(v.s);
          verts.push_back(v.t);
          indices.push_back(verts.size() / 5 - 1);
        }
        // strips of different instances must not join
        indices.push_back(UINT32_MAX);
        off += grp.num;
      }
      d.count = indices.size() - d.first;
      if (d.count) {
        tree_draws.push_back(d);
      }
    }
    if (tree_draws.empty()) {
      continue;
    }
    GLuint vb = 0, ib = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ib);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(u32), indices.data(),
                 GL_STATIC_DRAW);
    for (auto& d : tree_draws) {
      d.vertices = vb;
      d.indices = ib;
    }
    // the first one owns the buffers
    tree_draws[0].own_vertices = true;
    tree_draws[0].own_indices = true;
    draws->insert(draws->end(), tree_draws.begin(), tree_draws.end());
  }
}

// The characters drawn this frame (see EnhancedCasters), skinned again with their bones and drawn
// into the shadow map. Their bones put them in camera space: back to the world with the camera.
void EnhancedPost::draw_shadow_casters(SharedRenderState* render_state,
                                       ScopedProfilerNode& prof) {
  auto& cs = g_enhanced_casters;
  cs.capture = false;  // later merc draws (HUD) don't cast
  if (cs.draws.empty()) {
    return;
  }
  // a bound range always spans 128 bones: room past the last one
  constexpr size_t kBoneBlock = 128 * 8 * 4;  // floats
  const size_t size = (cs.bones.size() + kBoneBlock) * sizeof(float);
  if (!m_caster_bones) {
    glGenBuffers(1, &m_caster_bones);
    glGenVertexArrays(1, &m_caster_vao);
  }
  glBindBuffer(GL_UNIFORM_BUFFER, m_caster_bones);
  if (size > m_caster_bones_size) {
    m_caster_bones_size = size * 3 / 2;
    glBufferData(GL_UNIFORM_BUFFER, m_caster_bones_size, nullptr, GL_DYNAMIC_DRAW);
  }
  glBufferSubData(GL_UNIFORM_BUFFER, 0, cs.bones.size() * sizeof(float), cs.bones.data());
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  auto& shader = render_state->shaders[ShaderId::ENH_SHADOW_MERC];
  shader.activate();
  const auto id = shader.id();
  const auto& cam = g_enhanced_camera;
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  // world = cam - rot * merc camera space (the view space negated; rot as stored, see EnhancedCamera)
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform3f(glGetUniformLocation(id, "cam_trans"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  GLint u_alpha = glGetUniformLocation(id, "alpha_ref");
  glBindVertexArray(m_caster_vao);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(5);
  GLuint last_vertices = 0;
  for (const auto& d : cs.draws) {
    if (d.vertices != last_vertices) {
      glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
      constexpr GLsizei kStride = sizeof(tfrag3::MercVertex);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, pos));
      glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, weights[0]));
      glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, st[0]));
      glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, kStride,
                             (void*)offsetof(tfrag3::MercVertex, mats[0]));
      last_vertices = d.vertices;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
    glUniform1f(u_alpha, d.tex ? kShadowAlphaRef : -1.f);
    if (d.tex) {
      glBindTexture(GL_TEXTURE_2D, d.tex);
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_caster_bones, d.first_bone * 4 * sizeof(float),
                      kBoneBlock * sizeof(float));
    glDrawElements(d.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES, d.index_count, GL_UNSIGNED_INT,
                   (void*)(sizeof(u32) * d.first_index));
    prof.add_draw_call();
  }
  glBindVertexArray(0);
}

void EnhancedPost::do_volume(SharedRenderState* render_state,
                             GLuint screen_vao,
                             ScopedProfilerNode& prof) {
  int w = std::max(1, m_ssao_w / 2);
  int h = std::max(1, m_ssao_h / 2);
  if (w != m_vol_w || h != m_vol_h) {
    if (m_vol_fbo) {
      glDeleteFramebuffers(1, &m_vol_fbo);
      glDeleteTextures(1, &m_vol_tex);
    }
    m_vol_w = w;
    m_vol_h = h;
    glGenFramebuffers(1, &m_vol_fbo);
    glGenTextures(1, &m_vol_tex);
    glBindTexture(GL_TEXTURE_2D, m_vol_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_vol_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_vol_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  const auto& cam = g_enhanced_camera;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glBindFramebuffer(GL_FRAMEBUFFER, m_vol_fbo);
  glViewport(0, 0, w, h);
  auto& shader = render_state->shaders[ShaderId::ENH_VOLUME];
  shader.activate();
  auto id = shader.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  // the near sea isn't in the scene depth: the mist stops at its surface, not on the sea bed
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D,
                m_ocean_ready && m_settings.water_reflections ? m_ocean_depth[1] : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  // view -> world is the transpose of world -> view
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
  glUniform3f(glGetUniformLocation(id, "up_view"), cam.rot[1][0], cam.rot[1][1], cam.rot[1][2]);
  {
    float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                          m_sun_view[2] * m_sun_view[2]);
    float up = len > 0.f ? (m_sun_view[0] * cam.rot[1][0] + m_sun_view[1] * cam.rot[1][1] +
                            m_sun_view[2] * cam.rot[1][2]) / len
                         : 0.f;
    const bool sun = g_enhanced_sun.valid && len > 0.f && up > 0.f;
    static int debug_counter = 0;
    if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
      lg::info("[enhanced] mist sun valid {} up {} view {} {} {}", g_enhanced_sun.valid, up,
               m_sun_view[0], m_sun_view[1], m_sun_view[2]);
    }
    glUniform4f(glGetUniformLocation(id, "sun_dir"), sun ? m_sun_view[0] / len : 0.f,
                sun ? m_sun_view[1] / len : 0.f, sun ? m_sun_view[2] / len : 0.f,
                sun ? std::min(1.f, up * 4.f) : 0.f);
  }
  glUniform3f(glGetUniformLocation(id, "fog_color"), render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f);
  glUniform4f(glGetUniformLocation(id, "mist"), m_settings.mist_density * m_fog, m_settings.mist_height,
              m_settings.mist_distance, (float)m_time);
  glUniform4f(glGetUniformLocation(id, "mist_shape"), m_settings.mist_ground,
              m_settings.mist_noise, m_settings.mist_sun, m_settings.mist_aniso);
  glUniform1f(glGetUniformLocation(id, "frame"), (float)(m_frame % 64));
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready ? m_shadow_tex : m_no_shadow_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "shadow_on"), m_shadow_ready);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindVertexArray(0);
  m_vol_ready = true;
}

void EnhancedPost::free_ssao() {
  if (m_normal_fbo) {
    glDeleteFramebuffers(1, &m_normal_fbo);
    glDeleteTextures(1, &m_normal_tex);
    m_normal_fbo = m_normal_tex = 0;
  }
  if (m_depth_fbo) {
    glDeleteFramebuffers(1, &m_depth_fbo);
    glDeleteTextures(1, &m_depth_tex);
    glDeleteFramebuffers(2, m_ao_fbo);
    glDeleteTextures(2, m_ao_tex);
  }
  m_depth_fbo = m_depth_tex = 0;
  m_ao_fbo[0] = m_ao_fbo[1] = m_ao_tex[0] = m_ao_tex[1] = 0;
  m_ssao_w = m_ssao_h = 0;
}

void EnhancedPost::do_ssao(GLuint src_fbo,
                           int w,
                           int h,
                           GLuint screen_vao,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  m_ssao_ready = false;
  static int debug_counter = 0;
  if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
    lg::info("[enhanced] ssao camera valid {} a {} b {} fx {} fy {}", g_enhanced_camera.valid,
             g_enhanced_camera.depth_a, g_enhanced_camera.depth_b, g_enhanced_camera.fx,
             g_enhanced_camera.fy);
  }
  if (!g_enhanced_camera.valid || std::abs(g_enhanced_camera.depth_b) < 1e-12f) {
    return;  // no 3D scene drawn this frame (menus, loading)
  }

  if (w != m_ssao_w || h != m_ssao_h) {
    free_ssao();
    m_ssao_w = w;
    m_ssao_h = h;
    glGenFramebuffers(1, &m_depth_fbo);
    glGenTextures(1, &m_depth_tex);
    glBindTexture(GL_TEXTURE_2D, m_depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depth_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depth_tex,
                           0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    glGenFramebuffers(1, &m_normal_fbo);
    glGenTextures(1, &m_normal_tex);
    glBindTexture(GL_TEXTURE_2D, m_normal_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_normal_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_normal_tex, 0);

    glGenFramebuffers(2, m_ao_fbo);
    glGenTextures(2, m_ao_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_ao_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ao_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // copy (and for msaa, resolve) the depth buffer into a texture we can sample
  glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_depth_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                    GL_NEAREST);
  // and the level normals (msaa: averaged, renormalized in the shaders)
  m_normals_ready = false;
  if (g_enhanced_normals.enabled && src_fbo == g_enhanced_normals.fbo) {
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_normal_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    m_normals_ready = true;
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glViewport(0, 0, w, h);

  const auto& cam = g_enhanced_camera;
  const float fy = cam.fy * kScissorHeightScale;
  m_last_proj = {cam.depth_a, cam.depth_b, cam.fx, fy};

  // occlusion
  auto& ssao = render_state->shaders[ShaderId::ENH_SSAO];
  ssao.activate();
  auto id = ssao.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, m_normals_ready ? m_normal_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T11"), 11);
  glUniformMatrix3fv(glGetUniformLocation(id, "view_to_world"), 1, GL_TRUE, &cam.rot[0][0]);
  glUniform4f(glGetUniformLocation(id, "proj"), cam.depth_a, cam.depth_b, cam.fx, fy);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / w, 1.f / h);
  glUniform1f(glGetUniformLocation(id, "radius"), m_settings.ssao_radius * kMeter);
  glUniform1f(glGetUniformLocation(id, "intensity"), m_settings.ssao_intensity);
  glUniform1f(glGetUniformLocation(id, "max_distance"), m_settings.ssao_max_distance * kMeter);
  {
    // sun direction into the view space the shader rebuilds from depth
    float world_sun[3];
    sun_world(world_sun);
    float sun[3] = {0, 0, 0};
    for (int col = 0; col < 3; col++) {
      for (int row = 0; row < 3; row++) {
        sun[col] += world_sun[row] * cam.rot[row][col];
      }
    }
    // test switch: JAK_ENHANCED_SUN_FLIP=1 puts the sun on the other side (god rays, shadows)
    static const bool flip = [] {
      const char* env = std::getenv("JAK_ENHANCED_SUN_FLIP");
      return env && std::strcmp(env, "1") == 0;
    }();
    for (int i = 0; i < 3; i++) {
      if (flip) {
        sun[i] = -sun[i];
      }
      m_sun_view[i] = sun[i];
    }
    // kept with the shadow map: characters aren't in the map, these give them their shadows
    const bool on = m_settings.sun_shadows && g_enhanced_sun.valid;
    glUniform4f(glGetUniformLocation(id, "sun"), sun[0], sun[1], sun[2],
                on ? m_settings.sun_shadow_strength : 0.f);
    glUniform1f(glGetUniformLocation(id, "sun_length"), m_settings.sun_shadow_length * kMeter);
  }
  glUniform1f(glGetUniformLocation(id, "close_up"), m_movie * kCloseUp);
  // traced occlusion
  const bool rt_ao = m_settings.rt_ao && m_rt_ready;
  glUniform1i(glGetUniformLocation(id, "rt_levels"), rt_ao ? m_rt_count : 0);
  if (rt_ao) {
    for (int i = 0; i < 2; i++) {
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_rt_buf[i]);
    }
    glUniform4iv(glGetUniformLocation(id, "rt_base"), m_rt_count, m_rt_base.data());
    glUniform3f(glGetUniformLocation(id, "cam_pos"), cam.trans[0], cam.trans[1], cam.trans[2]);
    glUniform3f(glGetUniformLocation(id, "rt_ao"), m_settings.rt_ao_length * kMeter,
                m_settings.rt_ao_strength, m_settings.rt_ao_distance * kMeter);
  }
  if (m_settings.ssao_debug && (debug_counter % 300) == 1) {
    lg::info("[enhanced] rt ao {} ready {} levels {}", m_settings.rt_ao, m_rt_ready, m_rt_count);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[0]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();

  // depth-aware blur, horizontal then vertical; the result ends up back in m_ao_tex[0]
  auto& blur = render_state->shaders[ShaderId::ENH_SSAO_BLUR];
  blur.activate();
  id = blur.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform4f(glGetUniformLocation(id, "proj"), cam.depth_a, cam.depth_b, cam.fx, fy);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  for (int pass = 0; pass < 2; pass++) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ao_fbo[1 - pass]);
    glBindTexture(GL_TEXTURE_2D, m_ao_tex[pass]);
    glUniform2f(glGetUniformLocation(id, "step"), pass == 0 ? 1.f / w : 0.f,
                pass == 0 ? 0.f : 1.f / h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
  }
  m_ssao_ready = true;
  glBindVertexArray(0);
}

void EnhancedPost::free_levels() {
  for (auto& level : m_levels) {
    if (level.fbo) {
      glDeleteFramebuffers(1, &level.fbo);
      glDeleteTextures(1, &level.tex);
    }
    level = Level();
  }
  m_w = 0;
  m_h = 0;
}

void EnhancedPost::resize(int w, int h) {
  if (w == m_w && h == m_h) {
    return;
  }
  free_levels();
  m_w = w;
  m_h = h;
  int lw = w;
  int lh = h;
  for (auto& level : m_levels) {
    lw = std::max(1, lw / 2);
    lh = std::max(1, lh / 2);
    level.w = lw;
    level.h = lh;
    glGenFramebuffers(1, &level.fbo);
    glGenTextures(1, &level.tex);
    glBindTexture(GL_TEXTURE_2D, level.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, lw, lh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, level.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, level.tex, 0);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void EnhancedPost::prepare(GLuint scene_tex,
                           GLuint scene_fbo,
                           int w,
                           int h,
                           GLuint screen_vao,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  if (!m_settings.enabled) {
    return;
  }
  m_ssao_ready = false;
  m_shafts_ready = false;
  m_gi_ready = false;
  m_vol_ready = false;
  m_shadow_ready = false;
  if (m_settings.shadow_map && g_enhanced_camera.valid) {
    do_shadow_map(render_state, prof);
  }
  // the levels' ray tracing hierarchies (occlusion, reflections)
  update_rt(render_state);
  if (m_settings.ssao) {
    do_ssao(scene_fbo, w, h, screen_vao, render_state, prof);
    if (m_settings.volume && m_ssao_ready && m_fog > 0.01f) {
      do_volume(render_state, screen_vao, prof);
    }
    if (m_settings.gi && m_ssao_ready) {
      do_gi(scene_tex, screen_vao, render_state, prof);
    }
    if (m_settings.shafts && m_ssao_ready && g_enhanced_sun.valid) {
      do_shafts(scene_tex, screen_vao, render_state, prof);
    }
  }
  // set again by the next frame's background draws
  g_enhanced_camera.valid = false;
  if (!m_settings.bloom) {
    return;
  }
  resize(w, h);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(screen_vao);
  glActiveTexture(GL_TEXTURE0);

  // down: bright pass into level 0, then 13-tap downsample through the chain
  auto& down = render_state->shaders[ShaderId::ENH_BLOOM_DOWN];
  down.activate();
  GLint u_texel = glGetUniformLocation(down.id(), "texel");
  GLint u_threshold = glGetUniformLocation(down.id(), "threshold");
  GLint u_knee = glGetUniformLocation(down.id(), "knee");
  GLint u_first = glGetUniformLocation(down.id(), "first_pass");
  glUniform1i(glGetUniformLocation(down.id(), "tex_T0"), 0);
  glUniform1f(u_threshold, m_settings.bloom_threshold);
  glUniform1f(u_knee, m_settings.bloom_knee);

  GLuint src = scene_tex;
  int src_w = w;
  int src_h = h;
  for (int i = 0; i < kLevels; i++) {
    auto& level = m_levels[i];
    glBindFramebuffer(GL_FRAMEBUFFER, level.fbo);
    glViewport(0, 0, level.w, level.h);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform2f(u_texel, 1.f / src_w, 1.f / src_h);
    glUniform1i(u_first, i == 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
    src = level.tex;
    src_w = level.w;
    src_h = level.h;
  }

  // up: tent-filter each level and add it onto the next bigger one
  auto& up = render_state->shaders[ShaderId::ENH_BLOOM_UP];
  up.activate();
  glUniform1i(glGetUniformLocation(up.id(), "tex_T0"), 0);
  GLint u_up_texel = glGetUniformLocation(up.id(), "texel");
  glUniform1f(glGetUniformLocation(up.id(), "radius"), m_settings.bloom_radius);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glBlendEquation(GL_FUNC_ADD);
  for (int i = kLevels - 1; i > 0; i--) {
    auto& from = m_levels[i];
    auto& to = m_levels[i - 1];
    glBindFramebuffer(GL_FRAMEBUFFER, to.fbo);
    glViewport(0, 0, to.w, to.h);
    glBindTexture(GL_TEXTURE_2D, from.tex);
    glUniform2f(u_up_texel, 1.f / from.w, 1.f / from.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    prof.add_draw_call();
  }
  glDisable(GL_BLEND);
  glBindVertexArray(0);
}

// the sky hidden above Jak: five rays from his head, straight up and tilted 25 degrees four ways,
// through the levels' hierarchies (tfrag and tie: roofs, awnings, arches, trees), up to 40 m
float EnhancedPost::trace_cover(const float pos[3]) const {
  constexpr float kTilt = 0.4226f, kUp = 0.9063f;  // sin, cos of 25 degrees
  const float dirs[5][3] = {{0, 1, 0}, {kTilt, kUp, 0}, {-kTilt, kUp, 0}, {0, kUp, kTilt}, {0, kUp, -kTilt}};
  const float o[3] = {pos[0], pos[1] + 1.8f * 4096.f, pos[2]};
  const float tmax = 40.f * 4096.f;
  int hits = 0;
  for (const auto& d : dirs) {
    float inv[3];
    for (int k = 0; k < 3; k++) {
      inv[k] = 1.f / (std::abs(d[k]) < 1e-7f ? 1e-7f : d[k]);
    }
    bool hit = false;
    for (const auto& [id, level] : m_rt_levels) {
      const EnhancedRtLevel* rt = level.cpu.get();
      if (!rt || rt->nodes.empty() || hit) {
        continue;
      }
      u32 stack[64];
      int sp = 0;
      stack[sp++] = 0;
      while (sp > 0 && !hit) {
        const auto& n = rt->nodes[stack[--sp]];
        const u32 ni = (u32)(&n - rt->nodes.data());
        float tn = 0.f, tf = tmax;
        for (int k = 0; k < 3; k++) {
          float t0 = (n.min[k] - o[k]) * inv[k], t1 = (n.max[k] - o[k]) * inv[k];
          tn = std::max(tn, std::min(t0, t1));
          tf = std::min(tf, std::max(t0, t1));
        }
        if (tn > tf) {
          continue;
        }
        if (n.b > 0) {
          for (u32 i = n.a; i < n.a + n.b && !hit; i++) {
            const auto& tri = rt->tris[i];
            const float* e1 = tri.e1;
            const float* e2 = tri.e2;
            const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                                 d[0] * e2[1] - d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::abs(det) < 1e-4f) {
              continue;
            }
            const float id = 1.f / det;
            const float tv[3] = {o[0] - tri.v0[0], o[1] - tri.v0[1], o[2] - tri.v0[2]};
            const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * id;
            if (u < 0.f || u > 1.f) {
              continue;
            }
            const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                                 tv[0] * e1[1] - tv[1] * e1[0]};
            const float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * id;
            if (v < 0.f || u + v > 1.f) {
              continue;
            }
            const float hit_t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * id;
            hit = hit_t > 0.f && hit_t < tmax;
          }
        } else if (sp < 62) {
          stack[sp++] = n.a;     // the right child
          stack[sp++] = ni + 1;  // the left child, next
        }
      }
    }
    hits += hit;
  }
  return hits / 5.f;
}

void EnhancedPost::update_rt(SharedRenderState* render_state) {
  m_rt_ready = false;
  if (!render_state->loader) {
    return;
  }
  // the hierarchies are taken from the loader whatever the settings (the roof over Jak is traced on
  // the CPU); on the GPU only for the ray-traced reflections and occlusion
  const bool on_gpu = m_settings.ray_traced > 0 || m_settings.rt_ao;
  auto levels = render_state->loader->get_in_use_levels();
  std::vector<u64> in_use;
  for (auto* lev : levels) {
    if (!m_rt_levels.count(lev->load_id)) {
      // the hierarchy the loader built for it (once)
      m_rt_levels[lev->load_id].cpu = enhanced_take_rt(lev->level->level_name);
    }
    auto& gpu = m_rt_levels[lev->load_id];
    if (on_gpu && gpu.cpu && !gpu.buf[0]) {
      const auto& rt = gpu.cpu;
      const void* data[3] = {rt->nodes.data(), rt->tris.data(), rt->palettes.data()};
      gpu.size[0] = rt->nodes.size() * sizeof(EnhancedRtLevel::Node);
      gpu.size[1] = rt->tris.size() * sizeof(EnhancedRtLevel::Tri);
      gpu.size[2] = std::max<size_t>(rt->palettes.size(), 1) * sizeof(u32);
      u32 none = 0;
      if (rt->palettes.empty()) {
        data[2] = &none;
      }
      glGenBuffers(3, gpu.buf);
      for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_COPY_READ_BUFFER, gpu.buf[i]);
        glBufferData(GL_COPY_READ_BUFFER, gpu.size[i], data[i], GL_STATIC_DRAW);
      }
      glBindBuffer(GL_COPY_READ_BUFFER, 0);
      if (rt->layer_count > 0) {
        gpu.layers = rt->layer_count;
        glGenTextures(1, &gpu.tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, gpu.tex);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, kRtLayerSize, kRtLayerSize, gpu.layers);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, kRtLayerSize, kRtLayerSize, gpu.layers,
                        GL_RGBA, GL_UNSIGNED_BYTE, rt->layers.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
      }
      // the CPU keeps only the geometry
      std::vector<u32>().swap(rt->palettes);
      std::vector<u32>().swap(rt->layers);
    }
    if (gpu.buf[0] && in_use.size() < 16) {
      in_use.push_back(lev->load_id);
    }
  }
  // forget the levels that went away
  for (auto it = m_rt_levels.begin(); it != m_rt_levels.end();) {
    bool used = false;
    for (auto* lev : levels) {
      used |= lev->load_id == it->first;
    }
    if (!used) {
      if (it->second.buf[0]) {
        glDeleteBuffers(3, it->second.buf);
      }
      if (it->second.tex) {
        glDeleteTextures(1, &it->second.tex);
      }
      it = m_rt_levels.erase(it);
    } else {
      ++it;
    }
  }
  // the roof over Jak, a few times a second
  {
    static int frame = 0;
    if (++frame % 6 == 0 && g_enhanced_player.valid) {
      const float cover = trace_cover(g_enhanced_player.pos);
      // test switch: JAK_ENHANCED_COVER_LOG=1 logs each change
      static const bool log = std::getenv("JAK_ENHANCED_COVER_LOG") != nullptr;
      if (log && cover != m_cover_now) {
        lg::info("[enhanced] roof over Jak {:.1f} at {:.0f} {:.0f} {:.0f} m", cover,
                 g_enhanced_player.pos[0] / 4096.f, g_enhanced_player.pos[1] / 4096.f,
                 g_enhanced_player.pos[2] / 4096.f);
      }
      m_cover_now = cover;
    }
  }
  if (!on_gpu || in_use.empty()) {
    m_rt_count = 0;
    return;
  }
  if (in_use != m_rt_in_use) {
    // one buffer of each kind for all of them (copied on the GPU)
    m_rt_in_use = in_use;
    u32 total[3] = {0, 0, 0};
    for (auto id : in_use) {
      for (int i = 0; i < 3; i++) {
        total[i] += m_rt_levels[id].size[i];
      }
    }
    if (!m_rt_buf[0]) {
      glGenBuffers(3, m_rt_buf);
    }
    for (int i = 0; i < 3; i++) {
      glBindBuffer(GL_COPY_WRITE_BUFFER, m_rt_buf[i]);
      glBufferData(GL_COPY_WRITE_BUFFER, total[i], nullptr, GL_STATIC_DRAW);
    }
    // and one texture array (with mipmaps, for the far hits)
    GLint max_layers = 2048;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    int layers = 0;
    for (auto id : in_use) {
      layers += m_rt_levels[id].layers;
    }
    layers = std::clamp(layers, 1, (int)max_layers);
    if (m_rt_tex) {
      glDeleteTextures(1, &m_rt_tex);
    }
    glGenTextures(1, &m_rt_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_rt_tex);
    int mips = 1;
    while ((kRtLayerSize >> mips) > 0) {
      mips++;
    }
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, mips, GL_RGBA8, kRtLayerSize, kRtLayerSize, layers);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    int layer_at = 0;
    u32 offset[3] = {0, 0, 0};
    m_rt_count = 0;
    for (auto id : in_use) {
      const auto& gpu = m_rt_levels[id];
      auto* base = &m_rt_base[m_rt_count * 4];
      base[0] = offset[0] / sizeof(EnhancedRtLevel::Node);
      base[1] = offset[1] / sizeof(EnhancedRtLevel::Tri);
      base[2] = offset[2] / sizeof(u32) / 8;
      base[3] = layer_at;
      if (gpu.tex && layer_at + gpu.layers <= layers) {
        glCopyImageSubData(gpu.tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, m_rt_tex, GL_TEXTURE_2D_ARRAY,
                           0, 0, 0, layer_at, kRtLayerSize, kRtLayerSize, gpu.layers);
        layer_at += gpu.layers;
      } else {
        base[3] = -1;  // no textures: mean colors
      }
      for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_COPY_READ_BUFFER, gpu.buf[i]);
        glBindBuffer(GL_COPY_WRITE_BUFFER, m_rt_buf[i]);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, offset[i], gpu.size[i]);
        offset[i] += gpu.size[i];
      }
      m_rt_count++;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    lg::info("[enhanced] ray tracing: {} levels, {:.0f} MB, {} textures", m_rt_count,
             (total[0] + total[1] + total[2]) / 1e6, layer_at);
  }
  m_rt_ready = m_rt_count > 0;
}

Shader& EnhancedPost::activate_composite(SharedRenderState* render_state, int w, int h) {
  auto& shader = render_state->shaders[ShaderId::ENH_COMPOSITE];
  shader.activate();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, m_settings.bloom && m_levels[0].tex ? m_levels[0].tex : m_black_tex);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, m_ssao_ready ? m_ao_tex[0] : m_white_tex);
  // depth for the depth of field (only valid when the occlusion pass copied it this frame)
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, m_ssao_ready ? m_depth_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE4);
  glBindTexture(GL_TEXTURE_2D, m_shafts_ready ? m_shaft_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE7);
  glBindTexture(GL_TEXTURE_2D, m_gi_ready ? m_gi_tex[0] : m_black_tex);
  glActiveTexture(GL_TEXTURE8);
  glBindTexture(GL_TEXTURE_2D, m_vol_ready ? m_vol_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE9);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready ? m_shadow_tex : m_no_shadow_tex);
  glActiveTexture(GL_TEXTURE11);
  glBindTexture(GL_TEXTURE_2D, m_normals_ready ? m_normal_tex : m_black_tex);
  glActiveTexture(GL_TEXTURE0);

  auto id = shader.id();
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glUniform1i(glGetUniformLocation(id, "tex_T1"), 1);
  glUniform1i(glGetUniformLocation(id, "tex_T2"), 2);
  glUniform1i(glGetUniformLocation(id, "ao_debug"), m_settings.ssao_debug);
  glUniform1i(glGetUniformLocation(id, "tex_T11"), 11);
  glUniform1i(glGetUniformLocation(id, "normal_debug"), m_settings.normal_debug);
  // ray-traced reflections: the levels' hierarchies (update_rt), and the time-of-day weights
  const bool rt = m_settings.ray_traced > 0 && m_rt_ready && g_enhanced_tod.valid;
  glUniform1i(glGetUniformLocation(id, "rt_mode"), rt ? m_settings.ray_traced : 0);
  if (rt) {
    for (int i = 0; i < 3; i++) {
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, m_rt_buf[i]);
    }
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_rt_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T12"), 12);
    glUniform1i(glGetUniformLocation(id, "rt_levels"), m_rt_count);
    glUniform4iv(glGetUniformLocation(id, "rt_base"), m_rt_count, m_rt_base.data());
    // the same decoding as interp_time_of_day: palette p, channel ch, 64 = 1
    float tod[8 * 4];
    for (int p = 0; p < 8; p++) {
      for (int ch = 0; ch < 4; ch++) {
        u32 word = (u32)g_enhanced_tod.itimes[p / 2][(p % 2) * 2 + ch / 2];
        u32 hw = (ch % 2) ? (word >> 16) : word;
        tod[p * 4 + ch] = (hw & 0xff) / 64.f;
      }
    }
    glUniform4fv(glGetUniformLocation(id, "rt_tod"), 8, tod);
  }
  glUniform1i(glGetUniformLocation(id, "tex_T3"), 3);
  glUniform1i(glGetUniformLocation(id, "tex_T4"), 4);
  glUniform1i(glGetUniformLocation(id, "tex_T7"), 7);
  glUniform1i(glGetUniformLocation(id, "tex_T8"), 8);
  glUniform1i(glGetUniformLocation(id, "tex_T9"), 9);
  glUniform1f(glGetUniformLocation(id, "shadow_strength"),
              m_shadow_ready ? m_settings.shadow_strength : 0.f);
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, m_light_mvp);
  glUniform1i(glGetUniformLocation(id, "volume"), m_vol_ready);
  glUniform2f(glGetUniformLocation(id, "vol_texel"), m_vol_w ? 1.f / m_vol_w : 0.f,
              m_vol_h ? 1.f / m_vol_h : 0.f);
  glUniform1f(glGetUniformLocation(id, "gi_strength"),
              m_gi_ready ? m_settings.gi_strength * game_gi_scale() : 0.f);
  glUniform1f(glGetUniformLocation(id, "shafts_strength"),
              m_shafts_ready ? m_settings.shafts_strength * (1.f + 1.2f * m_clear) : 0.f);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  const bool dof = m_settings.dof && m_ssao_ready;
  glUniform3f(glGetUniformLocation(id, "dof"), m_settings.dof_start * kMeter,
              m_settings.dof_end * kMeter, dof ? m_settings.dof_radius * h / 1080.f : 0.f);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / w, 1.f / h);
  glUniform1f(glGetUniformLocation(id, "bloom_strength"),
              m_settings.bloom ? m_settings.bloom_strength : 0.f);
  // the area's grade on top of the settings (update_scene)
  glUniform1f(glGetUniformLocation(id, "saturation"), m_settings.saturation * m_grade.saturation);
  glUniform1f(glGetUniformLocation(id, "contrast"), m_settings.contrast + m_grade.contrast);
  glUniform1f(glGetUniformLocation(id, "warmth"), m_settings.warmth + m_grade.warmth);
  glUniform3fv(glGetUniformLocation(id, "grade_lift"), 1, m_grade.lift);
  glUniform3fv(glGetUniformLocation(id, "grade_gain"), 1, m_grade.gain);
  // soft shadows: the shadow map again, read as plain depths for the blocker search
  if (!m_shadow_raw_sampler) {
    glGenSamplers(1, &m_shadow_raw_sampler);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_shadow_raw_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  }
  glActiveTexture(GL_TEXTURE13);
  glBindTexture(GL_TEXTURE_2D, m_shadow_ready ? m_shadow_tex : m_no_shadow_tex);
  glBindSampler(13, m_shadow_raw_sampler);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T13"), 13);
  // the map's depth spans 2 x 300 m (do_shadow_map), its side 2 x range
  glUniform4f(glGetUniformLocation(id, "shadow_params"), 600.f, 2.f * m_settings.shadow_range,
              m_settings.shadow_softness, m_settings.soft_shadows ? 1.f : 0.f);
  {
    // the sun for the clouds, day or night (view space), and how high it stands (the camera's up:
    // last frame's, near enough)
    float len = std::sqrt(m_sun_view[0] * m_sun_view[0] + m_sun_view[1] * m_sun_view[1] +
                          m_sun_view[2] * m_sun_view[2]);
    float up = len > 0.f ? (m_sun_view[0] * m_prev_camera.rot[1][0] +
                            m_sun_view[1] * m_prev_camera.rot[1][1] +
                            m_sun_view[2] * m_prev_camera.rot[1][2]) / len
                         : 0.5f;
    float sv[3] = {len > 0.f ? m_sun_view[0] / len : 0.f, len > 0.f ? m_sun_view[1] / len : 0.f,
                   len > 0.f ? m_sun_view[2] / len : 1.f};
    // test: the sun low behind the camera (the rainbow ahead)
    static const bool rainbow_test = std::getenv("JAK_ENHANCED_RAINBOW_TEST") != nullptr;
    if (rainbow_test) {
      for (int i = 0; i < 3; i++) {
        sv[i] = m_prev_camera.rot[1][i] * 0.34f - (i == 2 ? 0.94f : 0.f);
      }
      up = 0.34f;
    }
    glUniform4f(glGetUniformLocation(id, "sky_sun"), sv[0], sv[1], sv[2], up);
  }
  // (no sky, clouds or weather without a 3D scene: loading screens, menus)
  glUniform4f(glGetUniformLocation(id, "clouds"),
              m_settings.clouds && m_ssao_ready ? m_settings.cloud_amount : 0.f, m_grade.clouds,
              (float)std::fmod(m_time, 10000.0), m_night);
  const float on = m_ssao_ready ? 1.f : 0.f;
  glUniform1f(glGetUniformLocation(id, "city_glow"), m_city ? m_night * on : 0.f);
  glUniform4f(glGetUniformLocation(id, "weather"), m_rain * on, m_dust * on,
              (float)std::fmod(m_time, 1000.0), m_wet * on);
  glUniform1f(glGetUniformLocation(id, "puddle_level"), m_puddle * on);
  glUniform4f(glGetUniformLocation(id, "weather2"), m_storm * on, m_mist * on, m_clear * on,
              m_flash * on);
  {
    const float wl = std::max(1e-3f, std::sqrt(m_wind[0] * m_wind[0] + m_wind[1] * m_wind[1]));
    glUniform4f(glGetUniformLocation(id, "dust_wind"), m_wind[0] / wl, m_wind[1] / wl,
                (float)m_drift[0], (float)m_drift[1]);
  }
  glUniform1f(glGetUniformLocation(id, "sunny"), m_sunny * on);
  glUniform1f(glGetUniformLocation(id, "sheet"), m_sheet * on);
  glUniform2f(glGetUniformLocation(id, "snow_heat"), m_snow * on, m_heat * on);
  glUniform4f(glGetUniformLocation(id, "bolt"), m_bolt[0], m_bolt[1], m_bolt[2],
              m_bolt[3] * m_storm * on);
  glUniform3f(glGetUniformLocation(id, "cine_dof"), m_focus, m_settings.cine_aperture,
              m_focus > 0.f && m_ssao_ready ? m_cine * 9.f * h / 1080.f : 0.f);
  glUniform1f(glGetUniformLocation(id, "blast_sparks"), m_settings.blast_sparks);
  glUniform1f(glGetUniformLocation(id, "close_up"), m_movie * kCloseUp);
  glUniform1f(glGetUniformLocation(id, "vignette"),
              m_settings.vignette + (g_enhanced_menu.photo_active ? m_settings.photo_vignette : 0.f));
  glUniform1f(glGetUniformLocation(id, "sharpen"), m_settings.sharpen);
  return shader;
}

void EnhancedPost::draw_photo_hint() {
  if (!g_enhanced_menu.photo_active) {
    return;
  }
  const double now =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const float age = (float)(now - g_enhanced_menu.photo_entered_at.load());
  if (age > 6.f) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y - 40.f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
  ImGui::SetNextWindowBgAlpha(0.6f * std::min(1.f, (6.f - age) * 2.f));
  ImGui::Begin("##photo-hint", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav);
  ImGui::TextUnformatted("Photo mode   left stick: move   right stick: look   L1 / R1: down / up   "
                         "R2: slower   F2: picture   F1: filters   F3 or Start: back");
  ImGui::End();
}

// the game wheel (Select, N): three slices, Jak 1 upper left, Jak II upper right, Jak 3 below, the
// one picked lit and pushed out. The game reads the stick and the buttons (pc-enhanced-wheel)
void EnhancedPost::draw_game_wheel() {
  const int pick = g_enhanced_menu.wheel.load();
  if (pick < 0) {
    return;
  }
  struct Slice {
    float from, to;  // degrees on screen: 0 to the right, clockwise
    const char* name;
    const char* sub;
    ImU32 color, lit;
  };
  static const Slice kSlices[3] = {
      {150.f, 270.f, "Jak", "and Daxter", IM_COL32(46, 84, 40, 225), IM_COL32(92, 160, 70, 240)},
      {-90.f, 30.f, "Jak II", "Renegade", IM_COL32(40, 56, 84, 225), IM_COL32(74, 112, 170, 240)},
      {30.f, 150.f, "Jak 3", "Wasteland", IM_COL32(96, 60, 28, 225), IM_COL32(196, 124, 52, 240)}};
  if (!m_wheel_loaded) {
    m_wheel_loaded = true;
    for (int i = 0; i < 3; i++) {
      const auto path = file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "wheel" /
                        fmt::format("jak{}.png", i + 1);
      int w, h;
      u8* data = stbi_load(path.string().c_str(), &w, &h, nullptr, 4);
      if (!data) {
        continue;
      }
      glGenTextures(1, &m_wheel_tex[i]);
      glBindTexture(GL_TEXTURE_2D, m_wheel_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
      glGenerateMipmap(GL_TEXTURE_2D);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindTexture(GL_TEXTURE_2D, 0);
      stbi_image_free(data);
    }
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // the text font: Noto Sans (game/assets/fonts), added once, sharp at any size (ImGui's fonts are
  // dynamic); the pixel font of the debug windows if it's missing
  static ImFont* noto = nullptr;
  static bool noto_tried = false;
  if (!noto_tried) {
    noto_tried = true;
    const auto path =
        file_util::get_jak_project_dir() / "game" / "assets" / "fonts" / "NotoSansJP-Medium.ttf";
    if (fs::exists(path)) {
      noto = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 32.f);
    }
  }
  ImFont* font = noto ? noto : ImGui::GetFont();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const ImVec2 c(size.x * 0.5f, size.y * 0.45f);
  const float r = size.y * 0.32f, r0 = r * 0.24f;
  constexpr float kRad = 3.14159265f / 180.f;
  const auto at = [&](float deg, float rad) {
    return ImVec2(c.x + std::cos(deg * kRad) * rad, c.y + std::sin(deg * kRad) * rad);
  };
  const auto text = [&](const char* s, float px, ImVec2 p, ImU32 col) {
    const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.f, s);
    dl->AddText(font, px, ImVec2(p.x - ts.x * 0.5f + 2, p.y - ts.y * 0.5f + 2), IM_COL32(0, 0, 0, 160), s);
    dl->AddText(font, px, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), col, s);
  };
  dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 120));
  for (int i = 0; i < 3; i++) {
    const auto& sl = kSlices[i];
    const bool lit = pick == i + 1;
    const float mid = (sl.from + sl.to) * 0.5f;
    // the slice picked moves out a little
    const ImVec2 off = lit ? ImVec2(std::cos(mid * kRad) * r * 0.05f, std::sin(mid * kRad) * r * 0.05f)
                           : ImVec2(0, 0);
    const float a0 = sl.from + 1.5f, a1 = sl.to - 1.5f;
    constexpr int kSteps = 40;
    // (the slice in quads, without the antialiased edges that would show as seams between them)
    const auto flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    // the portrait, if there is one: a square over the whole slice (its bounding box), cut to it
    const GLuint tex = m_wheel_tex[i];
    ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
    for (int k = 0; k <= kSteps; k++) {
      for (const float rad : {r0, r}) {
        const ImVec2 p = at(a0 + (a1 - a0) * k / kSteps, rad);
        lo = ImVec2(std::min(lo.x, p.x), std::min(lo.y, p.y));
        hi = ImVec2(std::max(hi.x, p.x), std::max(hi.y, p.y));
      }
    }
    // (moved so each face sits in its slice: Jak 3's would be under the middle)
    static const float kShift[3][2] = {{0.f, 0.02f}, {-0.06f, 0.04f}, {0.f, 0.1f}};
    const float side = std::max(hi.x - lo.x, hi.y - lo.y) * (i == 2 ? 1.15f : 1.f);
    const ImVec2 pc((lo.x + hi.x) * 0.5f + kShift[i][0] * side, (lo.y + hi.y) * 0.5f + kShift[i][1] * side);
    const auto uv = [&](ImVec2 p) { return ImVec2((p.x - pc.x) / side + 0.5f, (p.y - pc.y) / side + 0.5f); };
    for (int k = 0; k < kSteps; k++) {
      const float b0 = a0 + (a1 - a0) * k / kSteps, b1 = a0 + (a1 - a0) * (k + 1) / kSteps;
      const ImVec2 p[4] = {at(b0, r0), at(b0, r), at(b1, r), at(b1, r0)};
      const ImVec2 q[4] = {ImVec2(p[0].x + off.x, p[0].y + off.y), ImVec2(p[1].x + off.x, p[1].y + off.y),
                           ImVec2(p[2].x + off.x, p[2].y + off.y), ImVec2(p[3].x + off.x, p[3].y + off.y)};
      if (tex) {
        dl->AddImageQuad((ImTextureID)(intptr_t)tex, q[0], q[1], q[2], q[3], uv(p[0]), uv(p[1]), uv(p[2]),
                         uv(p[3]), lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 120, 120, 235));
      } else {
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], lit ? sl.lit : sl.color);
      }
    }
    dl->Flags = flags;
    // the rim
    dl->PathClear();
    dl->PathArcTo(ImVec2(c.x + off.x, c.y + off.y), r, a0 * kRad, a1 * kRad, kSteps);
    dl->PathStroke(lit ? IM_COL32(255, 236, 160, 255) : IM_COL32(200, 190, 150, 140), 0,
                   lit ? 4.f : 2.f);
    const ImU32 col = lit ? IM_COL32(255, 244, 200, 255) : IM_COL32(230, 220, 190, 200);
    const bool playing = (int)g_game_version == i + 1;
    if (tex) {
      // with a portrait: the name on a dark plate outside the wheel, off the faces
      const ImVec2 out = at(mid, r * (i == 2 ? 1.16f : 1.3f));
      const ImVec2 label(out.x + off.x, out.y + off.y);
      const float pw = std::max(r * 0.5f, font->CalcTextSizeA(r * 0.15f, FLT_MAX, 0.f, sl.name).x + r * 0.12f),
                  ph = r * (playing ? 0.23f : 0.16f);
      dl->AddRectFilled(ImVec2(label.x - pw * 0.5f, label.y - r * 0.075f),
                        ImVec2(label.x + pw * 0.5f, label.y - r * 0.075f + ph), IM_COL32(10, 10, 14, 170),
                        r * 0.03f);
      text(sl.name, r * 0.15f, label, col);
      if (playing) {
        text("playing now", r * 0.065f, ImVec2(label.x, label.y + r * 0.1f), IM_COL32(255, 220, 120, 230));
      }
    } else {
      ImVec2 label = at(mid, (r0 + r) * 0.55f);
      label.x += off.x;
      label.y += off.y;
      text(sl.name, r * 0.2f, ImVec2(label.x, label.y - r * 0.06f), col);
      text(sl.sub, r * 0.09f, ImVec2(label.x, label.y + r * 0.1f), col);
      if (playing) {
        text("playing", r * 0.075f, ImVec2(label.x, label.y + r * 0.2f), IM_COL32(255, 220, 120, 230));
      }
    }
  }
  // the middle: what X does
  dl->AddCircleFilled(c, r0 * 0.92f, IM_COL32(16, 16, 20, 235), 48);
  dl->AddCircle(c, r0 * 0.92f, IM_COL32(200, 190, 150, 160), 48, 2.f);
  const char* what = pick == 0 ? "Choose" : pick == (int)g_game_version ? "Resume" : "Play";
  text(what, r * 0.1f, c, IM_COL32(240, 230, 200, 255));
  // what the buttons do, with an Xbox pad's letters (A plays, Y the nap, B back)
  const float px = size.y * 0.032f, br = px * 0.62f;
  const auto button = [&](char kind, ImVec2 p) {
    ImU32 fill = IM_COL32(20, 20, 24, 230);
    const char* letter = nullptr;
    switch (kind) {
      case 'a':
        fill = IM_COL32(70, 170, 60, 255);
        letter = "A";
        break;
      case 'b':
        fill = IM_COL32(200, 50, 50, 255);
        letter = "B";
        break;
      case 'y':
        fill = IM_COL32(225, 175, 30, 255);
        letter = "Y";
        break;
      default:
        break;
    }
    dl->AddCircleFilled(p, br, fill, 32);
    dl->AddCircle(p, br, IM_COL32(230, 230, 230, 120), 32, 1.5f);
    if (letter) {
      text(letter, br * 1.3f, p, IM_COL32(255, 255, 255, 255));
    } else {  // the left stick
      const float s = br * 0.5f, w = std::max(2.f, br * 0.16f);
      dl->AddCircleFilled(p, s * 0.95f, IM_COL32(200, 200, 200, 255), 24);
      dl->AddCircle(p, s * 0.55f, IM_COL32(60, 60, 60, 255), 24, w * 0.6f);
    }
  };
  struct Hint {
    char kind;
    const char* what;
  };
  const Hint hints[4] = {{'s', "Choose"}, {'a', "Play"}, {'y', "Nap"}, {'b', "Back"}};
  const float gap = px * 1.4f;
  float total = 0.f;
  for (const auto& h : hints) {
    total += br * 2.f + px * 0.35f + font->CalcTextSizeA(px, FLT_MAX, 0.f, h.what).x;
  }
  total += gap * 3.f;
  float x = c.x - total * 0.5f;
  const float y = c.y + r + size.y * 0.15f;
  dl->AddRectFilled(ImVec2(x - px * 0.8f, y - px * 0.95f), ImVec2(x + total + px * 0.8f, y + px * 0.95f),
                    IM_COL32(10, 10, 14, 170), px * 0.9f);
  for (const auto& h : hints) {
    button(h.kind, ImVec2(x + br, y));
    x += br * 2.f + px * 0.35f;
    const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.f, h.what);
    dl->AddText(font, px, ImVec2(x, y - ts.y * 0.5f), IM_COL32(240, 232, 210, 255), h.what);
    x += ts.x + gap;
  }
}

void EnhancedPost::draw_menu() {
  ImGui::SetNextWindowPos(ImVec2(30, 60), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  // (focused as it opens, so a pad can move through it right away)
  static int drawn_frame = -10;
  if (ImGui::GetFrameCount() - drawn_frame > 1) {
    ImGui::SetNextWindowFocus();
  }
  drawn_frame = ImGui::GetFrameCount();
  // no close button: \ or F1 closes it (and gives the mouse back to the camera); Select + R3 on a pad
  if (!ImGui::Begin("Jak Enhanced Trilogy by stichlola  ( \\, F1 or Select+R3 closes )", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  auto& s = m_settings;
  // photo mode: in and out, and while it's on its filter
  if (ImGui::Button(g_enhanced_menu.photo ? "Leave photo mode  (F3, Start)" : "Photo mode  (F3)")) {
    g_enhanced_menu.photo = !g_enhanced_menu.photo;
    if (g_enhanced_menu.photo) {
      g_enhanced_menu.open = false;
    }
  }
  if (g_enhanced_menu.photo_active) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("Filter", kPhotoFilters[std::clamp(s.photo_filter, 0, kPhotoFilterCount - 1)].name)) {
      for (int i = 0; i < kPhotoFilterCount; i++) {
        if (ImGui::Selectable(kPhotoFilters[i].name, s.photo_filter == i)) {
          s.photo_filter = i;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Vignette##photo", &s.photo_vignette, 0.f, 1.f, "%.2f");
    ImGui::TextDisabled("Close this menu (F1), then F2 takes the picture (in your OpenGOAL screenshots folder).");
  }
  if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
    // (in the menu's order: Performance sits between Classic and Medium)
    static const char* presets[] = {"Classic", "Performance", "Medium", "Maximum"};
    static const int preset_ids[] = {0, 3, 1, 2};
    ImGui::TextUnformatted("Preset:");
    for (int i = 0; i < 4; i++) {
      ImGui::SameLine();
      if (ImGui::Button(presets[i], ImVec2(110, 0))) {
        apply_preset(preset_ids[i]);
      }
    }
    // the game's nap window
    if (ImGui::Button("Nap...  (N, Select)")) {
      g_enhanced_menu.ask(12, "");
      g_enhanced_menu.open = false;
    }
    ImGui::Checkbox("Enhanced graphics (all)", &s.enabled);
    ImGui::BeginDisabled(!s.enabled);
    // two columns of switches
    int n = 0;
    auto check = [&](const char* label, bool* value) {
      if (n++ % 2) {
        ImGui::SameLine(260);
      }
      return ImGui::Checkbox(label, value);
    };
    check("Antialiasing (TAA)", &s.taa);
    check("Volumetric mist", &s.volume);
    check("Sun shadows", &s.shadow_map);
    check("Character and tree shadows", &s.shadow_casters);
    check("Contact shadows", &s.sun_shadows);
    check("Occlusione (SSAO)", &s.ssao);
    check("Luce indiretta (SSGI)", &s.gi);
    check("Character lighting", &g_enhanced_merc.enabled);
    check("Bloom", &s.bloom);
    check("God rays", &s.shafts);
    check("Depth of field", &s.dof);
    bool blur = s.motion_blur > 0.f;
    if (check("Motion blur", &blur)) {
      s.motion_blur = blur ? 0.3f : 0.f;
    }
    check("Dynamic lights", &s.lights);
    check("Fuochi (calore, braci)", &s.fire);
    bool light_shadows = s.light_shadow > 0.f;
    if (check("Light shadows", &light_shadows)) {
      s.light_shadow = light_shadows ? 0.85f : 0.f;
    }
    check("Geometry normals", &s.geometry_normals);
    bool prints = s.prints > 0.f;
    if (check("Footprints in the sand", &prints)) {
      s.prints = prints ? 1.f : 0.f;
    }
    bool dark = s.dark_nights > 0.f;
    if (check("Dark nights", &dark)) {
      s.dark_nights = dark ? 1.f : 0.f;
    }
    bool spill = s.light_spill > 0.f;
    if (check("Light spill at night", &spill)) {
      s.light_spill = spill ? 1.f : 0.f;
    }
    check("Cloud shadows", &s.cloud_shadows);
    bool flare = s.lens_flare > 0.f;
    if (check("Lens flare", &flare)) {
      s.lens_flare = flare ? 0.6f : 0.f;
    }
    check("Jak's lantern in the desert", &s.desert_lamp);
    bool smoke = s.lit_smoke > 0.f;
    if (check("Lit smoke", &smoke)) {
      s.lit_smoke = smoke ? 0.8f : 0.f;
    }
    check("Extended draw distance", &s.far_view);
    bool relief = s.relief > 0.f;
    if (check("Surface relief", &relief)) {
      s.relief = relief ? 1.f : 0.f;
    }
    bool mats = s.materials > 0.f;
    bool traced = s.ray_traced > 0;
    if (check("Ray-traced reflections", &traced)) {
      s.ray_traced = traced ? 1 : 0;
    }
    check("Ray-traced ambient occlusion", &s.rt_ao);
    if (check("Reflective metal and glass", &mats)) {
      s.materials = mats ? 1.f : 0.f;
    }
    check("Wind on the grass", &g_enhanced_wind.enabled);
    check("Grass bends away", &g_enhanced_wind.push);
    check("Color grade per area", &s.area_grading);
    check("Soft shadows", &s.soft_shadows);
    check("Clouds", &s.clouds);
    check("Cutscene depth of field", &s.cine_dof);
    bool sparks = s.blast_sparks > 0.f;
    if (check("Explosion sparks", &sparks)) {
      s.blast_sparks = sparks ? 1.f : 0.f;
    }
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Vivid colors", &s.vivid, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Ambient sounds", &s.ambience, 0.f, 1.f, "%.2f");
    // the subtitles' size (accessibility): the default is smaller than the PS2's
    {
      static const int kSizes[] = {100, 120, 138, 170};
      int pick = 0;
      for (int i = 0; i < 4; i++) {
        if (g_enhanced_menu.subtitle_size >= kSizes[i]) {
          pick = i;
        }
      }
      static const char* const kSizeNames[] = {"Small", "Medium", "Large (original)", "Extra large"};
      ImGui::SetNextItemWidth(200);
      if (ImGui::Combo("Subtitle size", &pick, kSizeNames, 4)) {
        g_enhanced_menu.subtitle_size = kSizes[pick];
      }
    }
    weather_menu(s, "Weather");
    ImGui::SetNextItemWidth(200);
    ImGui::Combo("Vegetation", &g_enhanced_vegetation, "Classic\0Moderate\0Lush\0");
    ImGui::BeginDisabled(!s.lights);
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Lamp strength", &s.lamp_strength, 0.f, 3.f, "%.2f");
    if (ImGui::TreeNode("Place a light")) {
      // a light of your own where Jak stands, 2.5 m up (saved in game/assets/enhanced/lights.json)
      static float color[3] = {1.f, 0.62f, 0.32f};
      static float radius = 8.f;
      static bool flicker = false;
      ImGui::SetNextItemWidth(200);
      ImGui::ColorEdit3("Color##ml", color);
      ImGui::SetNextItemWidth(200);
      ImGui::SliderFloat("Radius (m)##ml", &radius, 2.f, 40.f, "%.0f");
      ImGui::Checkbox("Flickers like a fire##ml", &flicker);
      auto& list = g_manual_lights[game_index()];
      const auto& pl = g_enhanced_player;
      if (ImGui::Button("Add a light here") && pl.valid) {
        ManualLight l;
        l.note = g_enhanced_scene.level;
        l.pos[0] = pl.pos[0];
        l.pos[1] = pl.pos[1] + 2.5f * 4096.f;
        l.pos[2] = pl.pos[2];
        l.radius = radius * 4096.f;
        std::memcpy(l.color, color, sizeof(color));
        l.flicker = flicker;
        list.push_back(l);
        save_manual_lights();
      }
      ImGui::SameLine();
      if (ImGui::Button("Remove the nearest") && pl.valid && !list.empty()) {
        size_t best = 0;
        float best_d2 = 1e30f;
        for (size_t i = 0; i < list.size(); i++) {
          float d2 = 0.f;
          for (int k = 0; k < 3; k++) {
            d2 += (list[i].pos[k] - pl.pos[k]) * (list[i].pos[k] - pl.pos[k]);
          }
          if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
          }
        }
        list.erase(list.begin() + best);
        save_manual_lights();
      }
      ImGui::Text("%d lights of your own in this game", (int)list.size());
      ImGui::TreePop();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("Water");
    n = 0;
    bool swell = s.water_swell > 0.f, refr = s.water_refraction > 0.f, caus = s.water_caustics > 0.f;
    check("Reflections", &s.water_reflections);
    if (check("Swell", &swell)) {
      s.water_swell = swell ? 1.f : 0.f;
    }
    if (check("Refraction", &refr)) {
      s.water_refraction = refr ? 0.012f : 0.f;
    }
    if (check("Caustics", &caus)) {
      s.water_caustics = caus ? 0.8f : 0.f;
    }
    ImGui::EndDisabled();
  }
  if (ImGui::CollapsingHeader("City", ImGuiTreeNodeFlags_DefaultOpen)) {
    // passers-by, traffic, guards and Metal Heads (the game scales its wanted counts, see
    // enhanced-population-count); the game reads the same variable at boot
    ImGui::SetNextItemWidth(200);
    if (ImGui::Combo("Population", &m_population, "Low\0Normal\0High\0Extreme\0")) {
      g_enhanced_menu.ask(6, std::to_string(m_population));
    }
    ImGui::SetNextItemWidth(200);
    int fov = g_enhanced_menu.fov;
    if (ImGui::SliderInt("Field of view %", &fov, 80, 140)) {
      g_enhanced_menu.fov = fov;
    }
    bool map = g_enhanced_menu.minimap;
    if (ImGui::Checkbox("Minimap", &map)) {
      g_enhanced_menu.minimap = map;
    }
    ImGui::BeginDisabled(!map);
    ImGui::SetNextItemWidth(200);
    int map_size = g_enhanced_menu.minimap_size;
    if (ImGui::SliderInt("Minimap size %", &map_size, 50, 100)) {
      g_enhanced_menu.minimap_size = map_size;
    }
    ImGui::EndDisabled();
  }
  save_menu_settings();
  if (ImGui::CollapsingHeader("Time of day", ImGuiTreeNodeFlags_DefaultOpen)) {
    struct Hour {
      const char* name;
      int hour;
    };
    static const Hour hours[] = {{"Dawn", 6},    {"Morning", 9},  {"Noon", 12},
                                 {"Sunset", 18}, {"Evening", 20}, {"Night", 23}};
    for (int i = 0; i < 6; i++) {
      if (i % 3) {
        ImGui::SameLine();
      }
      if (ImGui::Button(hours[i].name, ImVec2(159, 0))) {
        g_enhanced_menu.ask(1, std::to_string(hours[i].hour));
      }
    }
    static int hour = 12;
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("##hour", &hour, 0, 23, "%d:00");
    ImGui::SameLine();
    if (ImGui::Button("Set")) {
      g_enhanced_menu.ask(1, std::to_string(hour));
    }
    static bool frozen = false;
    if (ImGui::Checkbox("Freeze time", &frozen)) {
      g_enhanced_menu.ask(4, frozen ? "0" : "1");
    }
  }
  if (ImGui::CollapsingHeader("Teleport", ImGuiTreeNodeFlags_DefaultOpen)) {
    struct Place {
      const char* name;
      const char* continue_point;
    };
    static const Place places[] = {
        {"Plaza (Jak 2)", "piazza-start"},
        {"Port", "ctyport-start"},
        {"Hip Hog", "ctyport-hiphog"},
        {"Slums alley", "ctysluma-alley"},
        {"Industrial zone", "ctyinda-start"},
        {"Central sector", "ctygenb-start"},
        {"Metal Head city", "mhcitya-near-ctyport"},
        {"Spargus", "wascitya-start"},
        {"Desert", "desert-facing-wasdoors"},
    };
    // (the plaza only where it's built, see enhanced_has_piazza)
    int shown = 0;
    for (int i = enhanced_has_piazza() ? 0 : 1; i < (int)(sizeof(places) / sizeof(places[0])); i++) {
      const auto& p = places[i];
      if (shown++ % 2) {
        ImGui::SameLine();
      }
      if (ImGui::Button(p.name, ImVec2(240, 0))) {
        g_enhanced_menu.ask(2, p.continue_point);
      }
    }
    const auto& cam = g_enhanced_camera;
    if (cam.valid) {
      ImGui::TextDisabled("camera %.0f %.0f %.0f m", cam.trans[0] / kMeter, cam.trans[1] / kMeter,
                          cam.trans[2] / kMeter);
    }
  }
  ImGui::End();
}

std::string EnhancedPost::menu_settings_json() const {
  const auto& s = m_settings;
  nlohmann::json j;
  j["enabled"] = s.enabled;
  j["taa"] = s.taa;
  j["volume"] = s.volume;
  j["shadow_map"] = s.shadow_map;
  j["shadow_casters"] = s.shadow_casters;
  j["sun_shadows"] = s.sun_shadows;
  j["ssao"] = s.ssao;
  j["gi"] = s.gi;
  j["characters"] = g_enhanced_merc.enabled;
  j["bloom"] = s.bloom;
  j["shafts"] = s.shafts;
  j["dof"] = s.dof;
  j["motion_blur"] = s.motion_blur;
  j["lights"] = s.lights;
  j["lamp_strength"] = s.lamp_strength;
  j["fire"] = s.fire;
  j["light_shadow"] = s.light_shadow;
  j["geometry_normals"] = s.geometry_normals;
  j["prints"] = s.prints;
  j["dark_nights"] = s.dark_nights;
  j["cloud_shadows"] = s.cloud_shadows;
  j["lens_flare"] = s.lens_flare;
  j["desert_lamp"] = s.desert_lamp;
  j["lantern_intro_done"] = s.lantern_intro_done;
  j["light_spill"] = s.light_spill;
  j["lit_smoke"] = s.lit_smoke;
  j["far_view"] = s.far_view;
  j["materials"] = s.materials;
  j["ray_traced"] = s.ray_traced;
  j["rt_ao"] = s.rt_ao;
  j["relief"] = s.relief;
  j["wind"] = g_enhanced_wind.enabled;
  j["grass_push"] = g_enhanced_wind.push;
  j["vegetation"] = g_enhanced_vegetation;
  j["water_reflections"] = s.water_reflections;
  j["water_swell"] = s.water_swell;
  j["water_refraction"] = s.water_refraction;
  j["water_caustics"] = s.water_caustics;
  j["area_grading"] = s.area_grading;
  j["soft_shadows"] = s.soft_shadows;
  j["clouds"] = s.clouds;
  j["cine_dof"] = s.cine_dof;
  j["blast_sparks"] = s.blast_sparks;
  j["ambience"] = s.ambience;
  j["weather"] = s.weather;
  j["weather_mix"] = s.weather_mix;
  j["season"] = s.season;
  j["photo_filter"] = s.photo_filter;
  j["photo_vignette"] = s.photo_vignette;
  j["vivid"] = s.vivid;
  j["population"] = m_population;
  j["fov"] = g_enhanced_menu.fov.load();
  j["subtitle_size"] = g_enhanced_menu.subtitle_size.load();
  j["show_minimap"] = g_enhanced_menu.minimap.load();
  j["minimap_size"] = g_enhanced_menu.minimap_size.load();
  return j.dump(2);
}

void EnhancedPost::apply_menu_settings(const std::string& text) {
  auto j = parse_commented_json(text, "enhanced-settings.json");
  auto get = [&](const char* key, auto& value) {
    if (j.contains(key)) {
      value = j.at(key).get<std::decay_t<decltype(value)>>();
    }
  };
  auto& s = m_settings;
  get("enabled", s.enabled);
  get("taa", s.taa);
  get("volume", s.volume);
  get("shadow_map", s.shadow_map);
  get("shadow_casters", s.shadow_casters);
  get("sun_shadows", s.sun_shadows);
  get("ssao", s.ssao);
  get("gi", s.gi);
  get("characters", g_enhanced_merc.enabled);
  get("bloom", s.bloom);
  get("shafts", s.shafts);
  get("dof", s.dof);
  get("motion_blur", s.motion_blur);
  get("lights", s.lights);
  get("lamp_strength", s.lamp_strength);
  get("fire", s.fire);
  get("light_shadow", s.light_shadow);
  get("geometry_normals", s.geometry_normals);
  get("prints", s.prints);
  get("dark_nights", s.dark_nights);
  get("cloud_shadows", s.cloud_shadows);
  get("lens_flare", s.lens_flare);
  get("desert_lamp", s.desert_lamp);
  get("lantern_intro_done", s.lantern_intro_done);
  get("light_spill", s.light_spill);
  get("lit_smoke", s.lit_smoke);
  get("far_view", s.far_view);
  get("materials", s.materials);
  get("ray_traced", s.ray_traced);
  get("rt_ao", s.rt_ao);
  get("relief", s.relief);
  get("wind", g_enhanced_wind.enabled);
  get("grass_push", g_enhanced_wind.push);
  if (!std::getenv("JAK_ENHANCED_VEGETATION")) {
    get("vegetation", g_enhanced_vegetation);
    g_enhanced_vegetation = std::clamp(g_enhanced_vegetation, 0, 2);
  }
  get("water_reflections", s.water_reflections);
  get("water_swell", s.water_swell);
  get("water_refraction", s.water_refraction);
  get("water_caustics", s.water_caustics);
  get("area_grading", s.area_grading);
  get("soft_shadows", s.soft_shadows);
  get("clouds", s.clouds);
  get("cine_dof", s.cine_dof);
  get("blast_sparks", s.blast_sparks);
  get("ambience", s.ambience);
  get("weather", s.weather);
  s.weather = std::clamp(s.weather, 0, kWeatherCount - 1);
  get("weather_mix", s.weather_mix);
  get("season", s.season);
  get("photo_filter", s.photo_filter);
  get("photo_vignette", s.photo_vignette);
  s.season = std::clamp(s.season, 0, 4);
  get("vivid", s.vivid);
  get("population", m_population);
  {
    int fov = g_enhanced_menu.fov;
    get("fov", fov);
    g_enhanced_menu.fov = fov;
    int subtitle_size = g_enhanced_menu.subtitle_size;
    get("subtitle_size", subtitle_size);
    g_enhanced_menu.subtitle_size = subtitle_size;
  }
  m_population = std::clamp(m_population, 0, 3);
  bool map = true;  // ("show_minimap": the old "minimap" key saved it off by default)
  get("show_minimap", map);
  g_enhanced_menu.minimap = map;
  int map_size = g_enhanced_menu.minimap_size;
  get("minimap_size", map_size);
  g_enhanced_menu.minimap_size = map_size;
}

namespace {
bool all_on(int preset) {
  return preset == 2;
}
}  // namespace

// Classico: the game as it was (the enhanced pass off), Medio: the costly passes off (mist, indirect
// light, depth of field, the lights' shadows...), Massimo: everything on. Performance (3): Medium
// without the passes that cost the most on a weak graphics card (the sun's shadow map, the
// occlusion, the traced reflections, the far view, the extra plants), the look kept (grade, lights,
// weather, nights)
void EnhancedPost::apply_preset(int preset) {
  const bool perf = preset == 3;
  if (perf) {
    apply_preset(1);
    auto& s = m_settings;
    s.shadow_map = false;
    s.ssao = false;
    s.sun_shadows = false;
    s.water_reflections = false;
    s.far_view = false;
    s.light_spill = 0.f;
    s.lit_smoke = 0.f;
    s.cloud_shadows = false;
    s.relief = 0.f;
    g_enhanced_vegetation = 0;
    return;
  }
  auto& s = m_settings;
  const Settings d;
  s.enabled = preset > 0;
  s.taa = d.taa;
  s.shadow_map = d.shadow_map;
  s.sun_shadows = d.sun_shadows;
  s.ssao = d.ssao;
  s.bloom = d.bloom;
  s.lights = d.lights;
  s.lamp_strength = d.lamp_strength;
  s.fire = d.fire;
  s.geometry_normals = d.geometry_normals;
  s.prints = d.prints;
  s.dark_nights = d.dark_nights;
  s.cloud_shadows = d.cloud_shadows;
  s.lens_flare = d.lens_flare;
  s.desert_lamp = d.desert_lamp;
  s.light_spill = d.light_spill;
  s.lit_smoke = d.lit_smoke;
  s.far_view = d.far_view;
  s.materials = d.materials;
  s.relief = d.relief;
  s.ray_traced = all_on(preset) ? d.ray_traced : 0;
  s.rt_ao = all_on(preset) && d.rt_ao;
  s.water_reflections = d.water_reflections;
  s.water_swell = d.water_swell;
  bool all = preset == 2;
  s.volume = all && d.volume;
  s.shadow_casters = all && d.shadow_casters;
  s.gi = all && d.gi;
  s.shafts = all && d.shafts;
  s.dof = all && d.dof;
  s.motion_blur = all ? d.motion_blur : 0.f;
  s.light_shadow = all ? d.light_shadow : 0.f;
  s.water_refraction = all ? d.water_refraction : 0.f;
  s.water_caustics = all ? d.water_caustics : 0.f;
  s.area_grading = d.area_grading;
  s.clouds = d.clouds;
  s.blast_sparks = d.blast_sparks;
  s.weather = d.weather;
  s.weather_mix = d.weather_mix;
  s.vivid = d.vivid;
  s.soft_shadows = all && d.soft_shadows;
  s.cine_dof = all && d.cine_dof;
  g_enhanced_merc.enabled = preset > 0;
  g_enhanced_wind.enabled = preset > 0;
  g_enhanced_wind.push = preset > 0;
  g_enhanced_vegetation = preset;
}

// written only when something changed (the menu calls it every frame it is open)
void EnhancedPost::save_menu_settings() {
  auto text = menu_settings_json();
  if (text == m_saved_settings) {
    return;
  }
  m_saved_settings = text;
  try {
    auto path = file_util::get_user_settings_dir(g_game_version) / "enhanced-settings.json";
    file_util::create_dir_if_needed_for_file(path);
    file_util::write_text_file(path, text);
  } catch (std::exception& e) {
    lg::error("[enhanced] could not save the settings: {}", e.what());
  }
}

// A warm glow this frame: merged with the one seen at the same spot (a flame is often several
// glows, and its glow comes and goes), the memory entry if it has been there long enough to be a fire
// A fire has been glowing at its spot for a second, in most frames (a gun fired from where a
// guard stands glows there too, but in bursts)
bool EnhancedPost::is_fire(const FireMemory& f) const {
  double age = m_time - f.first;
  return age >= 1.0 && f.hits >= 0.6 * age * 60.0;
}

const EnhancedPost::FireMemory* EnhancedPost::note_fire_glow(const float* pos, float size) {
  constexpr float kSame = 1.5f * kMeter;
  for (auto& f : m_fires) {
    float dx = f.pos[0] - pos[0], dy = f.pos[1] - pos[1], dz = f.pos[2] - pos[2];
    if (dx * dx + dy * dy + dz * dz < kSame * kSame) {
      if (f.last != m_time) {
        f.hits++;
        f.last = m_time;
      }
      f.size = std::max(f.size * 0.98f, size);
      return is_fire(f) ? &f : nullptr;
    }
  }
  if (m_fires.size() < 64) {
    m_fires.push_back({{pos[0], pos[1], pos[2]}, size, m_time, m_time, 1});
  }
  return nullptr;
}

void EnhancedPost::draw_debug_window() {
  if (ImGui::TreeNode("Enhanced Post")) {
    ImGui::Checkbox("Enabled", &m_settings.enabled);
    ImGui::Checkbox("SSAO", &m_settings.ssao);
    ImGui::SliderFloat("SSAO radius (m)", &m_settings.ssao_radius, 0.2f, 5.f);
    ImGui::SliderFloat("SSAO intensity", &m_settings.ssao_intensity, 0.f, 3.f);
    ImGui::SliderFloat("RT AO length (m)", &m_settings.rt_ao_length, 0.5f, 8.f);
    ImGui::SliderFloat("RT AO strength", &m_settings.rt_ao_strength, 0.f, 2.f);
    ImGui::SliderFloat("RT AO distance (m)", &m_settings.rt_ao_distance, 10.f, 300.f);
    ImGui::SliderFloat("SSAO max distance (m)", &m_settings.ssao_max_distance, 10.f, 500.f);
    ImGui::Checkbox("SSAO debug view", &m_settings.ssao_debug);
    ImGui::Checkbox("Sun shadow map", &m_settings.shadow_map);
    ImGui::Checkbox("Characters and wind trees cast", &m_settings.shadow_casters);
    ImGui::SliderFloat("Shadow map strength", &m_settings.shadow_strength, 0.f, 1.f);
    ImGui::SliderFloat("Shadow map range (m)", &m_settings.shadow_range, 20.f, 200.f);
    ImGui::Checkbox("Sun shadows", &m_settings.sun_shadows);
    ImGui::SliderFloat("Sun shadow strength", &m_settings.sun_shadow_strength, 0.f, 1.f);
    ImGui::SliderFloat("Sun shadow length (m)", &m_settings.sun_shadow_length, 0.5f, 10.f);
    ImGui::Checkbox("Depth of field", &m_settings.dof);
    ImGui::SliderFloat("DoF start (m)", &m_settings.dof_start, 10.f, 500.f);
    ImGui::SliderFloat("DoF end (m)", &m_settings.dof_end, 20.f, 1000.f);
    ImGui::SliderFloat("DoF radius (px)", &m_settings.dof_radius, 0.f, 8.f);
    ImGui::Checkbox("God rays", &m_settings.shafts);
    ImGui::SliderFloat("God rays strength", &m_settings.shafts_strength, 0.f, 2.f);
    ImGui::SliderFloat("God rays threshold", &m_settings.shafts_threshold, 0.f, 1.f);
    ImGui::Checkbox("FXAA", &m_settings.fxaa);
    ImGui::Checkbox("TAA", &m_settings.taa);
    ImGui::SliderFloat("TAA history (still)", &m_settings.taa_history, 0.5f, 0.97f);
    ImGui::SliderFloat("TAA history (fast)", &m_settings.taa_history_fast, 0.3f, 0.95f);
    ImGui::SliderFloat("Fog density (/m)", &m_settings.fog_density, 0.f, 0.01f, "%.4f");
    ImGui::SliderFloat("Fog height falloff (/m)", &m_settings.fog_height, 0.f, 0.2f);
    ImGui::SliderFloat("Fog amount", &m_settings.fog_amount, 0.f, 1.f);
    ImGui::SliderFloat("Heat haze", &m_settings.haze_strength, 0.f, 0.005f, "%.4f");
    ImGui::Checkbox("Volumetric mist", &m_settings.volume);
    ImGui::SliderFloat("Mist density (/m)", &m_settings.mist_density, 0.f, 0.1f, "%.4f");
    ImGui::SliderFloat("Mist height (m)", &m_settings.mist_height, 0.5f, 30.f);
    ImGui::SliderFloat("Mist distance (m)", &m_settings.mist_distance, 20.f, 400.f);
    ImGui::SliderFloat("Mist ground below camera (m)", &m_settings.mist_ground, -5.f, 20.f);
    ImGui::SliderFloat("Mist patches", &m_settings.mist_noise, 0.f, 1.f);
    ImGui::SliderFloat("Mist sunlight", &m_settings.mist_sun, 0.f, 4.f);
    ImGui::SliderFloat("Mist forward scattering", &m_settings.mist_aniso, 0.f, 0.95f);
    ImGui::SliderFloat("Motion blur", &m_settings.motion_blur, 0.f, 1.f);
    ImGui::SliderFloat("Sun relight", &m_settings.relight, 0.f, 2.f);
    ImGui::Checkbox("Dynamic lights (glows)", &m_settings.lights);
    ImGui::SliderFloat("Dynamic light strength", &m_settings.light_strength, 0.f, 4.f);
    ImGui::SliderFloat("Dynamic light reach (x glow)", &m_settings.light_reach, 1.f, 12.f);
    ImGui::SliderFloat("Lamp strength", &m_settings.lamp_strength, 0.f, 4.f);
    ImGui::SliderFloat("Lamp reach (x game)", &m_settings.lamp_reach, 0.25f, 3.f);
    ImGui::SliderFloat("Dynamic light shadows", &m_settings.light_shadow, 0.f, 1.f);
    ImGui::SliderFloat("Explosions (flash, shockwave)", &m_settings.blast, 0.f, 3.f);
    ImGui::SliderFloat("Wake rings on the water", &m_settings.wake, 0.f, 3.f);
    ImGui::Checkbox("Fires (flicker, hot air, embers)", &m_settings.fire);
    ImGui::SliderFloat("Fire hot air", &m_settings.fire_haze, 0.f, 0.05f, "%.3f");
    ImGui::SliderFloat("Fire embers", &m_settings.fire_embers, 0.f, 3.f);
    ImGui::Checkbox("Indirect light (SSGI)", &m_settings.gi);
    ImGui::SliderFloat("Indirect strength", &m_settings.gi_strength, 0.f, 3.f);
    ImGui::SliderFloat("Indirect radius (m)", &m_settings.gi_radius, 0.5f, 10.f);
    ImGui::Checkbox("Wind on grass/leaves", &g_enhanced_wind.enabled);
    ImGui::SliderFloat("Wind strength", &g_enhanced_wind.strength, 0.f, 3.f);
    ImGui::Checkbox("Character rim/highlights", &g_enhanced_merc.enabled);
    ImGui::SliderFloat("Character rim light", &g_enhanced_merc.rim, 0.f, 1.5f);
    ImGui::SliderFloat("Character highlights", &g_enhanced_merc.spec, 0.f, 1.5f);
    ImGui::Checkbox("Water reflections", &m_settings.water_reflections);
    ImGui::SliderFloat("Water reflection strength", &m_settings.water_reflection_strength, 0.f, 1.f);
    ImGui::ColorEdit3("Deep water", m_settings.water_deep);
    ImGui::ColorEdit3("Shallow water", m_settings.water_shallow);
    ImGui::SliderFloat("Water color (ours vs game)", &m_settings.water_unify, 0.f, 1.f);
    ImGui::SliderFloat("Water swell", &m_settings.water_swell, 0.f, 3.f);
    ImGui::SliderFloat("Water refraction", &m_settings.water_refraction, 0.f, 0.05f);
    ImGui::SliderFloat("Water caustics", &m_settings.water_caustics, 0.f, 3.f);
    ImGui::SliderFloat("Sun glints", &m_settings.water_glint, 0.f, 3.f);
    ImGui::Checkbox("Bloom", &m_settings.bloom);
    ImGui::SliderFloat("Bloom threshold", &m_settings.bloom_threshold, 0.f, 1.f);
    ImGui::SliderFloat("Bloom knee", &m_settings.bloom_knee, 0.f, 1.f);
    ImGui::SliderFloat("Bloom strength", &m_settings.bloom_strength, 0.f, 2.f);
    ImGui::SliderFloat("Bloom radius", &m_settings.bloom_radius, 0.5f, 3.f);
    ImGui::SliderFloat("Saturation", &m_settings.saturation, 0.f, 2.f);
    ImGui::SliderFloat("Contrast", &m_settings.contrast, 0.f, 1.f);
    ImGui::SliderFloat("Warmth", &m_settings.warmth, -0.2f, 0.2f);
    ImGui::SliderFloat("Vignette", &m_settings.vignette, 0.f, 1.f);
    ImGui::SliderFloat("Sharpen", &m_settings.sharpen, 0.f, 1.f);
    ImGui::Checkbox("Grade per area", &m_settings.area_grading);
    ImGui::Text("area %s, night %.2f, rain %.2f, dust %.2f, focus %.1f m", g_world.zones[m_zone].name.c_str(), m_night, m_rain,
                m_dust, m_focus / kMeter);
    ImGui::Text("wet %.2f, storm %.2f, mist %.2f, sun after rain %.2f, state %d, wind %.1f m/s",
                m_wet, m_storm, m_mist, m_clear, m_weather_state, m_wind_speed);
    ImGui::Text("season %s, spell %s, roof over Jak %.2f", kSeasonNames[m_season], kSpellNames[m_spell], m_cover);
    ImGui::Checkbox("Soft shadows", &m_settings.soft_shadows);
    ImGui::SliderFloat("Shadow softness (/m)", &m_settings.shadow_softness, 0.f, 0.08f, "%.3f");
    ImGui::Checkbox("Clouds", &m_settings.clouds);
    ImGui::SliderFloat("Cloud amount", &m_settings.cloud_amount, 0.f, 2.f);
    ImGui::SliderFloat("Vivid colors##debug", &m_settings.vivid, 0.f, 1.f);
    weather_menu(m_settings, "Weather##debug");
    ImGui::SliderFloat("Rain strength", &m_settings.rain_strength, 0.f, 2.f);
    ImGui::SliderFloat("Dust strength", &m_settings.dust_strength, 0.f, 2.f);
    ImGui::Checkbox("Cutscene depth of field", &m_settings.cine_dof);
    ImGui::SliderFloat("Cutscene aperture", &m_settings.cine_aperture, 0.f, 3.f);
    ImGui::SliderFloat("Explosion sparks", &m_settings.blast_sparks, 0.f, 3.f);
    ImGui::TreePop();
  }
}
