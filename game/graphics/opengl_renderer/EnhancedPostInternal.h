#pragma once

// What EnhancedPost's files share (EnhancedPost.cpp, EnhancedPostScene.cpp, EnhancedPostLight.cpp,
// EnhancedPostPasses.cpp, EnhancedPostMenu.cpp; one file before, split so an edit recompiles less):
// the world's look and weather (world.json), the menu's look and widgets, the hand-placed lights
// and beds, the sun's direction. Inline, so each is one object shared by all of them.

#include "game/graphics/opengl_renderer/EnhancedPost.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <ctime>
#include <random>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/enhanced_light.h"
#include "common/util/json_util.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/EnhancedEditor.h"
#include "game/graphics/opengl_renderer/EnhancedTune.h"
#include "game/graphics/opengl_renderer/loader/EnhancedGrass.h"
#include "game/graphics/opengl_renderer/loader/EnhancedRayTrace.h"
#include "game/graphics/opengl_renderer/loader/EnhancedScenery.h"
#include "game/graphics/opengl_renderer/loader/EnhancedTerrain.h"
#include "game/graphics/opengl_renderer/loader/EnhancedSmoothMerc.h"
#include "game/graphics/opengl_renderer/loader/EnhancedVegetation.h"
#include "game/graphics/opengl_renderer/loader/common.h"

#include "game/graphics/pipelines/opengl.h"
#include "game/kernel/common/kboot.h"
#include "game/runtime.h"
#include "game/sound/common/enhanced_ambience.h"

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_internal.h"
#include "third-party/stb_image/stb_image.h"
#include "game/graphics/opengl_renderer/loader/EnhancedMaterials.h"

namespace ImGui {
extern ImFont* g_ui_font_bold;  // debug_gui.cpp: the menu title's font
}

// JAK_ENHANCED_PROFILE=1: each pass also gets a node in the renderer's profile (CPU time); the GPU
// time of each is the pass timers' log line below
inline bool enh_profiling() {
  static const bool on = std::getenv("JAK_ENHANCED_PROFILE") != nullptr;
  return on;
}
// Otherwise each pass is timed on the GPU without waiting: two timestamp queries a pass, in a ring
// of 4 frames, read back when they're done (they nest; ~0.01 ms in all). EnhancedPost::pass_times
// gives the averages ("[enhanced] passes ..." in the log every 5 s). tuning.json
// "debug": {"pass_timers": 0} turns them off.
namespace enh_post {
struct PassTimer {
  const char* name;
  GLuint q[4][2] = {};
  bool used[4] = {};
  double sum_ms = 0.0;
  int n = 0;
  explicit PassTimer(const char* nm);
};
inline std::vector<PassTimer*>& pass_timers() {
  static std::vector<PassTimer*> all;
  return all;
}
inline PassTimer::PassTimer(const char* nm) : name(nm) {
  pass_timers().push_back(this);
}
inline unsigned& pass_timer_frame() {
  static unsigned f = 0;
  return f;
}
bool pass_timers_on();
struct PassTimerScope {
  PassTimer* t = nullptr;
  int slot = 0;
  explicit PassTimerScope(PassTimer& timer) {
    if (!pass_timers_on()) {
      return;
    }
    t = &timer;
    if (!t->q[0][0]) {
      glGenQueries(8, &t->q[0][0]);
    }
    slot = pass_timer_frame() & 3;
    if (t->used[slot]) {
      // (4 frames old: done by now nearly always; if not, that sample is dropped, never waited for)
      GLint ready = 0;
      glGetQueryObjectiv(t->q[slot][1], GL_QUERY_RESULT_AVAILABLE, &ready);
      if (ready) {
        GLuint64 a = 0, b = 0;
        glGetQueryObjectui64v(t->q[slot][0], GL_QUERY_RESULT, &a);
        glGetQueryObjectui64v(t->q[slot][1], GL_QUERY_RESULT, &b);
        if (b > a) {
          t->sum_ms += (double)(b - a) * 1e-6;
          t->n++;
        }
      }
      t->used[slot] = false;
    }
    glQueryCounter(t->q[slot][0], GL_TIMESTAMP);
  }
  ~PassTimerScope() {
    if (t) {
      glQueryCounter(t->q[slot][1], GL_TIMESTAMP);
      t->used[slot] = true;
    }
  }
  PassTimerScope(const PassTimerScope&) = delete;
  PassTimerScope& operator=(const PassTimerScope&) = delete;
};
}  // namespace enh_post

// The wrapped call gets the pass's own profiler node as `prof`, so a timed pass inside a timed pass
// becomes its child: adding siblings to the outer node instead grew the vector that holds the open
// node (dangling pointer, "finish() not called"). GPU time comes from the timestamp pair, never a
// glFinish (which changed the frame being measured).
#define ENH_TIMED(name, ...)                                   \
  {                                                            \
    static enh_post::PassTimer enh_pass_timer(name);           \
    enh_post::PassTimerScope enh_pass_scope(enh_pass_timer);   \
    if (enh_profiling()) {                                     \
      auto enh_timed_node = prof.make_scoped_child(name);      \
      [[maybe_unused]] auto& prof = enh_timed_node;            \
      __VA_ARGS__;                                             \
    } else {                                                   \
      __VA_ARGS__;                                             \
    }                                                          \
  }

