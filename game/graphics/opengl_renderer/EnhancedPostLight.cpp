// The scene's light: the area's grade and weather each frame, focus and exposure, light shafts,
// GI, the sun's shadow maps and their casters, the volumetric mist and the clouds (split from
// EnhancedPost.cpp; shared helpers in EnhancedPostInternal.h)

#include "game/graphics/opengl_renderer/EnhancedPostInternal.h"

using namespace enh_post;

// once a frame: the area Jak is in and the hour pick the grade and the weather, eased in over a
// few seconds so a level change or nightfall doesn't snap
float EnhancedPost::night_darkness() const {
  if (m_settings.dark_nights <= 0.f || !m_ssao_ready) {
    return 0.f;
  }
  const float zone = m_city ? 0.68f : (m_pond ? 0.9f : 0.78f);
  // the moon lights a full-moon night; an eclipse or a hazy night is darker
  static const float kMoonlight[kSkyNights] = {1.f, 1.05f, 0.72f, 0.82f, 1.1f};
  return std::min(1.f, m_night * zone * m_settings.dark_nights * kMoonlight[m_night_kind]);
}

// The evening's sky, picked once a game day as the afternoon ends (and on the first frame): a red
// sunset one evening in four or so, and the night: starry, hazy with few stars, a full moon, a
// nearly full one, now and then a red eclipse. JAK_ENHANCED_NIGHT=0..4 and JAK_ENHANCED_RED_SUNSET=0/1
// (or the menu's Time of day) force them
void EnhancedPost::pick_evening() {
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_real_distribution<float> u(0.f, 1.f);
  static const float kWeights[kSkyNights] = {0.3f, 0.2f, 0.2f, 0.22f, 0.08f};
  float r = u(rng);
  m_night_kind = kStarry;
  for (int i = 0; i < kSkyNights; i++) {
    if (r < kWeights[i]) {
      m_night_kind = i;
      break;
    }
    r -= kWeights[i];
  }
  m_red_sunset = u(rng) < 0.25f;
  // (the menu's pick lasts one evening)
  m_sky_force_night = -1;
  m_sky_force_red = -1;
  lg::info("[enhanced] this evening: {} sunset, night {}", m_red_sunset ? "red" : "normal",
           m_night_kind);
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
  // (the test switches JAK_ENHANCED_NAP and JAK_ENHANCED_MENU_SCRIPT run on the game's thread, in
  // kmachine.cpp's pc_enhanced_set_scene, so they work without a renderer too)
  m_zone = zone_of(level);
  m_pond = g_game_version == GameVersion::Jak3 && level.rfind("des", 0) == 0;
  m_city = g_game_version != GameVersion::Jak1 &&
           ((m_zone > 0 && m_zone < (int)g_world.zones.size() &&
             g_world.zones[m_zone].name.rfind("haven", 0) == 0) ||
            level.rfind("was", 0) == 0);
  // (the night's pale-ground press is for the slums' flagstones only: on every night level it took
  // a third off the desert and the port)
  m_pale_ground = g_game_version == GameVersion::Jak3 && level.rfind("ctyslum", 0) == 0;
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
  m_enclosed = !zone.weather.outdoors;
  for (const auto& prefix : g_world.enclosed[game_index()]) {
    m_enclosed = m_enclosed || level.rfind(prefix, 0) == 0;
  }
  // night from 19 to 21 in, from 5 to 7 out
  auto ramp = [](float a, float b, float x) { return std::clamp((x - a) / (b - a), 0.f, 1.f); };
  m_night = std::max(ramp(19.f, 21.f, hour), 1.f - ramp(5.f, 7.f, hour));
  g_enhanced_play_weather.night = m_night;
  // a new evening's sky as the afternoon passes 16:00 (or the clock is set past it)
  if (m_last_hour < 0.f || (m_last_hour < 16.f && hour >= 16.f) ||
      (hour < m_last_hour - 0.5f && hour >= 16.f)) {
    pick_evening();
  }
  m_last_hour = hour;
  {
    static const char* force_night = std::getenv("JAK_ENHANCED_NIGHT");
    static const char* force_red = std::getenv("JAK_ENHANCED_RED_SUNSET");
    if (force_night && m_sky_force_night < 0) {
      m_sky_force_night = std::clamp(std::atoi(force_night), 0, kSkyNights - 1);
    }
    if (force_red && m_sky_force_red < 0) {
      m_sky_force_red = std::atoi(force_red) != 0;
    }
  }
  if (m_sky_force_night >= 0) {
    m_night_kind = m_sky_force_night;
  }
  if (m_sky_force_red >= 0) {
    m_red_sunset = m_sky_force_red != 0;
  }
  // the sun going down (and, softer, coming up)
  m_dusk = std::max(ramp(16.5f, 18.5f, hour) * (1.f - ramp(19.6f, 20.8f, hour)),
                    0.5f * ramp(5.f, 6.f, hour) * (1.f - ramp(6.6f, 7.6f, hour)));
  m_red_dusk = m_red_sunset && hour > 12.f ? m_dusk : 0.f;

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
  want.sand = s.area_grading ? mix(day.sand, night.sand) : 0.f;
  want.sun_haze = s.area_grading ? mix(day.sun_haze, night.sun_haze) : 0.f;
  want.rock_detail = s.area_grading ? mix(day.rock_detail, night.rock_detail) : 0.f;
  want.headlights = s.area_grading ? mix(day.headlights, night.headlights) : 1.f;
  want.glows = s.area_grading ? mix(day.glows, night.glows) : 1.f;
  want.lamps = s.area_grading ? mix(day.lamps, night.lamps) : 1.f;
  want.golden = s.area_grading ? mix(day.golden, night.golden) : 0.f;
  want.day_sky = s.area_grading ? mix(day.day_sky, night.day_sky) : 0.f;
  want.rock_sun = s.area_grading ? mix(day.rock_sun, night.rock_sun) : 0.f;
  want.wall_sun = s.area_grading ? mix(day.wall_sun, night.wall_sun) : 0.f;
  want.sun_yaw = mix(day.sun_yaw, night.sun_yaw);
  want.morning_yaw = mix(day.morning_yaw, night.morning_yaw);
  want.sun_max_elev = mix(day.sun_max_elev, night.sun_max_elev);
  want.light_yaw = mix(day.light_yaw, night.light_yaw);
  want.light_max_elev = mix(day.light_max_elev, night.light_max_elev);

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
    // sunsets: warmer and richer; the red one deep red, the blue gone from the light
    want.warmth += m_dusk * 0.12f + m_red_dusk * 0.4f;
    want.saturation += m_dusk * 0.12f + m_red_dusk * 0.3f;
    want.contrast += m_red_dusk * 0.06f;
    want.gain[0] += m_red_dusk * 0.14f;
    want.gain[1] -= m_red_dusk * 0.04f;
    want.gain[2] -= m_red_dusk * 0.2f;
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
  // one line per change (the menu's pick, the script's steps): test runs check it
  static int logged_weather_mode = -1;
  if (weather_mode != logged_weather_mode) {
    lg::info("[enhanced] weather mode {} in {}", weather_mode, level.empty() ? "none" : level);
    logged_weather_mode = weather_mode;
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
        // (test: tuning weather.test_spell = a SpellKind (5 sandstorm) holds that spell, -1 off; one
        // log line per change)
        static const EnhancedTune t_test_spell("weather.test_spell", -1.f);
        if ((float)t_test_spell >= 0.f) {
          kind = std::clamp((int)(float)t_test_spell, 0, kSpellKinds - 1);
        }
        if (kind != m_spell) {
          lg::info("[enhanced] weather spell {} in {}", kSpellNames[kind], level);
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
      // Spargus, behind its walls: the game never shows its sandstorm in the city (the storm's
      // process may still run there after a trip, "rogue level entity desert-dust-storm"), and a
      // sandstorm spell laid an orange veil over the streets at noon. Out of the gates it blows
      // again (weather.town_dust: how much of it gets in, 1 = as before)
      if (g_game_version == GameVersion::Jak3 && level.rfind("was", 0) == 0) {
        static const EnhancedTune t_town_dust("weather.town_dust", 0.f);
        dust *= std::clamp((float)t_town_dust, 0.f, 1.f);
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
  // (tuning.json: "grade": {"sunny": {"gain", "lift", "saturation", ...}})
  if (sunny > 0.f) {
    static const EnhancedTune gain("grade.sunny.gain", {1.1f, 1.06f, 0.96f});
    static const EnhancedTune lift("grade.sunny.lift", {0.012f, 0.02f, 0.045f});
    static const EnhancedTune t_saturation("grade.sunny.saturation", 1.45f);
    static const EnhancedTune t_contrast("grade.sunny.contrast", 0.08f);
    static const EnhancedTune t_warmth("grade.sunny.warmth", 0.075f);
    static const EnhancedTune t_clouds("grade.sunny.clouds", 0.32f);
    auto toward = [&](float& v, float t) { v += (t - v) * std::min(sunny, 1.f); };
    for (int i = 0; i < 3; i++) {
      toward(want.gain[i], gain[i]);
      toward(want.lift[i], lift[i]);
    }
    toward(want.saturation, t_saturation);
    toward(want.contrast, t_contrast);
    toward(want.warmth, t_warmth);
    toward(want.clouds, t_clouds);
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
  ease(m_grade.sand, want.sand, k_grade);
  ease(m_grade.sun_haze, want.sun_haze, k_grade);
  ease(m_grade.rock_detail, want.rock_detail, k_grade);
  ease(m_grade.headlights, want.headlights, k_grade);
  ease(m_grade.glows, want.glows, k_grade);
  ease(m_grade.lamps, want.lamps, k_grade);
  ease(m_grade.golden, want.golden, k_grade);
  ease(m_grade.day_sky, want.day_sky, k_grade);
  ease(m_grade.rock_sun, want.rock_sun, k_grade);
  ease(m_grade.wall_sun, want.wall_sun, k_grade);
  ease(m_grade.sun_yaw, want.sun_yaw, k_grade);
  ease(m_grade.morning_yaw, want.morning_yaw, k_grade);
  g_sun_yaw = m_grade.sun_yaw;
  ease(m_grade.light_yaw, want.light_yaw, k_grade);
  g_light_yaw = m_grade.light_yaw;
  ease(m_grade.light_max_elev, want.light_max_elev, k_grade);
  ease(m_grade.sun_max_elev, want.sun_max_elev, k_grade);
  {
    // (by the late afternoon's hours; all of it from a golden of 0.5, as the shader's low light)
    auto smooth = [](float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); };
    // (and the morning's: the sun as low, long shadows). The area's own path: the desert's by
    // its late afternoon, the other outdoor areas' by their day sky
    const float path = std::max(std::min(m_grade.golden * 2.f, 1.f), m_grade.day_sky);
    // (the desert's sun stays as low all day: overhead at noon its rocks lost their shading)
    const float golden_area = std::min(m_grade.golden * 2.f, 1.f);
    const float g = std::max({smooth((hour - 14.5f) / 1.5f) * (1.f - smooth((hour - 18.f) / 1.f)),
                              smooth(hour - 6.f) * (1.f - smooth((hour - 10.f) / 1.5f)),
                              smooth(hour - 6.f) * (1.f - smooth((hour - 17.f) / 1.f)) * golden_area}) *
                    path;
    g_sun_max_elev = 90.f + (m_grade.sun_max_elev - 90.f) * g;
    g_light_max_elev = 90.f + (m_grade.light_max_elev - 90.f) * g;
    // (but not as low in the middle of the day: at 18 degrees the desert-start basin lay in its
    // cliffs' shadow at noon, grey-blue where the game has it bright. At 50 it still did: Spargus'
    // gate walls, 40-60 m high, 20-40 m behind it (the gap to the caster was 20-100 m along the
    // light). At 65 the basin is in the sun with short shadows at the walls' feet, the rocks still
    // shaded; the sun in the sky goes up with it, so the shadows match it. The morning and the
    // golden hour as they were)
    {
      static const EnhancedTune t_light("light.midday_elev", 65.f);
      static const EnhancedTune t_sun("light.midday_sun_elev", 58.f);
      const float midday = smooth((hour - 10.f) / 1.5f) * (1.f - smooth((hour - 14.f) / 1.5f)) * golden_area;
      g_light_max_elev += std::max((float)t_light - g_light_max_elev, 0.f) * midday;
      g_sun_max_elev += std::max((float)t_sun - g_sun_max_elev, 0.f) * midday;
    }
    const float morning = smooth(hour - 6.f) * (1.f - smooth((hour - 10.f) / 1.5f)) * path;
    g_morning_yaw = (std::isnan(g_tune_morning_yaw) ? m_grade.morning_yaw : g_tune_morning_yaw) * morning;
    // the morning's low fog in the areas under open sky (not the desert's dry air), gone by mid-morning
    m_dawn_fog = smooth(hour - 5.f) * (1.f - smooth((hour - 7.5f) / 2.f)) * m_grade.day_sky *
                 std::max(0.f, 1.f - m_grade.golden * 2.f) * s.dawn_fog * (m_city ? 0.5f : 1.f);
  }
  ease(m_grade.clouds, want.clouds, k_weather);
  ease(m_rain, rain * s.rain_strength, k_weather);
  ease(m_dust, dust * s.dust_strength, k_weather);
  {
    // (one line per tenth of change: what feeds the post's sandstorm, and the game's own storm)
    static int logged = -1;
    const int now = (int)std::lround(m_dust * 10.f) * 100 + (int)std::lround(game_dust * 10.f);
    if (now != logged) {
      lg::info("[enhanced] dust {:.1f} (game storm {:.1f}) in {}", m_dust, game_dust,
               level.empty() ? "none" : level);
      logged = now;
    }
  }
  ease(m_storm, storm, k_weather);
  if (kFogOff) {
    fog = 0.f;
    mist = 0.f;
    m_dawn_fog = 0.f;
  }
  ease(m_mist, mist, k_weather);
  // fog comes and goes slowly (half a minute)
  ease(m_fog, fog, 1.f - std::exp(-dt / 10.f));
  ease(m_clear, clear, k_weather);
  ease(m_sunny, sunny, k_weather);
  ease(m_snow, snow, k_weather);
  ease(m_heat, heat, k_grade);
  // the ground soaks in a few seconds and takes a minute and a half to dry
  ease(m_wet, wet, 1.f - std::exp(-dt / (wet > m_wet ? 5.f : 40.f)));
  g_enhanced_wind.wet = m_settings.enabled ? m_wet : 0.f;
  g_enhanced_parallax = m_settings.enabled ? m_settings.parallax : 0.f;
  g_enhanced_wall_notile = m_settings.enabled && m_settings.wall_notile;
  // snow lies: it builds up over ~6 minutes of heavy snow, melts in ~4 after (in a minute in the
  // rain). JAK_ENHANCED_SNOW_COVER=<0..1> holds it (screenshots)
  {
    static const char* cover_test = std::getenv("JAK_ENHANCED_SNOW_COVER");
    if (cover_test) {
      m_snow_cover = (float)std::atof(cover_test);
    } else if (m_snow > 0.05f) {
      m_snow_cover = std::min(1.f, m_snow_cover + dt * m_snow / 360.f);
    } else {
      m_snow_cover = std::max(0.f, m_snow_cover - dt * (m_rain > 0.1f ? 1.f / 60.f : 1.f / 240.f));
    }
  }
  // the puddles fill over a minute of rain and dry from their rims in, for minutes after
  ease(m_puddle, wet, 1.f - std::exp(-dt / (wet > m_puddle ? 20.f : 220.f)));
  g_enhanced_merc.wet = std::max(m_rain, m_wet * 0.5f);
  g_enhanced_play_weather.sand = std::clamp(std::max(m_dust, game_dust), 0.f, 1.f);
  g_enhanced_play_weather.fog = std::clamp(std::max(m_fog, m_mist), 0.f, 1.f);

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
  // the grass and trees sway with it: the wind's speed, a storm or a sandstorm, and slow swells
  // over a few minutes (calm spells, breezier ones)
  {
    const float swell = 0.8f + 0.2f * (float)std::sin(m_time * 0.031) * (float)std::sin(m_time * 0.0077 + 1.3);
    const float rough = std::max(m_storm, std::max(m_dust, 0.7f * m_rain));
    g_enhanced_wind.gust = std::clamp((m_wind_speed / 9.f + 1.4f * rough) * swell, 0.5f, 3.f);
  }
  const float wl =std::max(1e-3f, std::sqrt(m_wind[0] * m_wind[0] + m_wind[1] * m_wind[1]));
  // (wrapped now and then: a pop every few hours of the same wind)
  for (int i = 0; i < 2; i++) {
    m_drift[i] = std::fmod(m_drift[i] + m_wind[i] / wl * m_wind_speed * dt, 1e5);
  }

  // lightning: a bolt every few seconds in a thunderstorm, a bright flash that flickers
  if (m_next_bolt == 0) {
    m_next_bolt = m_time + 3.0;
  }
  if (m_storm > 0.3f && m_time >= m_next_bolt && m_strike_age < 0.f) {
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
  // lightning that strikes the ground, rarely, in a strong storm (a minute or three apart; the
  // rest stays up in the sky): mostly far from Jak (60-250 m), sometimes nearer (25-60), rarely
  // close (6-20). The game finds the ground and sets off a blast there (pc-enhanced-strike); when
  // it says where, the bolt comes down to that spot
  if (m_next_strike == 0) {
    m_next_strike = m_time + 45.0;
  }
  if (m_storm > 0.5f && m_time >= m_next_strike && g_enhanced_player.valid) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> u(0.f, 1.f);
    m_next_strike = m_time + (60.0 + 120.0 * u(rng)) / m_storm;
    const float r = u(rng);
    float dist = r < 0.04f ? 6.f + 14.f * u(rng) : r < 0.2f ? 25.f + 35.f * u(rng) : 60.f + 190.f * u(rng);
    float ang = u(rng) * 6.2832f;
    // test: every few seconds straight ahead of the camera, JAK_ENHANCED_STRIKE_TEST=<meters> away
    static const char* strike_test = std::getenv("JAK_ENHANCED_STRIKE_TEST");
    if (strike_test) {
      dist = (float)std::atof(strike_test);
      ang = std::atan2(m_prev_camera.rot[0][2], m_prev_camera.rot[2][2]);
      m_next_strike = m_time + 4.0;
    }
    std::lock_guard<std::mutex> lock(g_enhanced_strike.mutex);
    g_enhanced_strike.at[0] = g_enhanced_player.pos[0] + std::sin(ang) * dist * kMeter;
    g_enhanced_strike.at[1] = g_enhanced_player.pos[1];
    g_enhanced_strike.at[2] = g_enhanced_player.pos[2] + std::cos(ang) * dist * kMeter;
    g_enhanced_strike.at[3] = 7.f * kMeter;
    g_enhanced_strike.asked = true;
  }
  {
    std::lock_guard<std::mutex> lock(g_enhanced_strike.mutex);
    if (g_enhanced_strike.hit) {
      g_enhanced_strike.hit = false;
      for (int i = 0; i < 3; i++) {
        m_strike_pos[i] = g_enhanced_strike.hit_pos[i];
      }
      m_strike_age = 0.f;
      // what the strike throws up: a burst of sand or dirt, then a column of smoke that drifts off
      if (m_settings.blast_sparks > 0.f) {
        for (int k = 0; k < 7; k++) {
          Puff f;
          const float a = k * 0.9f + (float)(std::rand() % 100) * 0.01f;
          const float r = (k < 4 ? 1.2f : 0.3f) * kMeter;
          f.pos[0] = m_strike_pos[0] + std::cos(a) * r;
          f.pos[1] = m_strike_pos[1];
          f.pos[2] = m_strike_pos[2] + std::sin(a) * r;
          f.size = k < 4 ? 2.2f : 1.8f;
          f.kind = k < 4 ? 0 : 3;
          f.seed = (float)(std::rand() % 1000) / 1000.f;
          f.life = k < 4 ? 2.5f : 7.f + k;
          f.time = m_time;
          m_puffs.push_back(f);
        }
      }
      if (std::getenv("JAK_ENHANCED_STRIKE_TEST")) {
        lg::info("[strike] hit {:.1f} {:.1f} {:.1f} jak {:.1f} {:.1f} {:.1f} cam {:.1f} {:.1f} {:.1f} (m)",
                 m_strike_pos[0] / kMeter, m_strike_pos[1] / kMeter, m_strike_pos[2] / kMeter,
                 g_enhanced_player.pos[0] / kMeter, g_enhanced_player.pos[1] / kMeter,
                 g_enhanced_player.pos[2] / kMeter, m_prev_camera.trans[0] / kMeter,
                 m_prev_camera.trans[1] / kMeter, m_prev_camera.trans[2] / kMeter);
      }
      // the bolt (drawn to the ground: see the "strike" uniform), at its brightest
      const float dx = m_strike_pos[0] - m_prev_camera.trans[0];
      const float dz = m_strike_pos[2] - m_prev_camera.trans[2];
      m_bolt[0] = std::atan2(dx, dz);
      m_bolt[1] = (float)(std::rand() % 10000) / 100.f;
      m_bolt[2] = 0.f;
      m_bolt[3] = 1.f;
      // the thunder: a crack right away when it's close, later the farther it is
      const float meters = std::sqrt(dx * dx + dz * dz) / kMeter;
      m_thunder_at = m_time + meters / 343.f;
      m_thunder_kind = meters < 40.f ? 1 : 1 + std::rand() % 3;
      m_next_bolt = std::max(m_next_bolt, m_time + 1.0);
    }
  }
  static const bool strike_hold = std::getenv("JAK_ENHANCED_STRIKE_TEST") && std::getenv("JAK_ENHANCED_BOLT_TEST");
  if (m_strike_age >= 0.f && !strike_hold) {
    m_strike_age += dt;
    if (m_strike_age > 0.6f) {
      m_strike_age = -1.f;
    }
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
    // straight ahead, at the age JAK_ENHANCED_BOLT_TEST says (s; a strike's own place, if there's one)
    if (m_strike_age < 0.f) {
      m_bolt[0] = std::atan2(m_prev_camera.rot[0][2], m_prev_camera.rot[2][2]) + 0.25f;
    }
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
  // (and lighter while Jak aims his gun: what he aims at, at the middle, stays sharp)
  const bool aim = !movie && g_enhanced_player.valid && g_enhanced_aim.load() > 0.5f;
  ease(m_cine, movie && s.cine_dof ? 1.f : (aim && s.cine_dof ? 0.3f : 0.f),
       1.f - std::exp(-dt / (aim ? 0.25f : 0.4f)));
  ease(m_movie, movie ? 1.f : 0.f, 1.f - std::exp(-dt / 0.4f));
  {
    // Jak's own state: wet out of the water, a jolt of split colors on a hit, a little as Dark Jak
    // (test switch: JAK_ENHANCED_FX_TEST=<flags> holds them)
    static const int fx_test = std::getenv("JAK_ENHANCED_FX_TEST") ? std::atoi(std::getenv("JAK_ENHANCED_FX_TEST")) : 0;
    const int fx = (g_enhanced_player.valid && !movie ? g_enhanced_player.flags : 0) | fx_test;
    m_jak_wet = (fx & 1) ? 1.f : std::max(0.f, m_jak_wet - dt / 14.f);
    const float want = ((fx & 4) ? 1.f : (fx & 2) ? 0.35f : 0.f) * s.hit_chroma;
    ease(m_chroma, want, 1.f - std::exp(-dt / (want > m_chroma ? 0.04f : 0.35f)));
  }
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

// the eye's adaptation: an 8x6 grid of the scene's pixels into a pixel buffer, read the frame
// after (no stall); their mean brightness (geometric) sets how far the picture is lifted or
// lowered: part of the way toward a mid grey (at most -13%..+33%). Faster into the light than out of it
void EnhancedPost::read_exposure(int w, int h) {
  constexpr int kN = 48;
  if (!m_adapt_pbo[0]) {
    glGenBuffers(2, m_adapt_pbo);
    for (int i = 0; i < 2; i++) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, m_adapt_pbo[i]);
      // (8 bytes a pixel: room for half floats)
      glBufferData(GL_PIXEL_PACK_BUFFER, kN * 8, nullptr, GL_STREAM_READ);
    }
  }
  // (an RGBA16F scene, post.hdr_scene, is read in its own half floats: asked for as bytes, the
  // driver converted on the CPU, waiting for the GPU at each of the 48 reads, +5 ms CPU a frame)
  auto half_to_float = [](uint16_t v) {
    const int e = (v >> 10) & 0x1f;
    const int m = v & 0x3ff;
    const float f = e == 0    ? std::ldexp((float)m, -24)
                    : e == 31 ? 65504.f
                              : std::ldexp((float)(m | 0x400), e - 25);
    return (v & 0x8000) ? -f : f;
  };
  const float dt = m_adapt_time > 0.0 ? (float)std::clamp(m_time - m_adapt_time, 0.0, 0.2) : 0.f;
  m_adapt_time = m_time;
  const int prev = 1 - m_adapt_cur;
  if (m_adapt_pending[prev]) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_adapt_pbo[prev]);
    const bool half = m_adapt_half[prev];
    if (auto* d = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, kN * (half ? 8 : 4),
                                                   GL_MAP_READ_BIT)) {
      float sum = 0.f;
      for (int i = 0; i < kN; i++) {
        float c[3];
        for (int k = 0; k < 3; k++) {
          // (past 1 counted as 1, as the 8-bit frame clips it)
          c[k] = half ? std::clamp(half_to_float(((const uint16_t*)d)[i * 4 + k]), 0.f, 1.f)
                      : d[i * 4 + k] / 255.f;
        }
        const float l = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
        sum += std::log(std::max(l, 0.02f));
      }
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      const float mean = std::exp(sum / kN);
      {
        static const EnhancedTune t_cpu_profile("debug.cpu_profile", 0.f);
        static int frames = 0;
        if ((float)t_cpu_profile > 0.5f && ++frames >= 300) {
          frames = 0;
          lg::info("[enhanced] exposure: mean {:.4f}, eye {:.4f}", mean, m_adapt);
        }
      }
      const float want = m_settings.eye_adapt > 0.f
                             ? std::pow(std::clamp(0.36f / mean, 0.8f, 1.6f), m_settings.eye_adapt * 0.6f)
                             : 1.f;
      const float tau = want < m_adapt ? 0.7f : 2.f;
      // (not over a black frame: a level loading in, nothing drawn but a few torches. The eye
      // opened up to +33% there, and the city's first seconds came in washed out and orange)
      if (mean > 0.035f) {
        m_adapt += (want - m_adapt) * (1.f - std::exp(-dt / tau));
      }
    } else {
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    m_adapt_pending[prev] = false;
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_scene_fbo);
  // (the format query once per scene texture: it is recreated on a resize or a format switch)
  if (m_adapt_scene_tex != m_scene_tex) {
    GLint comp = GL_UNSIGNED_NORMALIZED;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &comp);
    m_adapt_scene_half = comp == GL_FLOAT;
    m_adapt_scene_tex = m_scene_tex;
  }
  const bool half = m_adapt_scene_half;
  m_adapt_half[m_adapt_cur] = half;
  // (tuning "post": {"exposure_blit": 0}: the 48 one-pixel reads as before. 48 reads cost 0.23 ms
  // of render-thread CPU a frame (Spargus 18h); one nearest blit to 8x6 picks the same pixels,
  // (2x + 1) w / 16 and (2y + 1) h / 12, and one read takes them all)
  static const EnhancedTune t_blit("post.exposure_blit", 1.f);
  if ((float)t_blit > 0.5f) {
    if (!m_adapt_fbo || m_adapt_tex_half != half) {
      if (!m_adapt_fbo) {
        glGenFramebuffers(1, &m_adapt_fbo);
        glGenTextures(1, &m_adapt_tex);
      }
      glBindTexture(GL_TEXTURE_2D, m_adapt_tex);
      glTexImage2D(GL_TEXTURE_2D, 0, half ? GL_RGBA16F : GL_RGBA8, 8, 6, 0, GL_RGBA,
                   half ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glBindTexture(GL_TEXTURE_2D, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_adapt_fbo);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_adapt_tex, 0);
      m_adapt_tex_half = half;
    }
    GLint draw_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) {
      glDisable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_adapt_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, 8, 6, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) {
      glEnable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_adapt_fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_adapt_pbo[m_adapt_cur]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, 8, 6, GL_RGBA, half ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fbo);
  } else {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_adapt_pbo[m_adapt_cur]);
    int i = 0;
    for (int y = 0; y < 6; y++) {
      for (int x = 0; x < 8; x++) {
        glReadPixels((2 * x + 1) * w / 16, (2 * y + 1) * h / 12, 1, 1, GL_RGBA,
                     half ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, (void*)(intptr_t)(i++ * (half ? 8 : 4)));
      }
    }
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  m_adapt_pending[m_adapt_cur] = true;
  m_adapt_cur = prev;
}

