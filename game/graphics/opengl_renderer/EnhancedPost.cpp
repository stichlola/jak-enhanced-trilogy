#include "EnhancedPost.h"

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
#include "game/graphics/opengl_renderer/EnhancedParticles.h"
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
#include "game/graphics/opengl_renderer/EnhancedPostInternal.h"

using namespace enh_post;


EnhancedCamera g_enhanced_camera;
EnhancedSun g_enhanced_sun;
EnhancedPlayer g_enhanced_player;
std::atomic<float> g_enhanced_l2_trigger{0.f};
std::atomic<float> g_enhanced_board_boost{0.f};
std::atomic<float> g_enhanced_aim{0.f};
std::atomic<float> g_enhanced_char_detail{1.f};
bool g_enhanced_debug_toolbar = false;
EnhancedScene g_enhanced_scene;
EnhancedDust g_enhanced_dust;
EnhancedPlayWeather g_enhanced_play_weather;
std::atomic<bool> g_enhanced_balance{false};
EnhancedBeds g_enhanced_beds;
std::atomic<int> g_enhanced_thunder{0};
EnhancedPushers g_enhanced_pushers;
EnhancedBlastPush g_enhanced_blast_push;
EnhancedSmokeLights g_enhanced_smoke_lights;
EnhancedMerc g_enhanced_merc;
EnhancedWind g_enhanced_wind;
float g_enhanced_parallax = 0.f;  // (see EnhancedMaterials.h)
bool g_enhanced_wall_notile = false;
EnhancedCasters g_enhanced_casters;
EnhancedLights g_enhanced_lights;
EnhancedGameLights g_enhanced_game_lights;
EnhancedExplosions g_enhanced_explosions;
EnhancedStrike g_enhanced_strike;
EnhancedHeadlights g_enhanced_headlights;
EnhancedFires g_enhanced_fires;
EnhancedMenu g_enhanced_menu;
EnhancedBigmap g_enhanced_bigmap;

// a tiling picture from custom_assets/<game>/scenery (repeating, mipmapped) and its average color;
// 0 when the file isn't there
EnhancedTerrain g_enhanced_terrain;

EnhancedTimeOfDay g_enhanced_tod;
float g_enhanced_render_scale = 1.f;
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
  const GLenum all[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
  glDrawBuffers(g_enhanced_normals.albedo ? 3 : 2, all);
  m_on = true;
}

EnhancedNormalTarget::~EnhancedNormalTarget() {
  if (m_on) {
    const GLenum color = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &color);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }
}