namespace enh_post {
// The world's look and weather for the three games: game/assets/enhanced/world.json, reloaded while
// the game runs whenever it's saved (see the file for what each value does)
struct ZoneLook {
  float lift[3], gain[3], saturation, contrast, warmth, clouds, sand, sun_haze, rock_detail;
  float golden = 0.f, sun_yaw = 0.f, sun_max_elev = 90.f, light_yaw = 0.f, light_max_elev = 90.f;
  float morning_yaw = 0.f;  // the sun's path turned further in the morning (degrees)
  float day_sky = 0.f;      // the clear day sky (pink toward the morning sun) and its clouds (0..1)
  float rock_sun = 0.f;     // the sun's own direction lighting rock and cliffs (0..1)
  float wall_sun = 0.f;     // and, milder, the walls and the rest of the plain scenery (0..1)
  float headlights = 1.f;   // the vehicles' headlights: their light, beams and glare (times)
  float glows = 1.f;        // the light the game's glows cast (engines, lamps' halos; times)
  float lamps = 1.f;        // the levels' own lamps and the fires they place (times)
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
  // how much metal and gloss reflect (times) and how rough they are (1 = as made)
  float reflect[3] = {1.f, 1.f, 1.f};
  float rough[3] = {1.f, 1.f, 1.f};
  std::vector<WorldZone> zones;  // [0]: "other", for the levels in no zone
  std::vector<std::pair<std::string, int>> prefixes[3];
  // levels under a roof (temples, palace halls): their fires burn at night strength by day
  std::vector<std::string> enclosed[3];
  Season seasons[4];
};
inline World g_world;

// Enhanced's own fog (the distance fog, the ground mist, the morning fog) is off for now: it gave
// trouble in all three games. Off, the weather never brings it and the menu doesn't offer it; the
// game's own fog is untouched.
constexpr bool kFogOff = true;

// the lamps' and fires' light in the air (volume local lights M1): the volume pass scatters the
// nearest lights (g_enhanced_smoke_lights, last frame's, in world space) in a thin air of its own,
// with no mist and no fog. volume.local_lights 1: on (off until proven), volume.light_air: the air's
// density (/m), volume.light_gain: its brightness
inline bool volume_lights_on() {
  static const EnhancedTune t_on("volume.local_lights", 0.f);
  return t_on > 0.5f && g_enhanced_smoke_lights.count > 0;
}

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

// The Enhanced menu's look (F1): a dark, slightly purple panel with a glowing amber edge, switches
// for the on/off settings, filled amber sliders and rows of choices drawn as one bar. Only that menu:
// draw_menu pushes the style and pops it on the way out, and the helpers below draw ImGui's own
// widgets elsewhere (weather_menu is also in the debug window)
inline bool g_menu_styled = false;
// one theme a game: Jak 1's bright blue, yellow and orange, Jak 2's city greys with a cool steel
// blue and a guard's red, Jak 3's desert sand and dusk. JAK_ENHANCED_MENU_THEME=1..3 picks one
// whatever the game (screenshots)
struct MenuTheme {
  ImVec4 accent, accent2, highlight, second, ink;  // second: hover, separators, tooltips
  ImVec4 text, text_disabled, panel, popup, title_band;
  ImVec4 frame, frame_hovered, frame_active, button, button_hovered;
};
const MenuTheme kMenuThemes[3] = {
    // Jak 1: a deep blue panel, sunny yellow and orange, sky blue
    {{1.00f, 0.80f, 0.12f, 1.f}, {1.00f, 0.50f, 0.10f, 1.f}, {1.00f, 0.93f, 0.45f, 1.f},
     {0.32f, 0.70f, 1.00f, 1.f}, {0.05f, 0.07f, 0.16f, 1.f},
     {0.95f, 0.97f, 1.00f, 1.f}, {0.55f, 0.66f, 0.82f, 1.f}, {0.04f, 0.08f, 0.18f, 1.f},
     {0.06f, 0.11f, 0.24f, 0.97f}, {0.07f, 0.16f, 0.36f, 0.85f},
     {0.09f, 0.18f, 0.36f, 0.90f}, {0.14f, 0.30f, 0.55f, 0.95f}, {0.18f, 0.38f, 0.68f, 1.f},
     {0.10f, 0.20f, 0.40f, 0.95f}, {0.20f, 0.45f, 0.80f, 1.f}},
    // Jak 2: slate greys, steel blue, a red edge
    {{0.62f, 0.80f, 0.92f, 1.f}, {0.86f, 0.26f, 0.22f, 1.f}, {0.88f, 0.94f, 1.00f, 1.f},
     {0.90f, 0.32f, 0.28f, 1.f}, {0.06f, 0.07f, 0.09f, 1.f},
     {0.90f, 0.92f, 0.94f, 1.f}, {0.55f, 0.58f, 0.62f, 1.f}, {0.07f, 0.08f, 0.09f, 1.f},
     {0.10f, 0.11f, 0.13f, 0.97f}, {0.16f, 0.18f, 0.21f, 0.85f},
     {0.17f, 0.19f, 0.22f, 0.90f}, {0.26f, 0.29f, 0.33f, 0.95f}, {0.33f, 0.37f, 0.42f, 1.f},
     {0.19f, 0.21f, 0.24f, 0.95f}, {0.36f, 0.42f, 0.50f, 1.f}},
    // Jak 3: the wasteland's burnt orange and rust red on scorched brown, sun-bleached sand, and
    // light eco's cyan glow
    {{1.00f, 0.47f, 0.10f, 1.f}, {0.86f, 0.18f, 0.08f, 1.f}, {1.00f, 0.80f, 0.42f, 1.f},
     {0.38f, 0.86f, 1.00f, 1.f}, {0.12f, 0.05f, 0.02f, 1.f},
     {0.98f, 0.92f, 0.82f, 1.f}, {0.66f, 0.55f, 0.45f, 1.f}, {0.09f, 0.05f, 0.03f, 1.f},
     {0.12f, 0.07f, 0.04f, 0.97f}, {0.27f, 0.08f, 0.03f, 0.85f},
     {0.22f, 0.12f, 0.07f, 0.90f}, {0.34f, 0.18f, 0.10f, 0.95f}, {0.44f, 0.22f, 0.12f, 1.f},
     {0.26f, 0.14f, 0.08f, 0.95f}, {0.58f, 0.26f, 0.10f, 1.f}},
};
inline MenuTheme g_menu_theme = kMenuThemes[2];
// (the theme's colors under the names the widgets use)
inline ImVec4 kMenuAmber, kMenuOrange, kMenuYellow, kMenuPurple, kMenuInk;
inline int game_index();

inline void pick_menu_theme() {
  static const char* forced = std::getenv("JAK_ENHANCED_MENU_THEME");
  const int i = forced ? std::clamp(std::atoi(forced) - 1, 0, 2) : game_index();
  g_menu_theme = kMenuThemes[i];
  kMenuAmber = g_menu_theme.accent;
  kMenuOrange = g_menu_theme.accent2;
  kMenuYellow = g_menu_theme.highlight;
  kMenuPurple = g_menu_theme.second;
  kMenuInk = g_menu_theme.ink;  // text on a picked choice
}

inline ImVec4 menu_rgba(const ImVec4& c, float a) {
  return ImVec4(c.x, c.y, c.z, a);
}
// (through GetColorU32: faded like the rest inside BeginDisabled)
inline ImU32 menu_col(const ImVec4& c, float a = 1.f) {
  return ImGui::GetColorU32(menu_rgba(c, c.w * a));
}

// the style vars and colors of the menu, popped when it goes out of scope (every way out of draw_menu)
struct MenuStyle {
  int vars = 0, colors = 0;
  MenuStyle() {
    pick_menu_theme();
    const MenuTheme& t = g_menu_theme;
    auto var = [&](ImGuiStyleVar v, float f) {
      ImGui::PushStyleVar(v, f);
      vars++;
    };
    auto var2 = [&](ImGuiStyleVar v, ImVec2 f) {
      ImGui::PushStyleVar(v, f);
      vars++;
    };
    auto col = [&](ImGuiCol c, ImVec4 v) {
      ImGui::PushStyleColor(c, v);
      colors++;
    };
    var(ImGuiStyleVar_WindowRounding, 10.f);
    var(ImGuiStyleVar_WindowBorderSize, 1.5f);
    var2(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    var(ImGuiStyleVar_ChildRounding, 6.f);
    var(ImGuiStyleVar_PopupRounding, 6.f);
    var(ImGuiStyleVar_PopupBorderSize, 1.f);
    var(ImGuiStyleVar_FrameRounding, 5.f);
    var(ImGuiStyleVar_FrameBorderSize, 0.f);
    var2(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    var2(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    var(ImGuiStyleVar_GrabRounding, 5.f);
    var(ImGuiStyleVar_GrabMinSize, 12.f);
    var(ImGuiStyleVar_ScrollbarRounding, 6.f);
    var(ImGuiStyleVar_SeparatorTextBorderSize, 2.f);
    col(ImGuiCol_Text, t.text);
    col(ImGuiCol_TextDisabled, t.text_disabled);
    col(ImGuiCol_WindowBg, t.panel);
    col(ImGuiCol_PopupBg, t.popup);
    col(ImGuiCol_Border, menu_rgba(kMenuAmber, 0.85f));
    col(ImGuiCol_FrameBg, t.frame);
    col(ImGuiCol_FrameBgHovered, t.frame_hovered);
    col(ImGuiCol_FrameBgActive, t.frame_active);
    col(ImGuiCol_Button, t.button);
    col(ImGuiCol_ButtonHovered, t.button_hovered);
    col(ImGuiCol_ButtonActive, kMenuOrange);
    col(ImGuiCol_Header, menu_rgba(kMenuOrange, 0.28f));
    col(ImGuiCol_HeaderHovered, menu_rgba(kMenuPurple, 0.50f));
    col(ImGuiCol_HeaderActive, menu_rgba(kMenuPurple, 0.70f));
    col(ImGuiCol_CheckMark, kMenuYellow);
    col(ImGuiCol_SliderGrab, kMenuAmber);
    col(ImGuiCol_SliderGrabActive, kMenuYellow);
    col(ImGuiCol_Separator, menu_rgba(kMenuPurple, 0.55f));
    col(ImGuiCol_SeparatorHovered, kMenuPurple);
    col(ImGuiCol_SeparatorActive, kMenuPurple);
    col(ImGuiCol_ScrollbarBg, ImVec4(0.f, 0.f, 0.f, 0.f));
    col(ImGuiCol_ScrollbarGrab, menu_rgba(kMenuAmber, 0.45f));
    col(ImGuiCol_ScrollbarGrabHovered, menu_rgba(kMenuAmber, 0.70f));
    col(ImGuiCol_ScrollbarGrabActive, kMenuAmber);
    col(ImGuiCol_NavCursor, kMenuPurple);
    col(ImGuiCol_TextSelectedBg, menu_rgba(kMenuPurple, 0.45f));
    g_menu_styled = true;
  }
  ~MenuStyle() {
    ImGui::PopStyleColor(colors);
    ImGui::PopStyleVar(vars);
    g_menu_styled = false;
  }
};

// an on/off switch where the menu had a checkbox: the same id, the same answer (true when flipped),
// the label clickable too; a checkbox outside the menu
inline bool menu_toggle(const char* label, bool* v) {
  if (!g_menu_styled) {
    return ImGui::Checkbox(label, v);
  }
  const ImGuiStyle& st = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float sw_h = std::round(h * 0.68f), sw_w = std::round(sw_h * 1.85f);
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(label, ImVec2(sw_w + st.ItemInnerSpacing.x * 2 + text.x, h));
  if (pressed) {
    *v = !*v;
  }
  const ImGuiID id = ImGui::GetItemID();
  const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  // the knob slides over a few frames
  ImGuiStorage* store = ImGui::GetStateStorage();
  float t = store->GetFloat(id, *v ? 1.f : 0.f);
  const float target = *v ? 1.f : 0.f;
  const float step = ImGui::GetIO().DeltaTime * 10.f;
  t = t < target ? std::min(target, t + step) : std::max(target, t - step);
  store->SetFloat(id, t);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 a(pos.x, pos.y + (h - sw_h) * 0.5f), b(a.x + sw_w, a.y + sw_h);
  const float r = sw_h * 0.5f;
  const ImVec4 off(0.20f, 0.13f, 0.29f, 1.f);
  const ImVec4 on = held ? kMenuYellow : hovered ? kMenuAmber : kMenuOrange;
  const ImVec4 track(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1.f);
  dl->AddRectFilled(a, b, menu_col(track), r);
  dl->AddRect(a, b, hovered ? menu_col(kMenuPurple) : menu_col(kMenuPurple, 0.45f + 0.3f * (1.f - t)), r, 0,
              1.f);
  const ImVec2 knob(a.x + r + (sw_w - sw_h) * t, a.y + r);
  dl->AddCircleFilled(knob, r - 2.5f,
                      menu_col(ImVec4(0.72f + 0.28f * t, 0.66f + 0.29f * t, 0.80f - 0.02f * t, 1.f)));
  dl->AddText(ImVec2(b.x + st.ItemInnerSpacing.x * 2, pos.y + st.FramePadding.y),
              ImGui::GetColorU32(ImGuiCol_Text), label, ImGui::FindRenderedTextEnd(label));
  ImGui::RenderNavCursor(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), id);
  return pressed;
}

// one choice of a row drawn as one bar (place 0: the first, 1: between, 2: the last); the picked
// one amber. Drawn next to each other with SameLine(0, 0)
inline bool menu_segment(const char* label, bool picked, float width, int place) {
  const float h = ImGui::GetFrameHeight();
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, h));
  const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 b(pos.x + width, pos.y + h);
  const ImDrawFlags corners = place == 0   ? ImDrawFlags_RoundCornersLeft
                              : place == 2 ? ImDrawFlags_RoundCornersRight
                                           : ImDrawFlags_RoundCornersNone;
  const float rounding = ImGui::GetStyle().FrameRounding;
  ImVec4 fill = picked ? (hovered ? kMenuYellow : kMenuAmber)
                : held ? kMenuOrange
                : hovered ? g_menu_theme.button_hovered
                          : menu_rgba(g_menu_theme.frame, 0.95f);
  dl->AddRectFilled(pos, b, menu_col(fill), rounding, corners);
  if (picked) {
    // a brighter line on top, as lit from above
    dl->AddRectFilled(ImVec2(pos.x + 2, pos.y + 1), ImVec2(b.x - 2, pos.y + 3), menu_col(kMenuYellow, 0.9f),
                      1.f);
  }
  if (place != 0) {
    dl->AddLine(ImVec2(pos.x, pos.y + 4), ImVec2(pos.x, b.y - 4), menu_col(kMenuPurple, 0.6f));
  }
  dl->AddRect(pos, b, menu_col(kMenuAmber, picked ? 0.9f : 0.30f), rounding, corners, 1.f);
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  dl->AddText(ImVec2(pos.x + (width - text.x) * 0.5f, pos.y + (h - text.y) * 0.5f),
              picked && !held ? menu_col(kMenuInk) : ImGui::GetColorU32(ImGuiCol_Text), label,
              ImGui::FindRenderedTextEnd(label));
  ImGui::RenderNavCursor(ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()), ImGui::GetItemID());
  return pressed;
}