void EnhancedPost::do_shafts(GLuint scene_tex,
                             GLuint screen_vao,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  // the sun on screen: in view space the camera looks down +z (see enh_ssao.frag)
  float len = std::sqrt(m_sky_view[0] * m_sky_view[0] + m_sky_view[1] * m_sky_view[1] +
                        m_sky_view[2] * m_sky_view[2]);
  if (m_settings.ssao_debug) {
    static int behind_counter = 0;
    if ((behind_counter++ % 300) == 0) {
      lg::info("[enhanced] sun view {} {} {}", m_sky_view[0], m_sky_view[1], m_sky_view[2]);
    }
  }
  // (the desert's haze shadow runs at every quality, with the sun anywhere; the rays need it in
  // front)
  bool haze = m_grade.sun_haze > 0.f && m_settings.enabled;
  bool rays = m_settings.shafts && m_fade_rays > 0.f;
  if (len < 1e-6f) {
    return;
  }
  float side = m_sky_view[2] / len < 0.05f ? -1.f : 1.f;  // behind: toward the opposite point
  if (side < 0.f) {
    rays = false;
  }
  if (!rays && !haze) {
    return;
  }
  float sz = std::max(std::abs(m_sky_view[2]), 0.05f * len);
  float sx = 0.5f * m_last_proj[2] * side * m_sky_view[0] / sz + 0.5f;
  float sy = 0.5f * m_last_proj[3] * side * m_sky_view[1] / sz + 0.5f;
  // fade out as the sun leaves the screen
  float off = std::max(std::max(-sx, sx - 1.f), std::max(-sy, sy - 1.f));
  float fade = side > 0.f ? 1.f - std::clamp(off / 0.6f, 0.f, 1.f) : 0.f;
  static int debug_counter = 0;
  if (m_settings.ssao_debug && (debug_counter++ % 300) == 0) {
    lg::info("[enhanced] sun on screen {} {} fade {}", sx, sy, fade);
  }
  if (fade <= 0.f) {
    rays = false;
    if (!haze) {
      return;
    }
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
  glUniform1f(glGetUniformLocation(id, "rays"), rays ? 1.f : 0.f);
  glUniform1f(glGetUniformLocation(id, "haze"), haze ? 1.f : 0.f);
  glUniform1f(glGetUniformLocation(id, "sun_side"), side);
  glUniform4f(glGetUniformLocation(id, "proj"), m_last_proj[0], m_last_proj[1], m_last_proj[2],
              m_last_proj[3]);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindVertexArray(0);
  m_shafts_ready = rays;
  m_haze_shadow_ready = haze;
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
    if (m_gi_hist_fbo[0]) {
      glDeleteFramebuffers(2, m_gi_hist_fbo);
      glDeleteTextures(2, m_gi_hist_tex);
      m_gi_hist_fbo[0] = m_gi_hist_fbo[1] = m_gi_hist_tex[0] = m_gi_hist_tex[1] = 0;
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
  // temporal (gi.temporal, as ssao.temporal): half the samples a frame, the other half last frame's
  // at the same world point (own RGBA16F history, a second target)
  // (0.53 -> 0.36 ms at 2560x1600, Spargus 18h; A/B pictures differ only by the clock's drift)
  static const EnhancedTune kGiTemporal("gi.temporal", 1.f);
  const bool gi_tmp = (float)kGiTemporal > 0.f;
  if (gi_tmp && !m_gi_hist_fbo[0]) {
    glGenFramebuffers(2, m_gi_hist_fbo);
    glGenTextures(2, m_gi_hist_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_gi_hist_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_gi_hist_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gi_tex[0], 0);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gi_hist_tex[i], 0);
      const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
      glDrawBuffers(2, bufs);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_gi_hist_valid = false;
  }
  if (!gi_tmp) {
    m_gi_hist_valid = false;
  }
  glUniform1i(glGetUniformLocation(id, "gi_temporal"), !gi_tmp ? 0 : m_gi_hist_valid ? 1 : 2);
  if (gi_tmp) {
    m_gi_frame++;
    glUniform1i(glGetUniformLocation(id, "gi_frame"), (int)(m_gi_frame & 1u));
    float to_prev[3][3];
    float offset[3];
    if (!enh_post::view_to_prev(g_enhanced_camera, m_prev_camera, to_prev, offset)) {
      glUniform1i(glGetUniformLocation(id, "gi_temporal"), 2);
    }
    glUniformMatrix3fv(glGetUniformLocation(id, "to_prev"), 1, GL_FALSE, &to_prev[0][0]);
    glUniform3f(glGetUniformLocation(id, "to_prev_offset"), offset[0], offset[1], offset[2]);
    glUniform2f(glGetUniformLocation(id, "prev_f"), m_prev_camera.fx, m_prev_fy);
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, m_gi_hist_tex[1 - m_gi_hist_cur]);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T12"), 12);
    glBindFramebuffer(GL_FRAMEBUFFER, m_gi_hist_fbo[m_gi_hist_cur]);
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, m_gi_fbo[0]);
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  if (gi_tmp) {
    m_gi_hist_cur = 1 - m_gi_hist_cur;
    m_gi_hist_valid = true;
  }

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
// (tuning.json: "shadow": {"alpha_ref", "far_range_m", "far_depth_m", "near_depth_m"})
// texture alpha under this doesn't cast a shadow (leaf cards, cutouts)
const EnhancedTune kShadowAlphaRef("shadow.alpha_ref", 0.25f);
// the level's alpha-tested draws: their own cutoff (aref / 127) times this (read at level load)
const EnhancedTune kShadowAlphaScale("shadow.alpha_scale", 1.f);
// the far shadow map: half its side and its depth span, meters
const EnhancedTune kShadowFarRange("shadow.far_range_m", 1200.f);
const EnhancedTune kShadowFarDepth("shadow.far_depth_m", 3000.f);
// the near map's depth span, meters (its side is the menu's shadow range)
const EnhancedTune kShadowNearDepth("shadow.near_depth_m", 300.f);
// the middle map (between the near and the far one, 2048 texels): on/off, half its side, its
// depth span, meters. From 80 m to ~400 m the far map's 59 cm texels left houses and rocks blobby
const EnhancedTune kShadowMidOn("shadow.mid_on", 0.f);  // (off until shown to help: no cost measured)
const EnhancedTune kShadowMidRange("shadow.mid_range_m", 320.f);
const EnhancedTune kShadowMidDepth("shadow.mid_depth_m", 1000.f);
constexpr int kMidRes = 2048;
}  // namespace