bool g_enhanced_lights_private = false;

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
  // (the trilogy's start menu is decided as the runtime boots: ee_runner in runtime.cpp)
  // the game played last, for the launcher to boot it next time
  try {
    // (OpenGOAL's own folder, the parent of the games' ones: %APPDATA%\OpenGOAL)
    auto dir = file_util::get_user_settings_dir(g_game_version).parent_path().parent_path();
    file_util::create_dir_if_needed(dir);
    file_util::write_text_file(dir / "enhanced-last-game.txt",
                               std::to_string((int)g_game_version));
  } catch (std::exception& e) {
    lg::error("[enhanced] could not write the last game: {}", e.what());
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
    // (e.g. 2:ctysluma-alley, 1:23), on the game's thread (kmachine.cpp, works without a renderer)
    g_enhanced_menu.open = true;
  }
  if (const char* ls = std::getenv("JAK_ENHANCED_LIGHT_SHADOWS")) {
    // how dark the dynamic lights' shadows are, 0 = none
    m_settings.light_shadow = (float)std::atof(ls);
  }
  if (const char* relief = std::getenv("JAK_ENHANCED_RELIEF")) {
    // the textures' small bumps under the sun, 0 = off, 2 = left half only (A/B)
    m_settings.relief = (float)std::atof(relief);
  }
  if (const char* px = std::getenv("JAK_ENHANCED_PARALLAX")) {
    // the relief on rock and walls near the camera, 0 = off
    m_settings.parallax = (float)std::atof(px);
  }
  if (const char* nt = std::getenv("JAK_ENHANCED_WALL_NOTILE")) {
    // walls that repeat breaking their repeats up, 0 = off
    m_settings.wall_notile = std::atoi(nt) != 0;
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
  if (const char* fx = std::getenv("JAK_ENHANCED_FX_DETAIL")) {
    m_settings.fx_detail = (float)std::atof(fx);
  }
  if (const char* motes = std::getenv("JAK_ENHANCED_MOTES")) {
    // fireflies and pollen: 0 off, else their amount; "force" shows them anywhere (screenshots)
    m_settings.motes = std::strcmp(motes, "force") == 0 ? 1.f : (float)std::atof(motes);
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
  for (auto* t : {&m_start_tex[0], &m_start_tex[1], &m_start_tex[2], &m_start_text[0],
                  &m_start_text[1], &m_start_text[2], &m_start_text[3]}) {
    if (*t) {
      glDeleteTextures(1, t);
      *t = 0;
    }
  }
  // the casters are global and outlive the renderer (a game switch boots the next game in the
  // same process): their buffer copies go with it, or the next game's Merc2 copies into dead names
  for (auto& c : g_enhanced_casters.mod_copies) {
    glDeleteBuffers(1, &c.buffer);
  }
  g_enhanced_casters = EnhancedCasters();
  if (m_adapt_pbo[0]) {
    glDeleteBuffers(2, m_adapt_pbo);
    m_adapt_pbo[0] = m_adapt_pbo[1] = 0;
  }
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
  if (m_cloud_fbo[0]) {
    glDeleteFramebuffers(2, m_cloud_fbo);
    glDeleteTextures(2, m_cloud_tex);
  }
  if (m_rays_fbo) {
    glDeleteFramebuffers(1, &m_rays_fbo);
    glDeleteTextures(1, &m_rays_tex);
  }
  if (m_noise_tex) {
    glDeleteTextures(1, &m_noise_tex);
  }
  if (m_shadow_far_fbo) {
    glDeleteFramebuffers(1, &m_shadow_far_fbo);
    glDeleteTextures(1, &m_shadow_far_tex);
    m_shadow_far_fbo = m_shadow_far_tex = 0;
  }
  if (m_shadow_mid_fbo) {
    glDeleteFramebuffers(1, &m_shadow_mid_fbo);
    glDeleteTextures(1, &m_shadow_mid_tex);
    m_shadow_mid_fbo = m_shadow_mid_tex = 0;
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

// Reads the GPU time of the frame drawn four frames ago (no waiting: the result is long ready),
// smooths it, and moves the auto quality level: one more pass set aside after 1.5 s over the
// 60 fps budget, one back after 6 s with room for it. Then starts timing this frame.
void EnhancedPost::update_auto_quality() {
  if (!m_gpu_query[0][0]) {
    glGenQueries(12, &m_gpu_query[0][0]);
  }
  const int slot = m_gpu_query_slot;
  if (m_gpu_query_used[slot]) {
    GLint ready = 0;
    glGetQueryObjectiv(m_gpu_query[slot][2], GL_QUERY_RESULT_AVAILABLE, &ready);
    if (ready) {
      GLuint64 t0 = 0, t1 = 0;
      glGetQueryObjectui64v(m_gpu_query[slot][0], GL_QUERY_RESULT, &t0);
      glGetQueryObjectui64v(m_gpu_query[slot][1], GL_QUERY_RESULT, &t1);
      GLuint64 t2 = 0;
      glGetQueryObjectui64v(m_gpu_query[slot][2], GL_QUERY_RESULT, &t2);
      const float post = (float)(t2 - t1) / 1e6f;
      if (post > 0.f && post < 200.f) {
        m_gpu_post_ms = m_gpu_post_ms <= 0.f ? post : m_gpu_post_ms * 0.95f + post * 0.05f;
      }
      const float ms = (float)(t2 - t0) / 1e6f;
      if (ms > 0.f && ms < 200.f) {
        m_gpu_ms = m_gpu_ms <= 0.f ? ms : m_gpu_ms * 0.95f + ms * 0.05f;
      }
      const float lag = (float)((double)(GLint64)t0 - (double)m_gpu_cpu_t0[slot]) / 1e6f;
      if (m_gpu_cpu_t0[slot] && lag > -5.f && lag < 200.f) {
        m_gpu_lag_ms = m_gpu_lag_ms * 0.95f + std::max(lag, 0.f) * 0.05f;
      }
    }
    m_gpu_query_used[slot] = false;
  }
  const double now =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  // (with vsync the GPU waits for the screen inside the frame when it has time to spare: the
  // frame's GPU time is only true when it can't keep up. The post pass has no waits in it.)
  if (m_last_frame > 0.0) {
    const float dt = (float)(now - m_last_frame) * 1000.f;
    if (dt > 0.f && dt < 200.f) {
      m_frame_ms = m_frame_ms * 0.95f + dt * 0.05f;
    }
  }
  m_last_frame = now;
  // slow: under ~58 fps for 3 s in a row, but not under 40 (a level loading: the CPU, not the
  // graphics card, and the GPU's clock then counts the waits for it), nor in the 10 s after such
  // frames: the first seconds after a boot or a load run at 52-55 fps (streaming, first draws)
  // and a pass set aside then stayed aside for good (the desert's mist volume, at 15.5 ms a frame
  // after; the whole noon came out ~8% brighter in one run and not in the next)
  if (m_frame_ms >= 25.f || m_warm_since <= 0.0) {
    m_warm_since = now;
  }
  const bool under = m_frame_ms > 17.2f && m_frame_ms < 25.f && now - m_warm_since > 10.0;
  if (!under) {
    m_slow_since = -1.0;
  } else if (m_slow_since < 0.0) {
    m_slow_since = now;
  }
  if (m_frame_ms <= 17.2f && m_fast_since <= 0.0) {
    m_fast_since = now;
  } else if (m_frame_ms > 17.2f) {
    m_fast_since = 0.0;
  }
  const bool slow = m_slow_since >= 0.0 && now - m_slow_since > 3.0;
  if (slow && m_gpu_ms > 0.f) {
    m_gpu_other_ms = std::max(0.f, m_gpu_ms - m_gpu_post_ms);
  }
  if (m_level_cost_pending && now - m_auto_changed > 1.5) {
    // what the level just entered saved
    m_level_cost[m_auto_level] = std::max(0.3f, m_level_post[m_auto_level] - m_gpu_post_ms);
    m_level_cost_pending = false;
  }
  // (auto.lag_gate 1: which part holds the frame back is read from how far the GPU runs behind
  // the CPU, not from its timestamps: with time to spare the driver lowers the clock and parks
  // the slack inside the post (the TAA's last blit read ~8 ms at desert-start, 0.1 ms when
  // busy), so "GPU 16 ms" also came from frames the CPU was late for, and the post never looked
  // cheap enough to bring a pass back. Behind by several ms = the graphics card is the limit;
  // under ~2 ms = it waited for the CPU, there's room. 0 = the old timestamp rule.)
  static const EnhancedTune t_lag_gate("auto.lag_gate", 1.f);
  const bool lag_gate = (float)t_lag_gate > 0.5f;
  const bool gpu_bound = lag_gate ? m_gpu_lag_ms > 4.f : m_gpu_ms > 16.f;
  if (m_gpu_lag_ms >= 2.f || m_frame_ms > 17.2f) {
    m_gpu_room_since = 0.0;
  } else if (m_gpu_room_since <= 0.0) {
    m_gpu_room_since = now;
  }
  if (!m_settings.auto_quality) {
    m_auto_level = 0;
  } else if (slow && gpu_bound && m_auto_level < kAutoLevels - 1 &&
             now - m_auto_changed > 1.5) {
    // the graphics card is what holds the frame back: one more pass aside (and if a step up
    // brought this on within 20 s, the next try waits twice as long, at most 2 minutes)
    if (now - m_auto_up_at < 20.0) {
      m_auto_up_wait = std::min(120.0, m_auto_up_wait * 2.0);
    } else if (now - m_auto_up_at > 60.0) {
      m_auto_up_wait = 10.0;
    }
    m_auto_level++;
    m_level_post[m_auto_level] = m_gpu_post_ms;
    m_level_cost_pending = true;
    m_auto_changed = now;
    lg::info("[enhanced] auto quality: {:.1f} fps, GPU {:.1f} ms a frame, {:.1f} ms behind, level {}",
             1000.f / m_frame_ms, m_gpu_ms, m_gpu_lag_ms, m_auto_level);
  } else if (m_auto_level > 0 && !m_level_cost_pending && now - m_auto_changed > 6.0 &&
             (lag_gate ? (m_gpu_room_since > 0.0 && now - m_gpu_room_since > m_auto_up_wait &&
                          now - m_auto_changed > m_auto_up_wait)
                       : (m_gpu_other_ms + m_gpu_post_ms + m_level_cost[m_auto_level] < 15.3f ||
                          (m_fast_since > 0.0 && now - m_fast_since > 60.0 &&
                           now - m_auto_changed > 60.0)))) {
    m_auto_up_at = now;
    // the frame would still fit with that pass back (or a minute at 60 fps: worth a try, the
    // estimate may come from a heavier place)
    m_auto_level--;
    m_auto_changed = now;
    lg::info("[enhanced] auto quality: room for {:.1f} ms more, level {}",
             m_level_cost[m_auto_level + 1], m_auto_level);
  }
  // each level also brings the detailed scenery models in closer (the far versions are cheap);
  // the last two draw the game at fewer pixels (85%, 72% a side), stretched to the window
  // (tuning.json: "auto": {"scenery_scale": [...], "render_scale": [...]}, one per level)
  static const EnhancedTune kSceneryScale("auto.scenery_scale",
                                          {1.f, 0.85f, 0.7f, 0.55f, 0.55f, 0.55f});
  static const EnhancedTune kRenderScale("auto.render_scale", {1.f, 1.f, 1.f, 1.f, 0.85f, 0.72f});
  static_assert(kAutoLevels == 6, "auto.scenery_scale and auto.render_scale have 6 levels");
  // (test switches: JAK_ENHANCED_AUTO_MIN=n / JAK_ENHANCED_AUTO_MAX=n keep the level at n or
  // above / below; both for a fixed level while measuring)
  static const int auto_min = [] {
    const char* env = std::getenv("JAK_ENHANCED_AUTO_MIN");
    return env ? std::clamp(std::atoi(env), 0, kAutoLevels - 1) : 0;
  }();
  static const int auto_max = [] {
    const char* env = std::getenv("JAK_ENHANCED_AUTO_MAX");
    return env ? std::clamp(std::atoi(env), 0, kAutoLevels - 1) : kAutoLevels - 1;
  }();
  m_auto_level = std::clamp(m_auto_level, auto_min, std::max(auto_min, auto_max));
  // the passes it sets aside or brings back fade over a second: the shade jumped in one frame
  // without the indirect light
  auto fade = [&](float& f, bool on) {
    f = std::clamp(f + (on ? 1.f : -1.f) * std::min(m_frame_ms, 50.f) / 1000.f, 0.f, 1.f);
  };
  fade(m_fade_volume, m_auto_level < 1);
  fade(m_fade_gi, m_auto_level < 2);
  fade(m_fade_rays, m_auto_level < 3);
  g_enhanced_scenery_scale = kSceneryScale[std::clamp(m_auto_level, 0, kAutoLevels - 1)];
  g_enhanced_render_scale = kRenderScale[std::clamp(m_auto_level, 0, kAutoLevels - 1)];
  glQueryCounter(m_gpu_query[slot][0], GL_TIMESTAMP);
  glGetInteger64v(GL_TIMESTAMP, &m_gpu_cpu_t0[slot]);
  m_gpu_query_open = true;
  ++pass_timer_frame();
}

bool enh_post::pass_timers_on() {
  static const EnhancedTune t_on("debug.pass_timers", 1.f);
  return t_on > 0.5f;
}

// each timed pass's mean GPU time since the last call (ms), "shadow-map 2.10 ssao 0.80 ..."; the
// sums start over. Under vsync the GPU clocks down to just make the frame: compare shares, or
// measure with JAK_ENHANCED_PROFILE for absolute costs
std::string EnhancedPost::pass_times() {
  std::string out;
  for (auto* t : pass_timers()) {
    if (t->n > 0) {
      const char* nm = std::strncmp(t->name, "enh-", 4) == 0 ? t->name + 4 : t->name;
      out += fmt::format("{}{} {:.2f}", out.empty() ? "" : " ", nm, t->sum_ms / t->n);
    }
    t->sum_ms = 0.0;
    t->n = 0;
  }
  return out;
}

void EnhancedPost::begin_frame(ShaderLibrary& shaders, int w, int h) {
  // the dev loop: tuning.json saved since, shaders saved since or asked for (EnhancedTune.h)
  enhanced_tune_poll();
  shaders.hot_reload();
  m_fx_count++;  // (capture_fx: copies from this frame)
  if (g_enhanced_menu.hints_dirty.exchange(false)) {
    save_menu_settings();  // a one-time hint was shown (pc-enhanced-hint-once)
  }
  g_enhanced_char_detail = m_settings.char_detail;
  {
    // (test switch "debug": {"cpu_burn": ms}: the renderer's thread busy that long each frame,
    // a CPU-bound frame for testing the auto quality)
    static const EnhancedTune t_cpu_burn("debug.cpu_burn", 0.f);
    if ((float)t_cpu_burn > 0.f) {
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::microseconds((int)(std::min((float)t_cpu_burn, 50.f) * 1000.f));
      while (std::chrono::steady_clock::now() < until) {
      }
    }
  }
  update_auto_quality();
  g_enhanced_fx.detail = m_settings.enabled ? m_settings.fx_detail : 0.f;
  g_enhanced_fx.time = (float)m_time;
  {
    const float* d = g_enhanced_sun.dir;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const bool up = g_enhanced_sun.valid && len > 1e-6f && d[1] / len > 0.02f;
    for (int k = 0; k < 3 && up; k++) {
      g_enhanced_fx.sun[k] = d[k] / len;
    }
    g_enhanced_fx.sun[3] = up ? 1.f - std::max(m_night, night_darkness()) : 0.f;
  }
  g_enhanced_lights.count = 0;  // the glows of this frame add themselves
  // the characters of this frame add themselves (Merc2), for the sun shadow map
  g_enhanced_casters.bones.clear();
  g_enhanced_casters.draws.clear();
  g_enhanced_casters.instances.clear();
  g_enhanced_casters.next_mod_copy = 0;
  g_enhanced_casters.capture =
      m_settings.enabled && m_settings.shadow_map && m_settings.shadow_casters;
  g_enhanced_normals.enabled = m_settings.enabled && m_settings.geometry_normals;
  // (the level's albedo as a third target: tuning lights.albedo 1; off until proven)
  static const EnhancedTune t_albedo("lights.albedo", 0.f);
  g_enhanced_normals.albedo = g_enhanced_normals.enabled && (float)t_albedo > 0.5f;
  if (g_enhanced_normals.enabled && g_enhanced_normals.fbo) {
    // a = 0: no normal (the post pass rebuilds one from depth there)
    GLint prev = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_enhanced_normals.fbo);
    const GLenum all[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(g_enhanced_normals.albedo ? 3 : 2, all);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float zero[4] = {0.f, 0.f, 0.f, 0.f};
    glClearBufferfv(GL_COLOR, 1, zero);
    if (g_enhanced_normals.albedo) {
      glColorMaski(2, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearBufferfv(GL_COLOR, 2, zero);
    }
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

// The game's sky as it drew it (sky bucket, far ocean), before the levels: what the levels drew
// over it later without depth (the palace's honeycomb glass, grates) is what differs from it. The
// game's own painted clouds and the mountains' edges don't: they kept grey halos and wisps over
// the Enhanced sky when that was found by local contrast instead
void EnhancedPost::capture_sky(GLuint render_fbo, int w, int h) {
  m_sky_frame = 0;
  // (by night too: the game's own sky stays then, but the composite smooths its painted cloud cards
  // away, and with them the lights drawn over the sky: Haven's tower beacon and searchlights)
  if (!m_settings.enabled) {
    return;
  }
  if (w != m_sky_w || h != m_sky_h) {
    if (m_sky_fbo) {
      glDeleteFramebuffers(1, &m_sky_fbo);
      glDeleteTextures(1, &m_sky_tex);
    }
    m_sky_w = w;
    m_sky_h = h;
    glGenFramebuffers(1, &m_sky_fbo);
    glGenTextures(1, &m_sky_tex);
    GLint act = 0, old = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    glBindTexture(GL_TEXTURE_2D, m_sky_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, old);
    glActiveTexture(act);
    GLint draw = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_sky_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_sky_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  }
  GLint read = 0, draw = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_sky_fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
  m_sky_frame = m_fx_count + 1;
}

void EnhancedPost::capture_fx(GLuint render_fbo, int w, int h, int which) {
  // (JAK_ENHANCED_FX=0, or tuning.json "fx": {"off": 1} in a running game: off, the light effects
  // graded with the scene as before)
  static const bool env_off = [] {
    const char* e = std::getenv("JAK_ENHANCED_FX");
    return e && e[0] == '0';
  }();
  static const EnhancedTune t_off("fx.off", 0.f);
  g_enhanced_fx_blend_mark = false;
  if (which == 0) {
    m_fx_marked = false;
  }
  // (nor at full night: the composite leaves the effects in the grade then, the copies unused)
  if (!m_settings.enabled || env_off || t_off > 0.5f || m_night >= 0.999f || which < 0 || which > 1) {
    return;
  }
  // (the GL state as the bucket renderers left it, put back after: the game's direct renderers keep
  // their textures bound on units 20+ and don't bind them again while their state is unchanged. A
  // copy that unbound the active unit's texture dropped a full-screen pass of the desert's noon,
  // and the whole picture came out 8% brighter)
  GLint act = 0, old_tex = 0, old_read = 0, old_draw = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  // (half floats while the frame is, post.hdr_scene: an additive pool over a bright floor past 1 left
  // "after - before" its clip contour, a jagged edge)
  static const EnhancedTune t_hdr_scene("post.hdr_scene", 0.f);
  static bool fx_hdr = false;
  const bool hdr_scene = (float)t_hdr_scene > 0.5f;
  if (w != m_fx_w || h != m_fx_h || hdr_scene != fx_hdr) {
    if (m_fx_fbo[0]) {
      glDeleteFramebuffers(2, m_fx_fbo);
      glDeleteTextures(2, m_fx_tex);
    }
    m_fx_w = w;
    m_fx_h = h;
    fx_hdr = hdr_scene;
    glGenFramebuffers(2, m_fx_fbo);
    glGenTextures(2, m_fx_tex);
    // (with mips: how far an effect spreads tells a shot or a flame from a wash of light over the
    // whole room, which stays in the scene's grade)
    int levels = 1;
    while ((std::max(w, h) >> levels) > 0) {
      levels++;
    }
    for (int i = 0; i < 2; i++) {
      glBindTexture(GL_TEXTURE_2D, m_fx_tex[i]);
      glTexStorage2D(GL_TEXTURE_2D, levels, hdr_scene ? GL_RGBA16F : GL_RGBA8, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glBindFramebuffer(GL_FRAMEBUFFER, m_fx_fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fx_tex[i], 0);
    }
    m_fx_frame[0] = m_fx_frame[1] = 0;
  }
  // (resolving msaa, as the scene's own copy)
  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fx_fbo[which]);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, m_fx_tex[which]);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, old_tex);
  // before the sprites: the frame's alpha to 1, so blended dust and smoke can take their coverage out
  // of it (EnhancedSpriteFx::mark_blended): added light never lowers it, and the composite keeps
  // what was dust out of the light effects (a grey puff glowed as an orange orb by day).
  // (tuning.json "fx": {"blend_mark": 0}: off)
  static const EnhancedTune t_mark("fx.blend_mark", 1.f);
  g_enhanced_fx_blend_mark = which == 0 && t_mark > 0.5f;
  if (g_enhanced_fx_blend_mark) {
    GLboolean mask[4];
    glGetBooleani_v(GL_COLOR_WRITEMASK, 0, mask);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, render_fbo);
    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    const float one[4] = {0.f, 0.f, 0.f, 1.f};
    glClearBufferfv(GL_COLOR, 0, one);
    glColorMaski(0, mask[0], mask[1], mask[2], mask[3]);
    if (scissor) {
      glEnable(GL_SCISSOR_TEST);
    }
  }
  if (which == 0) {
    m_fx_marked = g_enhanced_fx_blend_mark;
  }
  glActiveTexture(act);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
  m_fx_frame[which] = m_fx_count + 1;
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

// A far glow this frame: the lamp seen at the same spot, kept up to date (the brightest of its
// glows), or a new one (a shot moving on leaves short-lived ones, dropped as they age)
void EnhancedPost::remember_lamp(const float* pos, float size, const float* color) {
  constexpr float kSame = 2.f * kMeter;
  for (auto& l : m_lamps) {
    float dx = l.pos[0] - pos[0], dy = l.pos[1] - pos[1], dz = l.pos[2] - pos[2];
    if (dx * dx + dy * dy + dz * dz < kSame * kSame) {
      bool fresh = l.last != m_time;
      l.last = m_time;
      if (fresh || color[0] + color[1] + color[2] > l.color[0] + l.color[1] + l.color[2]) {
        l.size = size;
        std::memcpy(l.color, color, sizeof(float) * 3);
      }
      return;
    }
  }
  // (full: the oldest-seen goes)
  if (m_lamps.size() >= 512) {
    auto oldest = std::min_element(m_lamps.begin(), m_lamps.end(),
                                   [](const auto& a, const auto& b) { return a.last < b.last; });
    m_lamps.erase(oldest);
  }
  LampMemory l;
  std::memcpy(l.pos, pos, sizeof(float) * 3);
  l.size = size;
  std::memcpy(l.color, color, sizeof(float) * 3);
  l.first = l.last = m_time;
  m_lamps.push_back(l);
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
    if (!kFogOff) {
      ImGui::SliderFloat("Fog density (/m)", &m_settings.fog_density, 0.f, 0.01f, "%.4f");
      ImGui::SliderFloat("Fog height falloff (/m)", &m_settings.fog_height, 0.f, 0.2f);
      ImGui::SliderFloat("Fog amount", &m_settings.fog_amount, 0.f, 1.f);
    }
    ImGui::SliderFloat("Heat haze", &m_settings.haze_strength, 0.f, 0.005f, "%.4f");
    if (!kFogOff) {
      ImGui::Checkbox("Volumetric mist", &m_settings.volume);
      ImGui::SliderFloat("Mist density (/m)", &m_settings.mist_density, 0.f, 0.1f, "%.4f");
      ImGui::SliderFloat("Mist height (m)", &m_settings.mist_height, 0.5f, 30.f);
      ImGui::SliderFloat("Mist distance (m)", &m_settings.mist_distance, 20.f, 400.f);
      ImGui::SliderFloat("Mist ground below camera (m)", &m_settings.mist_ground, -5.f, 20.f);
      ImGui::SliderFloat("Mist patches", &m_settings.mist_noise, 0.f, 1.f);
      ImGui::SliderFloat("Mist sunlight", &m_settings.mist_sun, 0.f, 4.f);
      ImGui::SliderFloat("Mist forward scattering", &m_settings.mist_aniso, 0.f, 0.95f);
    }
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
    ImGui::SliderFloat("Eye adaptation", &m_settings.eye_adapt, 0.f, 1.f);
    ImGui::SliderFloat("Character detail", &m_settings.char_detail, 0.f, 1.f);
    ImGui::SliderFloat("Relief (parallax)", &m_settings.parallax, 0.f, 2.f);
    ImGui::Checkbox("Walls repeat less", &m_settings.wall_notile);
    ImGui::SliderFloat("Effects detail", &m_settings.fx_detail, 0.f, 1.f);
    ImGui::SliderFloat("Split colors on a hit", &m_settings.hit_chroma, 0.f, 1.f);
    if (!kFogOff) {
      ImGui::SliderFloat("Morning fog", &m_settings.dawn_fog, 0.f, 1.f);
    }
    ImGui::SliderFloat("Rays through clouds", &m_settings.cloud_rays, 0.f, 1.f);
    ImGui::SliderFloat("Drops on the lens", &m_settings.lens_drops, 0.f, 1.f);
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

void EnhancedPost::ground_puff(const float* pos, float size, float life, bool step) {
  if (m_settings.prints <= 0.f || std::max(m_wet, m_puddle) > 0.3f) {
    return;
  }
  const bool snow = m_snow > 0.3f;
  // a step raises dust only in the desert sand (or snow); a landing raises a little anywhere dry
  if (step && !snow && !m_pond) {
    return;
  }
  Puff f;
  std::memcpy(f.pos, pos, sizeof(f.pos));
  f.size = (m_pond || snow) ? size : size * 0.6f;
  f.kind = snow ? 2 : 0;
  f.seed = (float)(std::rand() % 1000) / 1000.f;
  f.life = snow ? life * 0.8f : life;
  f.time = m_time;
  m_puffs.push_back(f);
}

std::vector<EnhancedLight> enhanced_lights() {
  return g_manual_lights[game_index()];
}

void enhanced_set_lights(const std::vector<EnhancedLight>& lights, bool save) {
  g_manual_lights[game_index()] = lights;
  if (save) {
    save_manual_lights();
  }
}