// a slider whose track fills with amber up to its value (the slider draws over it, its own frame
// left clear); ImGui's plain slider outside the menu
template <typename F>
bool menu_slider(float frac, F&& slider) {
  if (!g_menu_styled) {
    return slider();
  }
  const float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  for (ImGuiCol c : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive}) {
    ImGui::PushStyleColor(c, ImVec4(0.f, 0.f, 0.f, 0.f));
  }
  const bool changed = slider();
  ImGui::PopStyleColor(3);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  dl->ChannelsSetCurrent(0);
  const ImGuiStyle& st = ImGui::GetStyle();
  const ImVec2 a = ImGui::GetItemRectMin(), b(a.x + w, a.y + h);
  dl->AddRectFilled(a, b,
                    active    ? ImGui::GetColorU32(ImGuiCol_FrameBgActive)
                    : hovered ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered)
                              : ImGui::GetColorU32(ImGuiCol_FrameBg),
                    st.FrameRounding);
  // (to the grab's middle: it moves inside the frame less its size and 2 pixels each side)
  const float x = a.x + 2.f + st.GrabMinSize * 0.5f + std::clamp(frac, 0.f, 1.f) * (w - 4.f - st.GrabMinSize);
  dl->AddRectFilled(a, ImVec2(x, b.y), menu_col(kMenuOrange, active ? 0.55f : 0.40f), st.FrameRounding,
                    ImDrawFlags_RoundCornersLeft);
  dl->AddRectFilled(ImVec2(a.x + 1, b.y - 3), ImVec2(x, b.y - 1), menu_col(kMenuAmber, 0.9f), 1.f);
  dl->ChannelsMerge();
  return changed;
}
inline bool menu_slider_float(const char* label, float* v, float lo, float hi, const char* format) {
  return menu_slider((*v - lo) / (hi - lo),
                     [&] { return ImGui::SliderFloat(label, v, lo, hi, format); });
}
inline bool menu_slider_int(const char* label, int* v, int lo, int hi, const char* format = "%d") {
  return menu_slider(float(*v - lo) / float(hi - lo),
                     [&] { return ImGui::SliderInt(label, v, lo, hi, format); });
}