void EnhancedPost::do_shadow_map(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_shadow_ready = false;
  float sun[3];
  sun_world(sun);
  const float len = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
  const auto& cam = g_enhanced_camera;
  const bool sun_up = g_enhanced_sun.valid && len >= 1e-6f && sun[1] / len >= 0.05f;
  // (the lamps' shadow maps draw the same casters, by night too: their list is kept up below)
  static const EnhancedTune t_lamp_maps("lights.shadow_maps", 4.f);
  const bool lamp_maps = (float)t_lamp_maps > 0.5f && m_settings.light_shadow > 0.f;
  if (!cam.valid || (!sun_up && !lamp_maps)) {
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
    // the far map, the same kind
    glGenFramebuffers(1, &m_shadow_far_fbo);
    glGenTextures(1, &m_shadow_far_tex);
    glBindTexture(GL_TEXTURE_2D, m_shadow_far_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kRes, kRes, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_far_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadow_far_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_shadow_far_ready = false;
  }
  const bool mid_on = (float)kShadowMidOn > 0.5f;
  if (mid_on && !m_shadow_mid_fbo) {
    glGenFramebuffers(1, &m_shadow_mid_fbo);
    glGenTextures(1, &m_shadow_mid_tex);
    glBindTexture(GL_TEXTURE_2D, m_shadow_mid_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kMidRes, kMidRes, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_mid_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadow_mid_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_shadow_mid_ready = false;
  }
  if (!mid_on) {
    m_shadow_mid_ready = false;
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
    m_shadow_draws_gen++;
    const auto& name = lev->level->level_name;
    const bool cut = (name == "mhcitya" || name == "ctyport") && enhanced_has_piazza();
    // (test: JAK_ENHANCED_SHADOW_SKIP=lev1,lev2 casts nothing from those levels: bisecting a
    // phantom shadow)
    // (and lev:t<i> / lev:e<i> one tfrag / tie tree, lev:wind, lev:shrub; the trees are logged)
    static const char* skip = std::getenv("JAK_ENHANCED_SHADOW_SKIP");
    auto skipped = [&](const std::string& what) {
      return skip && ("," + std::string(skip) + ",").find("," + what + ",") != std::string::npos;
    };
    if (skipped(name)) {
      lg::info("[enhanced] shadow casters {}: skipped (JAK_ENHANCED_SHADOW_SKIP)", name);
      continue;
    }
    auto log_tree = [&](const char* kind, size_t i, const tfrag3::BVH& bvh, size_t n) {
      if (skip && !bvh.vis_nodes.empty()) {
        const auto& b = bvh.vis_nodes[0].bsphere;
        lg::info("[enhanced] shadow tree {}:{}{} {} draws, sphere {:.0f} {:.0f} {:.0f} r {:.0f} m", name,
                 kind, i, n, b.x() / 4096.f, b.y() / 4096.f, b.z() / 4096.f, b.w() / 4096.f);
      }
    };
    // a tree's draws that cast, as runs of its index buffer: not the alpha-blended ones that leave
    // the depth alone (decals, veils): see-through, they cast hard-edged dark slabs. The blended
    // ones that write depth are solid (jak 3's desert draws its rocks, palms and buildings that
    // way: left out, nothing there cast); nor a tie's water, nor the envmapped ones' second
    // (shiny) draw, the same shape again
    // (counted for the log: what casts, what is left out and why)
    size_t n_cast = 0, n_veil = 0, n_water = 0;
    auto add_solid = [&](const std::vector<tfrag3::StripDraw>& tree_draws, size_t num_idx,
                         const ShadowDraw& proto, const tfrag3::TieTree* tie,
                         const std::string& tag) {
      size_t added = 0;
      for (size_t k = 0; k < tree_draws.size(); k++) {
        const auto& draw = tree_draws[k];
        if (skip && skipped(tag + "d" + std::to_string(k))) {
          continue;
        }
        if (tie) {
          int cat = 0;
          while (cat + 1 < tfrag3::kNumTieCategories && k >= tie->category_draw_indices[cat + 1]) {
            cat++;
          }
          const auto c = (tfrag3::TieCategory)cat;
          if (c == tfrag3::TieCategory::WATER || c == tfrag3::TieCategory::WATER_ENVMAP ||
              tfrag3::is_envmap_second_draw_category(c)) {
            n_water += !tfrag3::is_envmap_second_draw_category(c);
            continue;
          }
        }
        u32 first = draw.unpacked.idx_of_first_idx_in_full_buffer;
        u32 end = k + 1 < tree_draws.size()
                      ? tree_draws[k + 1].unpacked.idx_of_first_idx_in_full_buffer
                      : (u32)num_idx;
        // (nor the ones that leave the depth alone by the game's other trick, alpha test NEVER with
        // FB_ONLY: decals laid over the ground. The desert's basin floor sat in the shadow of its own
        // sand overlay, a few cm above it, at any hour)
        const bool no_z = !draw.mode.get_depth_write_enable() ||
                          (draw.mode.get_at_enable() &&
                           draw.mode.get_alpha_test() == DrawMode::AlphaTest::NEVER);
        if (no_z || end <= first) {
          n_veil += end > first;
          continue;
        }
        // alpha tested as the game draws them (fences, grates, leaf cards cast their holes)
        GLuint tex = 0;
        float alpha = -1.f;
        if (draw.mode.get_at_enable() && draw.mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL &&
            draw.mode.get_aref() > 0 && draw.tree_tex_id < lev->textures.size()) {
          tex = lev->textures[draw.tree_tex_id];
          alpha = std::max(draw.mode.get_aref() / 127.f * kShadowAlphaScale, 0.1f);
        }
        n_cast++;
        if (added && draws.back().first + draws.back().count == first && draws.back().tex == tex &&
            draws.back().alpha == alpha) {
          draws.back().count += end - first;  // (joined to the run before)
          continue;
        }
        ShadowDraw d = proto;
        d.own_indices = proto.own_indices && !added;  // (the buffer freed with the first)
        d.first = first;
        d.count = end - first;
        d.tex = tex;
        d.alpha = alpha;
        draws.push_back(d);
        added++;
      }
      return added;
    };
    const auto& tfrags = lev->level->tfrag_trees[0];
    for (size_t i = 0; i < tfrags.size() && i < lev->tfrag_vertex_data[0].size(); i++) {
      const auto& tree = tfrags[i];
      if (tree.kind == tfrag3::TFragmentTreeKind::WATER ||
          tree.kind == tfrag3::TFragmentTreeKind::LOWRES ||
          tree.kind == tfrag3::TFragmentTreeKind::LOWRES_TRANS || tree.unpacked.indices.empty()) {
        continue;
      }
      log_tree("t", i, tree.bvh, tree.draws.size());
      if (skipped(name + ":t" + std::to_string(i))) {
        continue;
      }
      if (skip) {
        // (test: each draw's extent, the big ones: a phantom caster is usually one of them)
        const auto& vs = tree.unpacked.vertices;
        const auto& is = tree.unpacked.indices;
        for (size_t k = 0; k < tree.draws.size(); k++) {
          u32 a = tree.draws[k].unpacked.idx_of_first_idx_in_full_buffer;
          u32 b = k + 1 < tree.draws.size() ? tree.draws[k + 1].unpacked.idx_of_first_idx_in_full_buffer
                                            : (u32)is.size();
          float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
          for (u32 q = a; q < b && q < is.size(); q++) {
            if (is[q] == UINT32_MAX || is[q] >= vs.size()) {
              continue;
            }
            const float pv[3] = {vs[is[q]].x, vs[is[q]].y, vs[is[q]].z};
            for (int ax = 0; ax < 3; ax++) {
              lo[ax] = std::min(lo[ax], pv[ax]);
              hi[ax] = std::max(hi[ax], pv[ax]);
            }
          }
          const auto& m = tree.draws[k].mode;
          lg::info("[enhanced] shadow draw {}:t{}d{} idx {} x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.0f}..{:.0f} "
                   "ab {} zw {} at {} atst {} aref {} tex {}",
                   name, i, k, b - a, lo[0] / 4096.f, hi[0] / 4096.f, lo[1] / 4096.f, hi[1] / 4096.f,
                   lo[2] / 4096.f, hi[2] / 4096.f, m.get_ab_enable(), m.get_depth_write_enable(),
                   m.get_at_enable(), (int)m.get_alpha_test(), m.get_aref(), tree.draws[k].tree_tex_id);
        }
      }
      ShadowDraw d;
      d.vertices = lev->tfrag_vertex_data[0][i];
      glGenBuffers(1, &d.indices);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.unpacked.indices.size() * sizeof(u32),
                   tree.unpacked.indices.data(), GL_STATIC_DRAW);
      d.own_indices = true;
      d.mode = tree.use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      if (!add_solid(tree.draws, tree.unpacked.indices.size(), d, nullptr, name + ":t" + std::to_string(i))) {
        glDeleteBuffers(1, &d.indices);
      }
    }
    const auto& ties = lev->level->tie_trees[0];
    for (size_t i = 0; i < ties.size() && i < lev->tie_data[0].size(); i++) {
      if (ties[i].unpacked.indices.empty()) {
        continue;
      }
      log_tree("e", i, ties[i].bvh, ties[i].static_draws.size());
      if (skipped(name + ":e" + std::to_string(i))) {
        continue;
      }
      ShadowDraw d;
      d.vertices = lev->tie_data[0][i].vertex_buffer;
      d.indices = lev->tie_data[0][i].index_buffer;
      d.mode = ties[i].use_strips ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      d.cut = cut;
      add_solid(ties[i].static_draws, ties[i].unpacked.indices.size(), d, &ties[i],
                name + ":e" + std::to_string(i));
    }
    if (!skipped(name + ":wind")) {
      add_wind_shadow_draws(lev, cut, &draws);
    }
    // the shrubs (bushes, plants, small rocks), cut out by their textures' alpha: the near map
    // only (on the far one they cost the most and their shadows are a texel or two), except the big
    // plants (over 4 m: bushes, small trees), whose strips go again, on their own, to the far map.
    // The game's own only: the vegetation's extra copies for the denser settings don't cast
    size_t n_shrubs = 0, n_big = 0;
    const auto& shrubs = lev->level->shrub_trees;
    for (size_t i = 0; i < shrubs.size() && i < lev->shrub_vertex_data.size(); i++) {
      const auto& tree = shrubs[i];
      if (tree.indices.empty() || skipped(name + ":shrub")) {
        continue;
      }
      GLuint ib = 0;
      size_t added = 0;
      for (const auto& draw : tree.static_draws) {
        if (draw.enh_tier != 0 || !draw.num_indices ||
            (draw.mode.get_ab_enable() && !draw.mode.get_depth_write_enable())) {
          continue;
        }
        if (!ib) {
          glGenBuffers(1, &ib);
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
          glBufferData(GL_ELEMENT_ARRAY_BUFFER, tree.indices.size() * sizeof(u32),
                       tree.indices.data(), GL_STATIC_DRAW);
        }
        ShadowDraw d;
        d.vertices = lev->shrub_vertex_data[i];
        d.indices = ib;
        d.own_indices = !added;
        d.mode = GL_TRIANGLE_STRIP;
        d.cut = cut;
        d.shrub = true;
        d.first = draw.first_index_index;
        d.count = draw.num_indices;
        if (draw.tree_tex_id < lev->textures.size()) {
          d.tex = lev->textures[draw.tree_tex_id];
        }
        draws.push_back(d);
        added++;
      }
      n_shrubs += added;
      // the big plants: each instance's size from its vertices (unpack() lays them out in order)
      const auto& verts = tree.unpacked.vertices;
      const auto& groups = tree.packed_vertices.instance_groups;
      if (!added || verts.empty() || groups.empty()) {
        continue;
      }
      std::vector<u8> big_vert(verts.size(), 0);
      bool any_big = false;
      for (u32 g = 0, v0 = 0; g < groups.size() && v0 < verts.size(); g++) {
        u32 v1 = std::min<u32>(v0 + groups[g].end_vert - groups[g].start_vert, verts.size());
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (u32 v = v0; v < v1; v++) {
          const float q[3] = {verts[v].x, verts[v].y, verts[v].z};
          for (int a = 0; a < 3; a++) {
            lo[a] = std::min(lo[a], q[a]);
            hi[a] = std::max(hi[a], q[a]);
          }
        }
        constexpr float kBig = 4.f * 4096.f;
        if (v1 > v0 && (hi[0] - lo[0] > kBig || hi[1] - lo[1] > kBig || hi[2] - lo[2] > kBig)) {
          std::fill(big_vert.begin() + v0, big_vert.begin() + v1, 1);
          any_big = true;
        }
        v0 = v1;
      }
      if (!any_big) {
        continue;
      }
      // their strips, draw by draw, into one index buffer of their own
      std::vector<u32> big_idx;
      std::vector<ShadowDraw> big_draws;
      for (size_t k = draws.size() - added; k < draws.size(); k++) {
        const auto& src = draws[k];
        u32 first = big_idx.size();
        u32 end = std::min<u32>(src.first + src.count, tree.indices.size());
        for (u32 at = src.first; at < end;) {
          u32 stop = at;
          while (stop < end && tree.indices[stop] != UINT32_MAX) {
            stop++;
          }
          if (stop > at && tree.indices[at] < big_vert.size() && big_vert[tree.indices[at]]) {
            big_idx.insert(big_idx.end(), tree.indices.begin() + at, tree.indices.begin() + stop);
            big_idx.push_back(UINT32_MAX);
          }
          at = stop + 1;
        }
        if (big_idx.size() > first) {
          ShadowDraw b = src;
          b.big = true;
          b.own_indices = false;
          b.first = first;
          b.count = big_idx.size() - first;
          big_draws.push_back(b);
        }
      }
      if (big_draws.empty()) {
        continue;
      }
      GLuint big_ib = 0;
      glGenBuffers(1, &big_ib);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, big_ib);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, big_idx.size() * sizeof(u32), big_idx.data(),
                   GL_STATIC_DRAW);
      for (size_t k = 0; k < big_draws.size(); k++) {
        big_draws[k].indices = big_ib;
        big_draws[k].own_indices = k == 0;
        draws.push_back(big_draws[k]);
      }
      n_big += big_draws.size();
    }
    lg::info("[enhanced] shadow casters {}: {} draws cast, {} shrub draws (near map), {} of big "
             "plants (far map); left out {} see-through (no depth), {} water",
             name, n_cast, n_shrubs, n_big, n_veil, n_water);
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
      m_shadow_draws_gen++;
    } else {
      ++it;
    }
  }
  if (!sun_up) {
    return;
  }

  // the light's frame: f from the sun down, right and up across; a square of 2 * range around a
  // point ahead of the camera, snapped to whole texels so the edges don't crawl as it moves
  // (in steps of ~0.15 degrees: the sun's path and the area's eased turn rotated the maps by a
  // fraction of a texel every frame, and the shadows' edges crawled, 9 cm a frame a km out)
  {
    const float live[3] = {-sun[0] / len, -sun[1] / len, -sun[2] / len};
    const float dot = live[0] * m_shadow_dir[0] + live[1] * m_shadow_dir[1] + live[2] * m_shadow_dir[2];
    static const EnhancedTune t_step("shadow.dir_step_deg", 0.15f);
    if (dot < std::cos((float)t_step * 3.14159265f / 180.f)) {
      memcpy(m_shadow_dir, live, sizeof(live));
      m_shadow_far_ready = false;  // (the far map redrawn now with it, not next frame)
      m_shadow_mid_ready = false;
    }
  }
  float f[3] = {m_shadow_dir[0], m_shadow_dir[1], m_shadow_dir[2]};
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
  // (shadow.cache: the far and middle maps kept between frames, their square snapped in steps of
  // several texels so it moves less often; still whole texels, the edges don't crawl. Off: as before,
  // the far map every other frame, the middle one on the frames between)
  static const EnhancedTune t_cache("shadow.cache", 0.f);
  static const EnhancedTune t_cache_snap("shadow.cache_snap_texels", 16.f);
  static const EnhancedTune t_cache_age("shadow.cache_max_frames", 120.f);
  const bool cache = (float)t_cache > 0.5f && !g_enhanced_menu.blank_world;
  const float cache_snap = cache ? std::max(1.f, std::floor((float)t_cache_snap)) : 1.f;
  auto make_mvp = [&](float range, float depth, float* mvp, int res, float snap = 1.f) {
    float center[3];
    // the camera looks down view +z: its forward in world is column 2 of the rotation; level (no
    // y), so looking up or down doesn't slide the map's square and its seam over the ground
    float fx = cam.rot[0][2], fz = cam.rot[2][2];
    const float fl = std::sqrt(fx * fx + fz * fz);
    fx = fl > 1e-3f ? fx / fl : 0.f;
    fz = fl > 1e-3f ? fz / fl : 0.f;
    const float fwd[3] = {fx, 0.f, fz};
    for (int i = 0; i < 3; i++) {
      center[i] = cam.trans[i] + fwd[i] * range * 0.5f;
    }
    const float texel = 2.f * range / res;
    float cr = right[0] * center[0] + right[1] * center[1] + right[2] * center[2];
    float cu = up[0] * center[0] + up[1] * center[1] + up[2] * center[2];
    float cf = f[0] * center[0] + f[1] * center[1] + f[2] * center[2];
    cr = std::floor(cr / (texel * snap)) * texel * snap;
    cu = std::floor(cu / (texel * snap)) * texel * snap;
    const float rows[3][4] = {{right[0] / range, right[1] / range, right[2] / range, -cr / range},
                              {up[0] / range, up[1] / range, up[2] / range, -cu / range},
                              {f[0] / depth, f[1] / depth, f[2] / depth, -cf / depth}};
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 4; c++) {
        mvp[r * 4 + c] = rows[r][c];
      }
    }
    mvp[12] = mvp[13] = mvp[14] = 0.f;
    mvp[15] = 1.f;
  };
  // (shadow.near_cache: the near map's static part kept between frames, its square snapped in steps
  // of shadow.near_cache_snap_texels; each frame only the wind-blown trees and the characters are
  // drawn over a copy of it. Off: the whole near map every frame, as before)
  static const EnhancedTune t_near_cache("shadow.near_cache", 0.f);
  static const EnhancedTune t_near_snap("shadow.near_cache_snap_texels", 16.f);
  const bool near_cache = (float)t_near_cache > 0.5f && !g_enhanced_menu.blank_world;
  float near_mvp[16];
  make_mvp(m_settings.shadow_range * kMeter, kShadowNearDepth * kMeter, near_mvp, kRes,
           near_cache ? std::max(1.f, std::floor((float)t_near_snap)) : 1.f);
  if (!near_cache) {
    memcpy(m_light_mvp, near_mvp, sizeof(near_mvp));
  }
  if (near_cache && !m_shadow_near_static_fbo) {
    glGenFramebuffers(1, &m_shadow_near_static_fbo);
    glGenTextures(1, &m_shadow_near_static_tex);
    glBindTexture(GL_TEXTURE_2D, m_shadow_near_static_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kRes, kRes, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_near_static_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           m_shadow_near_static_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_shadow_near_static_ready = false;
  }
  if (!near_cache) {
    m_shadow_near_static_ready = false;
  }
  m_shadow_near_span_m = 2.f * kShadowNearDepth;  // (the composite's biases and penumbrae, in meters)
  // the far map: the mountains, big rocks and trees out to ~1.2 km, the low sun's long shadows
  // there (the near map is 2 x 80 m)
  float far_mvp[16];
  make_mvp(kShadowFarRange * kMeter, kShadowFarDepth * kMeter, far_mvp, kRes, cache_snap);
  float mid_mvp[16];
  make_mvp(kShadowMidRange * kMeter, kShadowMidDepth * kMeter, mid_mvp, kMidRes, cache_snap);

  GLfloat old_clear_depth = 0.f;
  GLint old_depth_func = GL_GEQUAL;
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
  glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5f, 3.f);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);

  auto& shader = render_state->shaders[ShaderId::ENH_SHADOW];
  shader.activate();
  auto id = shader.id();
  GLint u_cut = glGetUniformLocation(id, "enh_cut");
  GLint u_alpha = glGetUniformLocation(id, "alpha_ref");
  glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
  glActiveTexture(GL_TEXTURE0);
  // (the middle map draws as the far one does (the big plants, not the small shrubs), the terrain too)
  // (part: 0 everything, 1 all but the wind-blown trees, 2 only those, over what the map holds)
  auto draw_levels = [&](GLuint fbo, const float* mvp, bool wind, bool far_map, bool mid_map = false,
                         int part = 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, mid_map ? kMidRes : kRes, mid_map ? kMidRes : kRes);
    if (part != 2) {
      glClear(GL_DEPTH_BUFFER_BIT);
    }
    glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, mvp);
    glBindVertexArray(m_shadow_vao);
    glEnableVertexAttribArray(0);
    if (g_enhanced_menu.blank_world) {
      glUniform1i(u_cut, 0);
      glUniform1f(u_alpha, -1.f);
      // (not in the far map: its big texels leave stripes on the ground, its slopes show the sun anyway)
      if (!far_map || mid_map) {
        enhanced_terrain_shadow_draw();
        prof.add_draw_call();
      }
      return;
    }
    for (auto& [lid, draws] : m_shadow_draws) {
      for (auto& d : draws) {
        if ((d.wind && (!m_settings.shadow_casters || !wind)) || (d.shrub && far_map != d.big) ||
            (part == 1 && d.wind) || (part == 2 && !d.wind)) {
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
        } else if (d.shrub) {
          // (the shrubs' vertices: world x, y, z, s, t first)
          glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::ShrubGpuVertex), (void*)0);
          glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(tfrag3::ShrubGpuVertex),
                                (void*)(3 * sizeof(float)));
          glEnableVertexAttribArray(1);
        } else {
          glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex),
                                (void*)0);
          if (d.tex) {
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex),
                                  (void*)offsetof(tfrag3::PreloadedVertex, s));
            glEnableVertexAttribArray(1);
          } else {
            glDisableVertexAttribArray(1);
          }
        }
        glUniform1f(u_alpha, !d.tex ? -1.f : d.alpha >= 0.f ? d.alpha : (float)kShadowAlphaRef);
        if (d.tex) {
          glBindTexture(GL_TEXTURE_2D, d.tex);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
        glDrawElements(d.mode, d.count, GL_UNSIGNED_INT, (void*)(d.first * sizeof(u32)));
        prof.add_draw_call();
      }
    }
  };
  // (the far one every other frame: the sun barely moves. Its matrix changes only along with the
  // map: the new one over last frame's map made the far shadows jump while the camera moved)
  bool far_drawn = false;
  // (cached: a map is stale when its square moved (rows 0-1 of the matrix), its depth window slid
  // more than 5%, the casters' levels changed or it is shadow.cache_max_frames old)
  auto stale = [&](const float* want, const float* have, u32 gen, u32 age) {
    for (int i = 0; i < 8; i++) {
      if (want[i] != have[i]) {
        return true;
      }
    }
    return std::abs(want[11] - have[11]) > 0.05f || gen != m_shadow_draws_gen ||
           age >= (u32)std::max(1.f, (float)t_cache_age);
  };
  bool far_due = m_shadow_far_frame++ % 2 == 0 || !m_shadow_far_ready;
  bool mid_due = true;
  if (cache) {
    m_shadow_far_age++;
    m_shadow_mid_age++;
    far_due = !m_shadow_far_ready ||
              stale(far_mvp, m_light_mvp_far, m_shadow_far_gen, m_shadow_far_age);
    mid_due = !m_shadow_mid_ready ||
              stale(mid_mvp, m_light_mvp_mid, m_shadow_mid_gen, m_shadow_mid_age);
    // (both due while flying fast: take turns, one extra pass a frame at most, as without the cache)
    if (far_due && mid_due && mid_on && m_shadow_far_ready && m_shadow_mid_ready &&
        m_shadow_far_frame % 2 == 1) {
      far_due = false;
    }
  }
  if (m_shadow_far_fbo && far_due) {
    m_shadow_far_gen = m_shadow_draws_gen;
    m_shadow_far_age = 0;
    m_shadow_cache_far_n++;
    memcpy(m_light_mvp_far, far_mvp, sizeof(far_mvp));
    m_shadow_far_span_m = 2.f * kShadowFarDepth;
    ENH_TIMED("enh-shadow-far", draw_levels(m_shadow_far_fbo, m_light_mvp_far, true, true));
    m_shadow_far_ready = true;
    far_drawn = true;
  }
  // (the middle one on the frames the far one skips: one extra pass a frame at most. Its matrix
  // only with its map, as the far one's)
  if (mid_on && m_shadow_mid_fbo && mid_due && (!far_drawn || !m_shadow_mid_ready)) {
    m_shadow_mid_gen = m_shadow_draws_gen;
    m_shadow_mid_age = 0;
    m_shadow_cache_mid_n++;
    memcpy(m_light_mvp_mid, mid_mvp, sizeof(mid_mvp));
    m_shadow_mid_span_m = 2.f * kShadowMidDepth;
    ENH_TIMED("enh-shadow-mid", draw_levels(m_shadow_mid_fbo, m_light_mvp_mid, true, true, true));
    m_shadow_mid_ready = true;
  }
  if (near_cache) {
    // the static part when stale (as the far map), then a copy of it plus the trees each frame;
    // the live map's matrix is always the static part's
    m_shadow_near_age++;
    if (!m_shadow_near_static_ready ||
        stale(near_mvp, m_light_mvp_near_static, m_shadow_near_gen, m_shadow_near_age)) {
      m_shadow_near_gen = m_shadow_draws_gen;
      m_shadow_near_age = 0;
      m_shadow_cache_near_n++;
      memcpy(m_light_mvp_near_static, near_mvp, sizeof(near_mvp));
      ENH_TIMED("enh-shadow-near-static",
                draw_levels(m_shadow_near_static_fbo, m_light_mvp_near_static, false, false, false, 1));
      m_shadow_near_static_ready = true;
    }
    memcpy(m_light_mvp, m_light_mvp_near_static, sizeof(m_light_mvp));
    auto near_live = [&]() {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, m_shadow_near_static_fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_shadow_fbo);
      glBlitFramebuffer(0, 0, kRes, kRes, 0, 0, kRes, kRes, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
      draw_levels(m_shadow_fbo, m_light_mvp, true, false, false, 2);
    };
    ENH_TIMED("enh-shadow-near", near_live());
  } else {
    ENH_TIMED("enh-shadow-near", draw_levels(m_shadow_fbo, m_light_mvp, true, false));
  }
  // (how often the far, middle and near static maps were drawn, every ~5 s)
  if (++m_shadow_cache_frames >= 300) {
    lg::info("[enhanced] shadow maps drawn: far {} mid {} near {} of {} frames (cache {}, near cache {})",
             m_shadow_cache_far_n, m_shadow_cache_mid_n, m_shadow_cache_near_n, m_shadow_cache_frames,
             cache ? "on" : "off", near_cache ? "on" : "off");
    m_shadow_cache_frames = m_shadow_cache_far_n = m_shadow_cache_mid_n = m_shadow_cache_near_n = 0;
  }
  glDisableVertexAttribArray(1);
  glBindVertexArray(0);
  // characters only with the sun (or moon) above ~7 degrees: lower, a 2 m character casts a band
  // tens of meters long that follows it around (the game's own short shadow stays)
  if (m_settings.shadow_casters && sun[1] / len > 0.12f) {
    ENH_TIMED("enh-shadow-chars", draw_shadow_casters(render_state, prof));
    g_enhanced_casters.cast = true;
    // (below ~25 degrees the map's character shadow is a long band off the feet: the game's own
    // short one stays under them too, or they float (Spargus street at 18h); 0: never)
    static const EnhancedTune t_keep_game("shadow.game_chars_below_deg", 25.f);
    g_enhanced_casters.keep_game =
        sun[1] / len < std::sin(std::clamp((float)t_keep_game, 0.f, 90.f) * 3.14159265f / 180.f);
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
                                       ScopedProfilerNode& prof,
                                       const float* mvp,
                                       const std::vector<u32>* only,
                                       bool upload) {
  auto& cs = g_enhanced_casters;
  cs.capture = false;  // later merc draws (HUD) don't cast
  if (cs.draws.empty() || (only && only->empty())) {
    return;
  }
  // a bound range always spans 128 bones: room past the last one
  constexpr size_t kBoneBlock = 128 * 8 * 4;  // floats
  const size_t size = (cs.bones.size() + kBoneBlock) * sizeof(float);
  if (!m_caster_bones) {
    glGenBuffers(1, &m_caster_bones);
    glGenVertexArrays(1, &m_caster_vao);
    upload = true;
  }
  if (upload || size > m_caster_bones_size) {
    glBindBuffer(GL_UNIFORM_BUFFER, m_caster_bones);
    if (size > m_caster_bones_size) {
      m_caster_bones_size = size * 3 / 2;
      glBufferData(GL_UNIFORM_BUFFER, m_caster_bones_size, nullptr, GL_DYNAMIC_DRAW);
    }
    glBufferSubData(GL_UNIFORM_BUFFER, 0, cs.bones.size() * sizeof(float), cs.bones.data());
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
  }

  auto& shader = render_state->shaders[ShaderId::ENH_SHADOW_MERC];
  shader.activate();
  const auto id = shader.id();
  const auto& cam = g_enhanced_camera;
  glUniformMatrix4fv(glGetUniformLocation(id, "light_mvp"), 1, GL_TRUE, mvp ? mvp : m_light_mvp);
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
  const size_t n_draws = only ? only->size() : cs.draws.size();
  for (size_t k = 0; k < n_draws; k++) {
    const auto& d = cs.draws[only ? (*only)[k] : k];
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
    glUniform1f(u_alpha, d.tex ? (float)kShadowAlphaRef : -1.f);
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

// Lamps' shadow maps (lights.shadow_maps n, 0 = off): see EnhancedPost.h. The faces follow GL's cube
// map convention, so the composite looks them up with the direction from the lamp, in world space.
void EnhancedPost::do_lamp_shadows(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_lamp_ready = false;
  static const EnhancedTune t_maps("lights.shadow_maps", 4.f);
  static const EnhancedTune t_res("lights.shadow_map_res", 512.f);
  static const EnhancedTune t_faces("lights.shadow_faces_per_frame", 2.f);
  static const EnhancedTune t_near("lights.shadow_near_m", 0.5f);
  static const EnhancedTune t_far("lights.shadow_far_x", 2.0f);
  static const EnhancedTune t_chars("lights.shadow_chars", 1.f);
  static const EnhancedTune t_log("lights.shadow_log", 0.f);
  const int want_n = std::clamp((int)(float)t_maps, 0, kLampSlots);
  const auto& cam = g_enhanced_camera;
  // (the lights' shadows off, Medium and Performance presets: nothing reads the maps)
  if (want_n == 0 || !cam.valid || g_enhanced_menu.blank_world || m_lamp_want.empty() ||
      m_settings.light_shadow <= 0.f) {
    return;
  }
  const int res = std::clamp((int)(float)t_res, 128, 2048);
  if (res != m_lamp_res) {
    for (int k = 0; k < 2; k++) {
      GLuint* t = k ? &m_lamp_tex : &m_lamp_static_tex;
      if (*t) {
        glDeleteTextures(1, t);
      }
      glGenTextures(1, t);
      glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, *t);
      glTexImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, 0, GL_DEPTH_COMPONENT32F, res, res, 6 * kLampSlots, 0,
                   GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MIN_FILTER, k ? GL_LINEAR : GL_NEAREST);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_MAG_FILTER, k ? GL_LINEAR : GL_NEAREST);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
      if (k) {
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_MODE,
                        GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
      }
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP_ARRAY, 0);
    if (!m_lamp_fbo) {
      glGenFramebuffers(1, &m_lamp_fbo);
    }
    m_lamp_res = res;
    for (auto& s : m_lamp_slots) {
      s = LampSlot();
    }
  }
  m_lamp_frame++;
  const float near_u = std::max((float)t_near, 0.05f) * kMeter;

  auto dist2 = [](const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
  };
  // only lamps that stay put (lights.shadow_still_frames in the same place, 2 cm): a light that
  // follows Jak or a moving glow would redraw its static faces every frame and never be ready
  static const EnhancedTune t_still("lights.shadow_still_frames", 30.f);
  // (within lights.shadow_still_m of where it was first seen: the temple braziers' lights jitter by
  // ~20 cm with the flame every second or so; at 2 cm frame to frame they lost their maps, 7 slots
  // redrawn in 5 s with the camera still. The map stays at that first place)
  static const EnhancedTune t_still_m("lights.shadow_still_m", 0.5f);
  const float still_u = std::clamp((float)t_still_m, 0.02f, 2.f) * kMeter;
  const float match2 = std::max(still_u, 0.3f * kMeter) * std::max(still_u, 0.3f * kMeter);
  {
    std::vector<std::pair<std::array<float, 3>, u32>> still;
    const float still2 = still_u * still_u;
    for (const auto& w : m_lamp_want) {
      std::pair<std::array<float, 3>, u32> e = {{w.pos[0], w.pos[1], w.pos[2]}, 1};
      for (const auto& [p, c] : m_lamp_still) {
        if (dist2(p.data(), w.pos) < still2) {
          e = {p, c + 1};
          break;
        }
      }
      still.push_back(e);
    }
    m_lamp_still.swap(still);
  }
  std::vector<LampWant> wanted;
  for (size_t i = 0; i < m_lamp_want.size(); i++) {
    if (m_lamp_still[i].second >= (u32)std::max((float)t_still, 1.f)) {
      wanted.push_back(m_lamp_want[i]);
      memcpy(wanted.back().pos, m_lamp_still[i].first.data(), sizeof(wanted.back().pos));
    }
  }
  // (a little hysteresis: among the strongest few more than there are maps, the lamps whose map is
  // drawn already go first; the order shifts as the camera turns, and the slots thrashed, two static
  // faces redrawn every frame at the port)
  // (lights.shadow_keep_rank k: a lamp that has its map ranks k places higher than it is, so a new
  // one takes its slot only when clearly stronger; fires flicker and swapped places every few
  // frames at the temple's braziers, 6 slots redrawn in 5 s with the camera still at +2)
  {
    static const EnhancedTune t_keep("lights.shadow_keep_rank", 2.f);
    const float keep = std::max((float)t_keep, 0.f);
    auto has_map = [&](const LampWant& w) {
      for (const auto& sl : m_lamp_slots) {
        if (sl.used && sl.faces >= 6 &&
            dist2(sl.pos, w.pos) < match2) {
          return true;
        }
      }
      return false;
    };
    std::vector<std::pair<float, size_t>> rank(wanted.size());
    for (size_t i = 0; i < wanted.size(); i++) {
      rank[i] = {(float)i - (has_map(wanted[i]) ? keep + 0.5f : 0.f), i};
    }
    std::stable_sort(rank.begin(), rank.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<LampWant> sorted;
    sorted.reserve(wanted.size());
    for (const auto& r : rank) {
      sorted.push_back(wanted[r.second]);
    }
    wanted.swap(sorted);
  }
  // a slot for each of the strongest lamps: the one already holding it (same place), else the one
  // left unused longest
  const int n_want = std::min(want_n, (int)wanted.size());
  int assign[kLampSlots];
  bool taken[kLampSlots] = {};
  for (int i = 0; i < n_want; i++) {
    assign[i] = -1;
    for (int s = 0; s < kLampSlots; s++) {
      if (!taken[s] && m_lamp_slots[s].used &&
          dist2(m_lamp_slots[s].pos, wanted[i].pos) < match2) {
        assign[i] = s;
        taken[s] = true;
        break;
      }
    }
  }
  for (int i = 0; i < n_want; i++) {
    if (assign[i] >= 0) {
      continue;
    }
    int best = -1;
    for (int s = 0; s < kLampSlots; s++) {
      if (!taken[s] && (best < 0 || !m_lamp_slots[s].used ||
                        (m_lamp_slots[best].used && m_lamp_slots[s].seen < m_lamp_slots[best].seen))) {
        best = s;
      }
    }
    if (best < 0) {
      break;
    }
    auto& sl = m_lamp_slots[best];
    sl = LampSlot();
    memcpy(sl.pos, wanted[i].pos, sizeof(sl.pos));
    sl.far_u = std::max(wanted[i].radius * std::max((float)t_far, 1.f), near_u * 4.f);
    sl.radius_u = wanted[i].radius;
    sl.used = true;
    m_lamp_assigns++;
    if ((float)t_log > 1.5f) {
      lg::info("[enhanced] lamp slot {} <- {:.1f},{:.1f},{:.1f} r {:.1f} m (rank {} of {})", best,
               sl.pos[0] / kMeter, sl.pos[1] / kMeter, sl.pos[2] / kMeter, sl.radius_u / kMeter, i,
               wanted.size());
    }
    sl.gen = m_shadow_draws_gen;
    assign[i] = best;
    taken[best] = true;
  }
  for (int i = 0; i < n_want; i++) {
    if (assign[i] >= 0) {
      auto& sl = m_lamp_slots[assign[i]];
      sl.seen = m_lamp_frame;
      if (sl.gen != m_shadow_draws_gen) {
        sl.gen = m_shadow_draws_gen;  // the levels changed: drawn again
        sl.faces = 0;
      }
    }
  }

  // the six faces: GL's cube map directions and up vectors, a 90 degree perspective each
  static const float kDir[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                   {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static const float kUp[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1},
                                  {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
  auto face_basis = [&](int face, float* s, float* u) {
    const float* f = kDir[face];
    const float* up = kUp[face];
    s[0] = f[1] * up[2] - f[2] * up[1];
    s[1] = f[2] * up[0] - f[0] * up[2];
    s[2] = f[0] * up[1] - f[1] * up[0];
    u[0] = s[1] * f[2] - s[2] * f[1];
    u[1] = s[2] * f[0] - s[0] * f[2];
    u[2] = s[0] * f[1] - s[1] * f[0];
  };
  auto face_mvp = [&](const LampSlot& sl, int face, float* m) {
    float s[3], u[3];
    face_basis(face, s, u);
    const float* f = kDir[face];
    const float* L = sl.pos;
    const float n = near_u, fa = sl.far_u;
    const float A = -(fa + n) / (fa - n), B = -2.f * fa * n / (fa - n);
    const float v2[4] = {-f[0], -f[1], -f[2], f[0] * L[0] + f[1] * L[1] + f[2] * L[2]};
    m[0] = s[0], m[1] = s[1], m[2] = s[2], m[3] = -(s[0] * L[0] + s[1] * L[1] + s[2] * L[2]);
    m[4] = u[0], m[5] = u[1], m[6] = u[2], m[7] = -(u[0] * L[0] + u[1] * L[1] + u[2] * L[2]);
    for (int c = 0; c < 4; c++) {
      m[8 + c] = A * v2[c] + (c == 3 ? B : 0.f);
      m[12 + c] = -v2[c];
    }
  };

  GLfloat old_clear_depth = 0.f;
  GLint old_depth_func = GL_GEQUAL;
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
  glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.5f, 3.f);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBindFramebuffer(GL_FRAMEBUFFER, m_lamp_fbo);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  glViewport(0, 0, res, res);

  // the static part: a few faces a frame, the strongest lamp's first
  int budget = std::max(1, (int)(float)t_faces);
  int static_faces = 0;
  for (int i = 0; i < n_want && budget > 0; i++) {
    if (assign[i] < 0 || m_lamp_slots[assign[i]].faces >= 6) {
      continue;
    }
    auto& shader = render_state->shaders[ShaderId::ENH_SHADOW];
    shader.activate();
    const auto id = shader.id();
    const GLint u_cut = glGetUniformLocation(id, "enh_cut");
    const GLint u_alpha = glGetUniformLocation(id, "alpha_ref");
    const GLint u_mvp = glGetUniformLocation(id, "light_mvp");
    glUniform1i(glGetUniformLocation(id, "tex_T0"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(m_shadow_vao);
    glEnableVertexAttribArray(0);
    auto& sl = m_lamp_slots[assign[i]];
    sl.live_valid = false;
    while (sl.faces < 6 && budget > 0) {
      glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_lamp_static_tex, 0,
                                assign[i] * 6 + sl.faces);
      glClear(GL_DEPTH_BUFFER_BIT);
      float mvp[16];
      face_mvp(sl, sl.faces, mvp);
      glUniformMatrix4fv(u_mvp, 1, GL_TRUE, mvp);
      for (auto& [lid, draws] : m_shadow_draws) {
        for (auto& d : draws) {
          // (as the near sun map: the small shrubs, not the big plants' second strips)
          if ((d.wind && !m_settings.shadow_casters) || (d.shrub && d.big)) {
            continue;
          }
          glUniform1i(u_cut, d.cut);
          glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
          if (d.own_vertices || d.wind) {
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                                  (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
          } else if (d.shrub) {
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::ShrubGpuVertex),
                                  (void*)0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(tfrag3::ShrubGpuVertex),
                                  (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
          } else {
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex),
                                  (void*)0);
            if (d.tex) {
              glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(tfrag3::PreloadedVertex),
                                    (void*)offsetof(tfrag3::PreloadedVertex, s));
              glEnableVertexAttribArray(1);
            } else {
              glDisableVertexAttribArray(1);
            }
          }
          glUniform1f(u_alpha, !d.tex ? -1.f : d.alpha >= 0.f ? d.alpha : (float)kShadowAlphaRef);
          if (d.tex) {
            glBindTexture(GL_TEXTURE_2D, d.tex);
          }
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
          glDrawElements(d.mode, d.count, GL_UNSIGNED_INT, (void*)(d.first * sizeof(u32)));
          prof.add_draw_call();
        }
      }
      sl.faces++;
      budget--;
      static_faces++;
    }
    glDisableVertexAttribArray(1);
    glBindVertexArray(0);
  }

  // the characters' places this frame (world), by their first bone: the mean of their bones'
  // translations (camera space, back to the world with the camera), the garbage spare slots left out
  const auto& cs = g_enhanced_casters;
  const bool chars = (float)t_chars > 0.5f && m_settings.shadow_casters && !cs.draws.empty();
  std::vector<std::pair<u32, std::array<float, 3>>> places;
  if (chars) {
    for (const auto& in : cs.instances) {
      double acc[3] = {0, 0, 0};
      int cnt = 0;
      for (u32 b = 0; b < in.bone_count; b++) {
        const size_t at = ((size_t)in.first_bone + b * 8) * 4 + 12;
        if (at + 3 > cs.bones.size()) {
          break;
        }
        const float* t = &cs.bones[at];
        if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]) ||
            t[0] * t[0] + t[1] * t[1] + t[2] * t[2] > (2000.f * kMeter) * (2000.f * kMeter)) {
          continue;
        }
        for (int r = 0; r < 3; r++) {
          acc[r] += cam.rot[r][0] * t[0] + cam.rot[r][1] * t[1] + cam.rot[r][2] * t[2];
        }
        cnt++;
      }
      if (cnt) {
        places.push_back({in.first_bone,
                          {cam.trans[0] + (float)(acc[0] / cnt), cam.trans[1] + (float)(acc[1] / cnt),
                           cam.trans[2] + (float)(acc[2] / cnt)}});
      }
    }
  }

  // the live maps: a copy of the static part, then the characters near the lamp in the faces they
  // reach (a 2.5 m sphere around their middle against each face's pyramid)
  int live = 0, char_draws = 0, copied = 0;
  const decltype(places) no_places;
  bool uploaded = false;
  std::vector<u32> only;
  const float rad = 2.5f * kMeter;
  // (characters further than this from the lamp left out: their shadow is long and faint there,
  // and the big Spargus lamps reach ~45 m: every NPC of the street, 700 draws)
  static const EnhancedTune t_chars_m("lights.shadow_chars_m", 12.f);
  const float chars_m = std::max((float)t_chars_m, 1.f) * kMeter;
  // (but the characters near the camera, Jak first, anywhere in the lamp's reach: a brazier 25 m
  // away still throws his long shadow across the temple floor, where the player looks; 0: off)
  static const EnhancedTune t_chars_cam_m("lights.shadow_chars_cam_m", 15.f);
  const float cam_m = std::max((float)t_chars_cam_m, 0.f) * kMeter;
  std::vector<u8> near_cam(places.size(), 0);
  for (size_t k = 0; k < places.size(); k++) {
    const auto& c = places[k].second;
    const float x = c[0] - cam.trans[0], y = c[1] - cam.trans[1], z = c[2] - cam.trans[2];
    near_cam[k] = x * x + y * y + z * z < cam_m * cam_m;
  }
  for (int i = 0; i < n_want; i++) {
    if (assign[i] < 0 || m_lamp_slots[assign[i]].faces < 6) {
      continue;
    }
    const int s_i = assign[i];
    auto& sl = m_lamp_slots[s_i];
    // (the live map holds the static part plus last frame's characters: a face is copied again only
    // where characters were or are drawn, the whole map once after its static part changed)
    auto copy_faces = [&](int first, int count) {
      glCopyImageSubData(m_lamp_static_tex, GL_TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, s_i * 6 + first,
                         m_lamp_tex, GL_TEXTURE_CUBE_MAP_ARRAY, 0, 0, 0, s_i * 6 + first, res, res,
                         count);
      copied += count;
    };
    if (!sl.live_valid) {
      copy_faces(0, 6);
      sl.live_valid = true;
      sl.dirty = 0;
    }
    live++;
    for (int face = 0; face < 6; face++) {
      float s[3], u[3];
      face_basis(face, s, u);
      const float* f = kDir[face];
      only.clear();
      const auto& pl = chars ? places : no_places;
      for (size_t k = 0; k < pl.size(); k++) {
        const auto& [fb, c] = pl[k];
        const float d[3] = {c[0] - sl.pos[0], c[1] - sl.pos[1], c[2] - sl.pos[2]};
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float fz = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        const float fx = d[0] * s[0] + d[1] * s[1] + d[2] * s[2];
        const float fy = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
        const float reach =
            std::min(sl.far_u, near_cam[k] ? std::max(sl.radius_u, chars_m) : chars_m);
        if (dl - rad > reach || fz < -rad || std::abs(fx) > fz + rad * 1.4143f ||
            std::abs(fy) > fz + rad * 1.4143f) {
          continue;
        }
        for (u32 k = 0; k < cs.draws.size(); k++) {
          if (cs.draws[k].first_bone == fb) {
            only.push_back(k);
          }
        }
      }
      const u8 bit = (u8)(1u << face);
      if (only.empty()) {
        if (sl.dirty & bit) {
          copy_faces(face, 1);
          sl.dirty &= (u8)~bit;
        }
        continue;
      }
      copy_faces(face, 1);
      sl.dirty |= bit;
      glBindFramebuffer(GL_FRAMEBUFFER, m_lamp_fbo);
      glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_lamp_tex, 0, s_i * 6 + face);
      glViewport(0, 0, res, res);
      float mvp[16];
      face_mvp(sl, face, mvp);
      draw_shadow_casters(render_state, prof, mvp, &only, !uploaded);
      uploaded = true;
      char_draws += (int)only.size();
    }
  }
  glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 0, 0, 0);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_PRIMITIVE_RESTART);
  glClearDepth(old_clear_depth);
  glDepthFunc(old_depth_func);
  m_lamp_ready = live > 0;
  m_lamp_static_total += static_faces;
  if ((float)t_log > 0.5f && m_lamp_frame % 300 == 0) {
    lg::info("[enhanced] lamp shadows: {} wanted ({} lamps, {} still), {} live, {} static faces "
             "this frame ({} in 300 frames, {} new slots), {} character draws, {} faces copied; "
             "slots {}",
             n_want, m_lamp_want.size(), wanted.size(), live, static_faces, m_lamp_static_total,
             m_lamp_assigns, char_draws, copied,
             [&]() {
               std::string o;
               for (int s = 0; s < kLampSlots; s++) {
                 const auto& sl = m_lamp_slots[s];
                 o += fmt::format("[{} {:.1f},{:.1f},{:.1f} far {:.1f} m faces {}] ", sl.used ? "on" : "--",
                                  sl.pos[0] / kMeter, sl.pos[1] / kMeter, sl.pos[2] / kMeter,
                                  sl.far_u / kMeter, sl.faces);
               }
               return o;
             }());
  }
  if (m_lamp_frame % 300 == 0) {
    m_lamp_static_total = 0;
    m_lamp_assigns = 0;
  }
}

// TAA motion vectors: the characters of this frame (EnhancedCasters) skinned twice, with this frame's
// bones and with last frame's bones of the same model (same name, the nearest place last frame once
// the camera's move is taken out), into an RG16F target: the uv offset to where the pixel was. Only
// where the model is the front surface (its depth against the scene's); the rest stays (64, 0) and
// the TAA reprojects it with the camera, as before. Without them a running Jak or a flying Hellcat
// took the history of whatever was behind them last frame, clipped to the colors around: smeared.
bool EnhancedPost::do_motion(SharedRenderState* render_state,
                             ScopedProfilerNode& prof,
                             int w,
                             int h,
                             const float to_prev[3][3],
                             const float offset[3],
                             bool reproject) {
  auto& cs = g_enhanced_casters;
  using mv_clock = std::chrono::steady_clock;
  auto mv_t = mv_clock::now();
  auto mv_lap = [&](int i) {
    const auto t = mv_clock::now();
    m_mv_cpu[i] += std::chrono::duration<double, std::milli>(t - mv_t).count();
    mv_t = t;
  };
  m_mv_matched = 0;
  m_mv_still = 0;
  m_mv_draws = 0;
  m_mv_models = (int)cs.instances.size();
  // (the models that have draws: a sorted list, not a scan of every draw per model)
  std::vector<u32> drawn_bones;
  drawn_bones.reserve(cs.draws.size());
  for (const auto& d : cs.draws) {
    drawn_bones.push_back(d.first_bone);
  }
  std::sort(drawn_bones.begin(), drawn_bones.end());
  drawn_bones.erase(std::unique(drawn_bones.begin(), drawn_bones.end()), drawn_bones.end());
  // this frame's models and their places (the mean of their bones' translations, view space)
  std::vector<MotionModel> now;
  now.reserve(cs.instances.size());
  for (const auto& in : cs.instances) {
    MotionModel m;
    m.hash = in.hash;
    m.bone_count = in.bone_count;
    m.first_bone = in.first_bone;
    int n = 0;
    for (u32 b = 0; b < in.bone_count; b++) {
      const size_t f = (size_t)(in.first_bone + 8 * b + 3) * 4;  // the matrix's 4th column
      if (f + 2 >= cs.bones.size()) {
        break;
      }
      // (a model's spare bone slots can hold garbage: NaN, or kilometers away)
      const float* t = &cs.bones[f];
      if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]) ||
          std::abs(t[0]) + std::abs(t[1]) + std::abs(t[2]) > 3000.f * kMeter) {
        continue;
      }
      for (int k = 0; k < 3; k++) {
        m.center[k] += t[k];
      }
      n++;
    }
    for (int k = 0; k < 3 && n; k++) {
      m.center[k] /= n;
    }
    // (only models with draws here: a second copy of the Hellcat drew nothing)
    const bool drawn =
        std::binary_search(drawn_bones.begin(), drawn_bones.end(), in.first_bone);
    if (n == 0 || !drawn) {
      m.hash = 0;  // matches nothing
    }
    now.push_back(m);
  }

  // last frame's bones of each model found again (-1: none)
  constexpr float kMatch = 3.f * kMeter;  // 180 m/s: anything faster is a different one
  std::unordered_map<u32, u32> prev_of;  // first bone now -> first bone last frame
  std::unordered_map<u32, u32> count_of;  // first bone now -> its bones
  // A model found again whose own motion on screen is under taa.motion_still_px (every bone,
  // the camera's move taken out; 0 = draw them all) needs no draws: the TAA's camera
  // reprojection is already its motion, within that. A bone's rotation counts as the move of a
  // point 2 m from it. (Spargus 18h: the 284 draws were ~0.35 ms of the post's CPU.)
  static const EnhancedTune t_still("taa.motion_still_px", 0.03f);
  const float still_px = (float)t_still;
  const float fpix = std::max(0.5f * std::abs(m_last_proj[2]) * w, 0.5f * std::abs(m_last_proj[3]) * h);
  constexpr float kMoved = 1e9f;  // (a bone that can't be compared)
  static const EnhancedTune t_log("taa.motion_log", 0.f);
  const float px_cap = (float)t_log > 0.5f ? kMoved : still_px;
  auto moved_px = [&](u32 now_first, u32 prev_first, u32 bone_count) {
    const auto& pb = m_mv_prev_bones;
    float most = 0.f;
    for (u32 b = 0; b < bone_count; b++) {
      const size_t fn = (size_t)(now_first + 8 * b) * 4;
      const size_t fp = (size_t)(prev_first + 8 * b) * 4;
      // (past the end of the list: spare slots no draw reads; props have 101 "bones" for one)
      if (fn + 16 > cs.bones.size() || fp + 16 > pb.size()) {
        break;
      }
      const float* tn = &cs.bones[fn + 12];
      // (spare bone slots with garbage, as the centers skip them: kilometers away, or columns
      // that no model's scale would give)
      bool spare = !std::isfinite(tn[0]) || !std::isfinite(tn[1]) || !std::isfinite(tn[2]) ||
                   std::abs(tn[0]) + std::abs(tn[1]) + std::abs(tn[2]) > 3000.f * kMeter;
      for (int k = 0; k < 12 && !spare; k++) {
        spare = !(std::abs(cs.bones[fn + k]) < 1000.f);
      }
      if (spare) {
        continue;
      }
      float moved = 0.f;  // game units
      for (int col = 0; col < 4; col++) {
        const float* vn = &cs.bones[fn + 4 * col];
        const float* vp = &pb[fp + 4 * col];
        float d = 0.f;
        for (int c = 0; c < 3; c++) {
          float p = col == 3 ? offset[c] : 0.f;
          for (int k = 0; k < 3; k++) {
            p += to_prev[k][c] * vn[k];
          }
          const float diff = std::abs(p - vp[c]);
          if (!std::isfinite(diff)) {
            return kMoved;  // (last frame's slot held garbage)
          }
          d = std::max(d, diff);
        }
        moved = std::max(moved, col == 3 ? d : d * 2.f * kMeter);
      }
      const float z = std::max(std::abs(tn[2]), 1.f * kMeter);
      most = std::max(most, moved * fpix / z);
      if (most > px_cap) {
        break;  // (moving: the rest only matters to the log)
      }
    }
    return most;
  };
  std::vector<float> px_of(now.size(), -1.f);  // (the log's)
  if (reproject && m_mv_prev_ok) {
    std::vector<bool> taken(m_mv_prev.size(), false);
    for (size_t mi = 0; mi < now.size(); mi++) {
      const auto& m = now[mi];
      if (!m.hash) {
        continue;
      }
      float pc[3];
      for (int c = 0; c < 3; c++) {
        pc[c] = offset[c];
        for (int k = 0; k < 3; k++) {
          pc[c] += to_prev[k][c] * m.center[k];
        }
      }
      float best = kMatch * kMatch;
      int bi = -1;
      for (size_t i = 0; i < m_mv_prev.size(); i++) {
        const auto& p = m_mv_prev[i];
        if (p.hash != m.hash || p.bone_count != m.bone_count || taken[i]) {
          continue;
        }
        float d2 = 0.f;
        for (int k = 0; k < 3; k++) {
          d2 += (pc[k] - p.center[k]) * (pc[k] - p.center[k]);
        }
        if (d2 < best) {
          best = d2;
          bi = (int)i;
        }
      }
      if (bi >= 0) {
        taken[bi] = true;
        px_of[mi] = moved_px(m.first_bone, m_mv_prev[bi].first_bone, m.bone_count);
        if (still_px > 0.f && px_of[mi] <= still_px) {
          m_mv_still++;
          continue;
        }
        prev_of[m.first_bone] = m_mv_prev[bi].first_bone;
        count_of[m.first_bone] = m.bone_count;
      }
    }
  }
  m_mv_matched = (int)prev_of.size() + m_mv_still;
  // (tuning taa.motion_log 1: each model, every 2 s: found again or not, its draws, its own
  // motion on screen)
  static int log_frames = 0;
  if ((float)t_log > 0.5f && ++log_frames >= 120) {
    log_frames = 0;
    for (size_t i = 0; i < cs.instances.size(); i++) {
      const auto& in = cs.instances[i];
      int draws = 0;
      for (const auto& d : cs.draws) {
        draws += d.first_bone == in.first_bone;
      }
      lg::info("[enhanced] taa motion model {} bones {} draws {} found {} moved {:.3f} px at {:.1f} {:.1f} {:.1f} m",
               in.name, in.bone_count, draws, prev_of.count(in.first_bone) ? 1 : 0, px_of[i],
               now[i].center[0] / kMeter,
               now[i].center[1] / kMeter, now[i].center[2] / kMeter);
    }
  }

  // the bones go up only when something moves, and only the moving models': this frame's packed
  // into buffer 0, last frame's (kept on the CPU) into buffer 1, each a fresh store (orphaned: no
  // wait on the draws still reading the old one)
  constexpr size_t kBoneBlock = 128 * 8 * 4;  // floats: a bound range always spans 128 bones
  std::unordered_map<u32, std::pair<u32, u32>> packed;  // first bone now -> both places (vec4s)
  mv_lap(0);
  if (!prev_of.empty()) {
    if (!m_mv_bones[0]) {
      glGenBuffers(2, m_mv_bones);
    }
    const u32 align = std::max<u32>(1, cs.alignment);
    static std::vector<float> pack[2];
    pack[0].clear();
    pack[1].clear();
    const std::vector<float>* src[2] = {&cs.bones, &m_mv_prev_bones};
    for (const auto& [now_first, prev_first] : prev_of) {
      const size_t n = (size_t)std::min<u32>(count_of[now_first], 128) * 8;  // vec4s
      const u32 from[2] = {now_first, prev_first};
      u32 at[2];
      for (int i = 0; i < 2; i++) {
        auto& p = pack[i];
        const size_t v = (p.size() / 4 + align - 1) / align * align;
        at[i] = (u32)v;
        p.resize(v * 4, 0.f);
        const size_t b0 = std::min(src[i]->size(), (size_t)from[i] * 4);
        const size_t b1 = std::min(src[i]->size(), b0 + n * 4);
        p.insert(p.end(), src[i]->begin() + b0, src[i]->begin() + b1);
        p.resize((v + n) * 4, 0.f);
      }
      packed[now_first] = {at[0], at[1]};
    }
    for (int i = 0; i < 2; i++) {
      pack[i].resize(pack[i].size() + kBoneBlock, 0.f);
      const size_t size = pack[i].size() * sizeof(float);
      glBindBuffer(GL_UNIFORM_BUFFER, m_mv_bones[i]);
      if (size > m_mv_bones_size[i]) {
        m_mv_bones_size[i] = size * 3 / 2;
      }
      glBufferData(GL_UNIFORM_BUFFER, m_mv_bones_size[i], nullptr, GL_STREAM_DRAW);
      glBufferSubData(GL_UNIFORM_BUFFER, 0, size, pack[i].data());
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
  }
  m_mv_prev_bones.assign(cs.bones.begin(), cs.bones.end());
  m_mv_prev = std::move(now);
  m_mv_prev_ok = true;

  // (taa.motion_stamp, default on: the target is never cleared. Each frame's vectors carry the
  // frame's stamp in b and the TAA takes only this frame's; one full clear when the stamp wraps,
  // every 1024 frames. The clear cost 0.13-0.23 ms of render-thread CPU a frame (Spargus 18h),
  // whatever the color or a second target in turn; taa.motion_stamp 0: RG16F, cleared each frame)
  static const EnhancedTune t_stamp("taa.motion_stamp", 1.f);
  const bool stamped = (float)t_stamp > 0.5f;
  bool fresh = false;
  if (w != m_mv_w || h != m_mv_h || stamped != m_mv_stamped) {
    if (m_mv_fbo) {
      glDeleteFramebuffers(1, &m_mv_fbo);
      glDeleteTextures(1, &m_mv_tex);
    }
    m_mv_w = w;
    m_mv_h = h;
    m_mv_stamped = stamped;
    glGenFramebuffers(1, &m_mv_fbo);
    glGenTextures(1, &m_mv_tex);
    glBindTexture(GL_TEXTURE_2D, m_mv_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, stamped ? GL_RGBA16F : GL_RG16F, w, h, 0,
                 stamped ? GL_RGBA : GL_RG, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_mv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_mv_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    fresh = true;
  }
  mv_lap(1);
  if (prev_of.empty()) {
    return false;  // nothing moves on its own: the TAA skips the target
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_mv_fbo);
  glViewport(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  mv_lap(2);
  if (stamped) {
    m_mv_stamp = (m_mv_stamp >= 1024.f || m_mv_stamp < 1.f) ? 1.f : m_mv_stamp + 1.f;
  } else {
    m_mv_stamp = -1.f;
  }
  if (!stamped || fresh || m_mv_stamp == 1.f) {
    glClearColor(64.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.f, 0.f, 0.f, 0.f);
  }
  mv_lap(3);

  auto& shader = render_state->shaders[ShaderId::ENH_MOTION];
  shader.activate();
  const auto id = shader.id();
  // (the block bindings and locations once per program: set each frame they cost the draws'
  // stage more than the draws)
  static GLuint bound_prog = 0;
  static GLint loc_proj = -1, loc_prev_f = -1, loc_tol = -1, loc_stamp = -1;
  if (bound_prog != id) {
    bound_prog = id;
    glUniformBlockBinding(id, glGetUniformBlockIndex(id, "ub_bones"), 1);
    glUniformBlockBinding(id, glGetUniformBlockIndex(id, "ub_prev_bones"), 2);
    loc_proj = glGetUniformLocation(id, "proj");
    loc_prev_f = glGetUniformLocation(id, "prev_f");
    loc_tol = glGetUniformLocation(id, "depth_tol");
    loc_stamp = glGetUniformLocation(id, "stamp");
    glUniform1i(glGetUniformLocation(id, "tex_depth"), 3);
  }
  glUniform4f(loc_proj, m_last_proj[0], m_last_proj[1], m_last_proj[2], m_last_proj[3]);
  glUniform2f(loc_prev_f, m_prev_camera.fx, m_prev_fy);
  static const EnhancedTune t_tol("taa.motion_depth_tol", 0.03f);
  glUniform1f(loc_tol, (float)t_tol);
  glUniform1f(loc_stamp, std::max(m_mv_stamp, 0.f));
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, m_depth_tex);
  glActiveTexture(GL_TEXTURE0);
  if (!m_caster_vao) {
    glGenVertexArrays(1, &m_caster_vao);
  }
  glBindVertexArray(m_caster_vao);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(5);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  mv_lap(4);
  GLuint last_vertices = 0;
  // (a model's draws in a row on the same buffers go as one multi-draw: taa.motion_batch 0 = one
  // call each)
  static const EnhancedTune t_batch("taa.motion_batch", 1.f);
  const bool batch = (float)t_batch > 0.5f;
  std::vector<GLsizei> counts;
  std::vector<const void*> firsts;
  GLenum batch_mode = GL_TRIANGLES;
  auto flush = [&]() {
    if (counts.empty()) {
      return;
    }
    glMultiDrawElements(batch_mode, counts.data(), GL_UNSIGNED_INT, firsts.data(),
                        (GLsizei)counts.size());
    m_mv_draws++;
    prof.add_draw_call();
    counts.clear();
    firsts.clear();
  };
  const EnhancedCasters::Draw* last = nullptr;
  GLuint last_indices = 0;
  u32 last_bone = UINT32_MAX;
  for (const auto& d : cs.draws) {
    if (!prev_of.count(d.first_bone)) {
      continue;
    }
    if (batch) {
      const GLenum mode = d.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
      if (last && last->vertices == d.vertices && last->indices == d.indices &&
          last->first_bone == d.first_bone && batch_mode == mode) {
        counts.push_back((GLsizei)d.index_count);
        firsts.push_back((const void*)(sizeof(u32) * (size_t)d.first_index));
        last = &d;
        continue;
      }
      flush();
      batch_mode = mode;
      last = &d;
    }
    if (d.vertices != last_vertices) {
      glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
      constexpr GLsizei kStride = sizeof(tfrag3::MercVertex);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, pos));
      glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, kStride,
                            (void*)offsetof(tfrag3::MercVertex, weights[0]));
      glVertexAttribIPointer(5, 4, GL_UNSIGNED_BYTE, kStride,
                             (void*)offsetof(tfrag3::MercVertex, mats[0]));
      last_vertices = d.vertices;
    }
    if (d.indices != last_indices) {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
      last_indices = d.indices;
    }
    if (d.first_bone != last_bone) {
      const auto& at = packed[d.first_bone];
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_mv_bones[0], at.first * 4 * sizeof(float),
                        kBoneBlock * sizeof(float));
      glBindBufferRange(GL_UNIFORM_BUFFER, 2, m_mv_bones[1], at.second * 4 * sizeof(float),
                        kBoneBlock * sizeof(float));
      last_bone = d.first_bone;
    }
    if (batch) {
      counts.push_back((GLsizei)d.index_count);
      firsts.push_back((const void*)(sizeof(u32) * (size_t)d.first_index));
      continue;
    }
    glDrawElements(d.strip ? GL_TRIANGLE_STRIP : GL_TRIANGLES, d.index_count, GL_UNSIGNED_INT,
                   (void*)(sizeof(u32) * d.first_index));
    m_mv_draws++;
    prof.add_draw_call();
  }
  flush();
  glDisable(GL_PRIMITIVE_RESTART);
  glBindVertexArray(0);
  mv_lap(5);
  return true;
}