// a section of the menu: an amber bar on its left (purple when the pointer is on it)
inline bool menu_section(const char* label, ImGuiTreeNodeFlags flags = 0) {
  const bool open = ImGui::CollapsingHeader(label, flags);
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool hovered = ImGui::IsItemHovered();
  dl->AddRectFilled(a, ImVec2(a.x + 4.f, b.y), menu_col(hovered ? kMenuPurple : kMenuAmber), 2.f,
                    ImDrawFlags_RoundCornersLeft);
  dl->AddLine(ImVec2(a.x, b.y), b, menu_col(hovered ? kMenuPurple : kMenuOrange, 0.6f));
  return open;
}

// a heading inside a section, in amber over a purple line
inline void menu_subhead(const char* label) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMenuAmber);
  ImGui::SeparatorText(label);
  ImGui::PopStyleColor();
}

// the title bar: JAK ENHANCED in bold, the credit and which keys close it smaller, on a band with
// an amber-to-purple line under it (drawn after the text, under it, through a second channel)
inline void menu_title(const char* closes) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float size = ImGui::GetStyle().FontSizeBase;
  ImGui::PushFont(ImGui::g_ui_font_bold, size * 1.75f);
  const ImVec2 title_pos = ImGui::GetCursorScreenPos();
  const char* title = "JAK ENHANCED";
  const ImVec2 title_size = ImGui::CalcTextSize(title);
  // a soft orange glow behind the letters
  for (int i = 0; i < 4; i++) {
    const float dx = i == 0 ? -1.5f : i == 1 ? 1.5f : 0.f, dy = i == 2 ? -1.5f : i == 3 ? 1.5f : 0.f;
    dl->AddText(ImVec2(title_pos.x + dx, title_pos.y + dy), menu_col(kMenuOrange, 0.35f), title);
  }
  ImGui::TextColored(kMenuYellow, "%s", title);
  ImGui::PopFont();
  ImGui::PushFont(nullptr, size * 0.875f);
  const float small_h = ImGui::GetTextLineHeight();
  const float base = title_pos.y + title_size.y - small_h - 4.f;
  ImGui::SameLine(0, 12);
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, base));
  ImGui::TextColored(menu_rgba(kMenuPurple, 1.f), "TRILOGY");
  ImGui::SameLine(0, 6);
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, base));
  ImGui::TextDisabled("by stichlola");
  // the keys that close it, at the right edge: its room kept after the credit (so the window, which
  // fits its contents, doesn't chase its own width), the text drawn at the edge
  const ImVec2 hint = ImGui::CalcTextSize(closes);
  ImGui::SameLine(0, 32);
  const ImVec2 hint_pos(ImGui::GetCursorScreenPos().x, base);
  ImGui::SetCursorScreenPos(hint_pos);
  ImGui::Dummy(hint);
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
  dl->AddText(ImVec2(std::max(hint_pos.x, right - hint.x), base), ImGui::GetColorU32(ImGuiCol_TextDisabled), closes);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(start.x, title_pos.y + title_size.y + 4.f));
  const float band_bottom = ImGui::GetCursorScreenPos().y + 4.f;
  ImGui::Dummy(ImVec2(0, 8.f));
  dl->ChannelsSetCurrent(0);
  const ImVec2 wp = ImGui::GetWindowPos();
  const float ww = ImGui::GetWindowWidth();
  const float rounding = ImGui::GetStyle().WindowRounding;
  dl->AddRectFilled(ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + ww - 1, band_bottom),
                    ImGui::GetColorU32(g_menu_theme.title_band), rounding - 1.f,
                    ImDrawFlags_RoundCornersTop);
  dl->AddRectFilledMultiColor(ImVec2(wp.x + 1, band_bottom - 2.f), ImVec2(wp.x + ww - 1, band_bottom),
                              menu_col(kMenuAmber), menu_col(kMenuPurple), menu_col(kMenuPurple),
                              menu_col(kMenuAmber));
  dl->ChannelsMerge();
}