// char-ground-occlusion: under a roof or at night there is no sun shadow, so the characters stood on
// the floor with no contact darkening. Their lowest point per column around Jak (an orthographic map
// looking up, 256^2): the composite darkens what lies just below them, softly by the height gap.
void EnhancedPost::do_chr_occlusion(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_chr_occ_ready = false;
  static const EnhancedTune t_half("shadow.chr_occ_m", 6.f);
  const float half_m = std::clamp((float)t_half, 0.f, 32.f);
  if (half_m <= 0.f || g_enhanced_casters.draws.empty() || !g_enhanced_camera.valid ||
      !g_enhanced_player.valid) {
    return;
  }
  constexpr int kRes = 256;
  if (!m_chr_occ_fbo) {
    glGenFramebuffers(1, &m_chr_occ_fbo);
    glGenTextures(1, &m_chr_occ_tex);
    glBindTexture(GL_TEXTURE_2D, m_chr_occ_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kRes, kRes, 0, GL_DEPTH_COMPONENT,
                 GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_chr_occ_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_chr_occ_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  // x, z around Jak to -1..1; height: depth 0 at 4 m below him, 1 at 4 m above (the lowest wins)
  constexpr float kM = 4096.f;
  const float r = half_m * kM;
  const float hh = 4.f * kM;
  const auto& pl = g_enhanced_player.pos;
  const float m[16] = {1.f / r, 0.f, 0.f, -pl[0] / r,  //
                       0.f, 0.f, 1.f / r, -pl[2] / r,  //
                       0.f, 1.f / hh, 0.f, -pl[1] / hh,  //
                       0.f, 0.f, 0.f, 1.f};
  memcpy(m_chr_occ_mvp, m, sizeof(m));
  m_chr_occ_depth_m = 8.f;

  GLfloat old_clear_depth = 0.f;
  GLint old_depth_func = GL_GEQUAL;
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &old_clear_depth);
  glGetIntegerv(GL_DEPTH_FUNC, &old_depth_func);
  glDisable(GL_BLEND);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glClearDepth(1.0);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glBindFramebuffer(GL_FRAMEBUFFER, m_chr_occ_fbo);
  glViewport(0, 0, kRes, kRes);
  glClear(GL_DEPTH_BUFFER_BIT);
  glActiveTexture(GL_TEXTURE0);
  draw_shadow_casters(render_state, prof, m_chr_occ_mvp);
  glDisable(GL_PRIMITIVE_RESTART);
  glClearDepth(old_clear_depth);
  glDepthFunc(old_depth_func);
  m_chr_occ_ready = true;
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
  {
    // the nearest lights in the air (volume_lights_on): world -> this frame's view space
    static const EnhancedTune t_air("volume.light_air", 0.06f);
    static const EnhancedTune t_gain("volume.light_gain", 1.f);
    static const EnhancedTune t_halo_r("volume.light_halo_m", 8.f);  // a halo's reach (m), at most
    static const EnhancedTune t_max_r("volume.light_max_m", 60.f);  // lights reaching farther (the sky's): none
    constexpr int kVol = 8;
    float vpos[kVol * 4] = {}, vcol[kVol * 4] = {};
    int nv = 0;
    const auto& sl = g_enhanced_smoke_lights;
    if (volume_lights_on() && m_settings.lights) {
      for (int i = 0; i < sl.count && nv < kVol; i++) {
        float v[3] = {0.f, 0.f, 0.f};
        for (int col = 0; col < 3; col++) {
          for (int row = 0; row < 3; row++) {
            v[col] += (sl.pos[i][row] - cam.trans[row]) * cam.rot[row][col];
          }
        }
        // (only those within 40 m of the camera, plus their reach)
        const float d2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        const float reach = 40.f * 4096.f + sl.pos[i][3];
        // (a halo hugs its lamp: the slums' street lamps reach 27-30 m, as halos they washed a whole wall pale)
        if (d2 > reach * reach || sl.pos[i][3] > t_max_r * 4096.f) {
          continue;
        }
        std::memcpy(vpos + nv * 4, v, sizeof(float) * 3);
        vpos[nv * 4 + 3] = std::min(sl.pos[i][3], (float)t_halo_r * 4096.f);
        std::memcpy(vcol + nv * 4, sl.color[i], sizeof(float) * 3);
        nv++;
      }
    }
    glUniform1i(glGetUniformLocation(id, "vl_count"), nv);
    glUniform4fv(glGetUniformLocation(id, "vl_pos"), kVol, vpos);
    glUniform4fv(glGetUniformLocation(id, "vl_color"), kVol, vcol);
    glUniform2f(glGetUniformLocation(id, "vl_air"), (float)t_air, (float)t_gain);
  }
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  prof.add_draw_call();
  glBindVertexArray(0);
  m_vol_ready = true;
}

// the cloud volume, marched by the composite shader (cloud_pass) for one pixel in every 2x2 into
// a small target, each frame blended into the last one (reprojected): the march's noise averages
// out, at a quarter of the cost. Leaves the composite's target bound and its texture on unit 15.
void EnhancedPost::draw_clouds(GLuint id,
                               int w,
                               int h,
                               bool history,
                               GLuint render_fbo,
                               ScopedProfilerNode& prof,
                               SharedRenderState* render_state) {
  // (tuning "clouds.slim" 1: marched by enh_clouds, the composite's file with only the cloud march
  // compiled in, its uniforms copied from the composite; 0: by the composite itself, as before.
  // Measured 2560x1600, desert 16h, one run: 0.75 -> 0.51 ms, same clouds; default on)
  static const EnhancedTune t_slim("clouds.slim", 1.f);
  const Shader& slim = render_state->shaders[ShaderId::ENH_CLOUDS];
  const bool use_slim = (float)t_slim > 0.5f && slim.okay();
  const int cw = std::max(1, (w + 1) / 2);
  const int ch = std::max(1, (h + 1) / 2);
  if (cw != m_cloud_w || ch != m_cloud_h) {
    if (m_cloud_fbo[0]) {
      glDeleteFramebuffers(2, m_cloud_fbo);
      glDeleteTextures(2, m_cloud_tex);
    }
    m_cloud_w = cw;
    m_cloud_h = ch;
    glGenFramebuffers(2, m_cloud_fbo);
    glGenTextures(2, m_cloud_tex);
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_cloud_tex[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, cw, ch, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_cloud_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_cloud_tex[i], 0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    m_cloud_history = false;
  }
  const int cur = m_cloud_cur;
  glBindFramebuffer(GL_FRAMEBUFFER, m_cloud_fbo[cur]);
  glViewport(0, 0, cw, ch);
  glActiveTexture(GL_TEXTURE15);
  glBindTexture(GL_TEXTURE_2D, m_cloud_tex[1 - cur]);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T15"), 15);
  glUniform1i(glGetUniformLocation(id, "cloud_pass"), 1);
  glUniform4f(glGetUniformLocation(id, "cloud_info"), 0.f, m_cloud_history && history ? 1.f : 0.f,
              0.85f, 0.f);
  if (use_slim) {
    slim.copy_uniforms_from(render_state->shaders[ShaderId::ENH_COMPOSITE]);  // (binds it)
    ENH_TIMED("enh-clouds-slim", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    glUseProgram(id);
  } else {
    ENH_TIMED("enh-clouds", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
  }
  prof.add_draw_call();
  m_cloud_cur = 1 - cur;
  m_cloud_history = true;

  glActiveTexture(GL_TEXTURE15);
  glBindTexture(GL_TEXTURE_2D, m_cloud_tex[cur]);
  glActiveTexture(GL_TEXTURE0);
  // the low sun's rays through the gaps in the clouds at the clouds' size, by enh_cloud_rays (its
  // own program: in the composite their 20 reads a pixel took 1.4 ms at 2560x1600, desert 16h),
  // read back bilinearly on unit 33 (cloud_info.w 1). Tuning "clouds.rays_slim" 0: in the composite
  static const EnhancedTune t_rays_slim("clouds.rays_slim", 1.f);
  const Shader& rays = render_state->shaders[ShaderId::ENH_CLOUD_RAYS];
  const bool use_rays = (float)t_rays_slim > 0.5f && rays.okay();
  if (use_rays) {
    if (cw != m_rays_w || ch != m_rays_h) {
      if (m_rays_fbo) {
        glDeleteFramebuffers(1, &m_rays_fbo);
        glDeleteTextures(1, &m_rays_tex);
      }
      m_rays_w = cw;
      m_rays_h = ch;
      glGenFramebuffers(1, &m_rays_fbo);
      glGenTextures(1, &m_rays_tex);
      glBindTexture(GL_TEXTURE_2D, m_rays_tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, cw, ch, 0, GL_RED, GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_rays_fbo);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_rays_tex, 0);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_rays_fbo);
    glViewport(0, 0, cw, ch);
    rays.copy_uniforms_from(render_state->shaders[ShaderId::ENH_COMPOSITE]);  // (binds it)
    ENH_TIMED("enh-cloud-rays", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
    prof.add_draw_call();
    glUseProgram(id);
    glActiveTexture(GL_TEXTURE0 + 33);
    glBindTexture(GL_TEXTURE_2D, m_rays_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(id, "tex_T33"), 33);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  glViewport(0, 0, w, h);
  glUniform1i(glGetUniformLocation(id, "cloud_pass"), 0);
  glUniform4f(glGetUniformLocation(id, "cloud_info"), 1.f, 0.f, 0.f, use_rays ? 1.f : 0.f);
}

void EnhancedPost::draw_sun_half(GLuint id,
                                 int w,
                                 int h,
                                 GLuint render_fbo,
                                 ScopedProfilerNode& prof,
                                 SharedRenderState* render_state) {
  // the world's sun shadow looks (the map pushed off the surface, the rough one from 2 m toward the
  // sun, the contact march) at one pixel per 2x2 by enh_sun_half, the composite's file with only them
  // compiled in; the composite reads them back on unit 34 with the occlusion's texels and depth
  // weights (sun_half_on 1). Characters keep theirs at full size. At 2560x1600: composite -0.6 ms for
  // a 0.33-0.45 ms pass (desert 16h, Spargus 18h), the picture equal even rendered at 640x360.
  // Tuning "shadow.half" 0: all in the composite, as before
  static const EnhancedTune t_half("shadow.half", 1.f);
  const Shader& half = render_state->shaders[ShaderId::ENH_SUN_HALF];
  // (needs the occlusion's picture: the composite reuses ao_upsample's texels and weights, so the
  // same size, (w + 1) / 2)
  const bool use = (float)t_half > 0.5f && half.okay() && m_ssao_ready;
  glUniform1f(glGetUniformLocation(id, "sun_half_on"), use ? 1.f : 0.f);
  if (!use) {
    return;
  }
  const int hw = (w + 1) / 2;
  const int hh = (h + 1) / 2;
  if (hw != m_sun_half_w || hh != m_sun_half_h) {
    if (m_sun_half_fbo) {
      glDeleteFramebuffers(1, &m_sun_half_fbo);
      glDeleteTextures(1, &m_sun_half_tex);
    }
    m_sun_half_w = hw;
    m_sun_half_h = hh;
    glGenFramebuffers(1, &m_sun_half_fbo);
    glGenTextures(1, &m_sun_half_tex);
    glBindTexture(GL_TEXTURE_2D, m_sun_half_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, hw, hh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_sun_half_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sun_half_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_sun_half_fbo);
  glViewport(0, 0, hw, hh);
  half.copy_uniforms_from(render_state->shaders[ShaderId::ENH_COMPOSITE]);  // (binds it)
  ENH_TIMED("enh-sun-half", glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));
  prof.add_draw_call();
  glUseProgram(id);
  glActiveTexture(GL_TEXTURE0 + 34);
  glBindTexture(GL_TEXTURE_2D, m_sun_half_tex);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(id, "tex_T34"), 34);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
  glViewport(0, 0, w, h);
}