// the glow round the panel's amber edge, behind the windows
inline void menu_glow() {
  const ImVec2 a = ImGui::GetWindowPos();
  const ImVec2 b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
  const float rounding = ImGui::GetStyle().WindowRounding;
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  for (int i = 1; i <= 6; i++) {
    const float d = i * 1.5f;
    bg->AddRect(ImVec2(a.x - d, a.y - d), ImVec2(b.x + d, b.y + d),
                ImGui::GetColorU32(menu_rgba(kMenuOrange, 0.16f * (1.f - i / 7.f))), rounding + d, 0, 2.f);
  }
}

// the weather in a menu: the presets, and under them the mix (moving a slider makes it "Custom")
inline void weather_menu(EnhancedPost::Settings& s, const char* id) {
  ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo(id, kWeatherPresets[std::clamp(s.weather, 0, kWeatherCount - 1)].name)) {
    for (int i = 0; i < kWeatherCount; i++) {
      if (kFogOff && std::strstr(kWeatherPresets[i].name, "fog")) {
        continue;
      }
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
    if (kFogOff && (i == 2 || i == 3)) {
      continue;  // fog, ground mist
    }
    ImGui::SetNextItemWidth(160);
    float v = s.weather_mix[i];
    const float lo = i == 8 ? -0.01f : 0.f;  // wet ground below 0: as wet as it rains
    if (menu_slider_float(fmt::format("{}##{}", names[i], id).c_str(), &v, lo, top[i],
                          i == 8 && v < 0.f ? "with the rain" : "%.2f")) {
      s.weather_mix[i] = v;
      s.weather = kWeatherCustom;
    }
  }
  ImGui::Unindent();
}

inline int game_index() {
  return g_game_version == GameVersion::Jak1 ? 0 : g_game_version == GameVersion::Jak2 ? 1 : 2;
}

inline float game_gi_scale() {
  return g_world.gi[game_index()];
}
inline float game_lamp_scale() {
  return g_world.lamps[game_index()];
}

inline fs::path world_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "world.json";
}

inline WorldZone neutral_zone() {
  WorldZone z;
  z.name = "other";
  for (auto& l : z.look) {
    l = {{0, 0, 0}, {1, 1, 1}, 1.f, 0.f, 0.f, 0.4f, 0.f, 0.f, 0.f};
  }
  return z;
}

// reads world.json; on an error the world stays as it was (the log says why)
inline void load_world() {
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
        w.reflect[g] = t.value("reflect", w.reflect[g]);
        w.rough[g] = t.value("rough", w.rough[g]);
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
      out.sand = l.value("sand", 0.f);
      out.sun_haze = l.value("sun_haze", 0.f);
      out.rock_detail = l.value("rock_detail", 0.f);
      out.headlights = l.value("headlights", 1.f);
      out.glows = l.value("glows", 1.f);
      out.lamps = l.value("lamps", 1.f);
      // (as the day's unless the night says otherwise)
      out.golden = l.value("golden", out.golden);
      out.sun_yaw = l.value("sun_yaw", out.sun_yaw);
    out.morning_yaw = l.value("morning_yaw", out.morning_yaw);
      out.sun_max_elev = l.value("sun_max_elev", out.sun_max_elev);
      out.light_yaw = l.value("light_yaw", out.light_yaw);
      out.light_max_elev = l.value("light_max_elev", out.light_max_elev);
      out.day_sky = l.value("day_sky", out.day_sky);
      out.rock_sun = l.value("rock_sun", out.rock_sun);
  out.wall_sun = l.value("wall_sun", out.wall_sun);
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
      if (j.contains("enclosed") && j["enclosed"].contains(games[g])) {
        for (const auto& prefix : j["enclosed"][games[g]]) {
          w.enclosed[g].push_back(prefix.get<std::string>());
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
using ManualLight = EnhancedLight;
inline std::vector<ManualLight> g_manual_lights[3];


inline fs::path manual_lights_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "lights.json";
}

inline void load_manual_lights() {
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
        l.intensity = e.value("intensity", 1.f);
        g_manual_lights[g].push_back(l);
      }
    }
    lg::info("[enhanced] lights.json: {} lights", g_manual_lights[game_index()].size());
  } catch (const std::exception& e) {
    lg::error("[enhanced] lights.json not loaded: {}", e.what());
  }
}

inline void save_manual_lights() {
  if (g_enhanced_lights_private) {
    return;
  }
  nlohmann::json j = nlohmann::json::object();
  const char* games[3] = {"jak1", "jak2", "jak3"};
  for (int g = 0; g < 3; g++) {
    auto arr = nlohmann::json::array();
    for (const auto& l : g_manual_lights[g]) {
      // (as doubles: a float written out comes back with all its binary digits)
      auto r = [](float v) { return std::round((double)v * 100.0) / 100.0; };
      arr.push_back({{"note", l.note},
                     {"pos", {r(l.pos[0] / 4096.f), r(l.pos[1] / 4096.f), r(l.pos[2] / 4096.f)}},
                     {"radius", r(l.radius / 4096.f)},
                     {"color", {r(l.color[0]), r(l.color[1]), r(l.color[2])}},
                     {"flicker", l.flicker},
                     {"night", l.night},
                     {"bulb", l.bulb},
                     {"intensity", r(l.intensity)}});
    }
    if (!arr.empty()) {
      j[games[g]] = arr;
    }
  }
  file_util::write_text_file(manual_lights_path(), j.dump(2));
}

// beds.json: beds for the nap, per game ({"jak3": [[x, y, z], ...]}, meters: where Jak stood on it)
inline std::vector<std::array<float, 3>> g_beds[3];

inline fs::path beds_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "beds.json";
}

inline void publish_beds() {
  std::lock_guard<std::mutex> lock(g_enhanced_beds.mutex);
  g_enhanced_beds.pos = g_beds[game_index()];
}

inline void load_beds() {
  for (auto& b : g_beds) {
    b.clear();
  }
  std::error_code ec;
  if (fs::exists(beds_path(), ec)) {
    try {
      auto j = parse_commented_json(file_util::read_text_file(beds_path()), "beds.json");
      const char* games[3] = {"jak1", "jak2", "jak3"};
      for (int g = 0; g < 3; g++) {
        if (j.contains(games[g])) {
          for (const auto& e : j[games[g]]) {
            g_beds[g].push_back({e[0].get<float>() * 4096.f, e[1].get<float>() * 4096.f,
                                 e[2].get<float>() * 4096.f});
          }
        }
      }
    } catch (const std::exception& e) {
      lg::error("[enhanced] beds.json not loaded: {}", e.what());
    }
  }
  publish_beds();
}

inline void save_beds() {
  nlohmann::json j = nlohmann::json::object();
  const char* games[3] = {"jak1", "jak2", "jak3"};
  for (int g = 0; g < 3; g++) {
    auto arr = nlohmann::json::array();
    for (const auto& b : g_beds[g]) {
      auto r = [](float v) { return std::round(v / 4096.f * 100.f) / 100.f; };
      arr.push_back({r(b[0]), r(b[1]), r(b[2])});
    }
    if (!arr.empty()) {
      j[games[g]] = arr;
    }
  }
  file_util::write_text_file(beds_path(), j.dump(2));
  publish_beds();
}

// once a second: load world.json again if it was saved since
inline void poll_world() {
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
    load_beds();
  }
}

// the zone of the level Jak is in, from the start of its name (the current game's list)
inline int zone_of(const std::string& level) {
  for (const auto& [prefix, zone] : g_world.prefixes[game_index()]) {
    if (level.rfind(prefix, 0) == 0) {
      return zone;
    }
  }
  return 0;
}

inline GLuint make_1x1(u32 rgba) {
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

// the area's turn of the sun's path ("sun_yaw" in world.json, degrees), eased (update_scene)
inline float g_sun_yaw = 0.f;
// and in the morning further by the area's "morning_yaw", by the morning's amount (update_scene)
inline float g_morning_yaw = 0.f;
// JAK_ENHANCED_TUNE's third number: the morning's turn instead of the area's (NAN: none)
inline float g_tune_morning_yaw = NAN;
// how high the sun may stand (degrees): the area's "sun_max_elev" by the late afternoon's amount,
// so the golden hour's sun is low and grazing (update_scene)
inline float g_sun_max_elev = 90.f;
// how much farther the light that falls on the scene is turned than the sun seen in the sky
// ("light_yaw", degrees): a painter's key light, so the sun can sit behind a mountain while its
// light still rakes the scene from the side
inline float g_light_yaw = 0.f;
// and how high that light may stand ("light_max_elev", degrees; by the late afternoon's amount as
// the sun's): higher than a sun sitting on a mountain, so the open ground still gets its light
inline float g_light_max_elev = 90.f;

// test switch: JAK_ENHANCED_TUNE=<file> reads "light_yaw light_max_elev" from it every second
// (tuning in one run); tuning.json's "light": {"yaw", "max_elev", "morning_yaw"} too, over it
// (EnhancedTune.h)
inline void tune_light(float* yaw, float* elev) {
  static const EnhancedTune t_yaw("light.yaw", 0.f);
  static const EnhancedTune t_elev("light.max_elev", 90.f);
  static const EnhancedTune t_morning("light.morning_yaw", 0.f);
  static const char* file = std::getenv("JAK_ENHANCED_TUNE");
  if (file) {
    static float y = 0.f, e = 90.f;
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(1)) {
      last = now;
      if (FILE* f = std::fopen(file, "r")) {
        float a, b, m;
        const int n = std::fscanf(f, "%f %f %f", &a, &b, &m);
        if (n >= 2) {
          y = a;
          e = b;
        }
        g_tune_morning_yaw = n == 3 ? m : NAN;
        std::fclose(f);
      }
    }
    *yaw = y;
    *elev = e;
  } else {
    g_tune_morning_yaw = NAN;
  }
  if (t_yaw.is_set()) {
    *yaw = t_yaw;
  }
  if (t_elev.is_set()) {
    *elev = t_elev;
  }
  if (t_morning.is_set()) {
    g_tune_morning_yaw = t_morning;
  }
}

// the key light in world space (or with sky, the sun as seen in the sky), turned around the world's
// up axis by the area's sun_yaw; test
// switches: JAK_ENHANCED_SUN_YAW=<degrees> instead, JAK_ENHANCED_SUN_ELEV=<degrees> its height cap
inline void sun_world(float out[3], bool sky = false) {
  static const char* env = std::getenv("JAK_ENHANCED_SUN_YAW");
  static const char* env_light = std::getenv("JAK_ENHANCED_LIGHT_YAW");
  float light = sky ? 0.f : env_light ? (float)std::atof(env_light) : g_light_yaw;
  float light_elev = g_light_max_elev;
  if (!sky) {
    tune_light(&light, &light_elev);
  }
  const float yaw =
      ((env ? (float)std::atof(env) : g_sun_yaw) + g_morning_yaw + light) * 3.14159265f / 180.f;
  const float* d = g_enhanced_sun.dir;
  out[0] = std::cos(yaw) * d[0] + std::sin(yaw) * d[2];
  out[1] = d[1];
  out[2] = -std::sin(yaw) * d[0] + std::cos(yaw) * d[2];
  // (lowered toward the horizon, keeping its bearing)
  static const char* env_elev = std::getenv("JAK_ENHANCED_SUN_ELEV");
  const float max_elev = sky ? (env_elev ? (float)std::atof(env_elev) : g_sun_max_elev) : light_elev;
  const float max_y = std::sin(max_elev * 3.14159265f / 180.f);
  if (out[1] > max_y) {
    const float flat = std::sqrt(out[0] * out[0] + out[2] * out[2]);
    if (flat > 1e-4f) {
      const float k = std::sqrt(1.f - max_y * max_y) / flat;
      out[0] *= k;
      out[2] *= k;
      out[1] = max_y;
    }
  }
}
}  // namespace enh_post
