#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "common/common_types.h"

/*!
 * Jak Enhanced Trilogy: values the game hands to the renderer through the kernel (kmachine.cpp).
 */

// The key light ("sun") direction of the current time of day, world space, as the game's
// time-of-day light-group 0 dir0. Set every frame by pc-enhanced-set-sun.
struct EnhancedSun {
  float dir[3] = {0.f, 1.f, 0.f};
  bool valid = false;
};
extern EnhancedSun g_enhanced_sun;

// Jak's position (world, game units), set every frame by pc-enhanced-set-player: grass bends away
// from it.
struct EnhancedPlayer {
  float pos[3] = {0.f, 0.f, 0.f};
  int state = 0;  // 0 elsewhere, 1 on foot on soft ground (sand, dirt, snow), 2 driving
  bool valid = false;
};
extern EnhancedPlayer g_enhanced_player;

// The level Jak is in, the hour (0-24, fractional) and whether a cutscene plays, set every frame by
// pc-enhanced-set-scene: grading per area, weather, cutscene depth of field
struct EnhancedScene {
  std::string level;
  float hour = 12.f;
  bool movie = false;
  bool valid = false;
  std::mutex mutex;
};
extern EnhancedScene g_enhanced_scene;

// The game's own sandstorm (desert-dust-storm.gc), set every frame while it runs by
// pc-enhanced-set-dust: intensity 0..1, the wind's direction (world xz) and speed (game units/s).
// Stale after a moment without updates (the storm process is gone).
struct EnhancedDust {
  std::atomic<float> intensity{0.f};
  std::atomic<float> wind_x{0.f}, wind_z{1.f}, wind_vel{0.f};
  std::atomic<int> frame{-1000};  // the renderer's frame counter at the last update
  std::atomic<int> now{0};        // the renderer's frame counter
};
extern EnhancedDust g_enhanced_dust;

// Thunder after a lightning bolt, for the game to play (pc-enhanced-thunder): 0 none, else
// kind * 1000 + the bolt's azimuth in degrees (0..359, world: atan2(x, z))
extern std::atomic<int> g_enhanced_thunder;

// The enemies, passers-by and vehicles near Jak (world, game units; w = the radius the grass bends
// in), set every frame by pc-enhanced-set-pushers
struct EnhancedPushers {
  static constexpr int kMax = 8;
  float pos[kMax][4] = {};
  int count = 0;
};
extern EnhancedPushers g_enhanced_pushers;

// The strongest dynamic lights of the last frame (world, game units; w: radius / strength), for the
// smoke and dust particles they light (Sprite3)
struct EnhancedSmokeLights {
  static constexpr int kMax = 8;
  float pos[kMax][4] = {};
  float color[kMax][4] = {};
  int count = 0;
  float strength = 0.8f;  // 0 = off
};
extern EnhancedSmokeLights g_enhanced_smoke_lights;

// Light sources of the current frame: the game's glow sprites (lamps, torches, shots,
// explosions), recorded by the sprite renderer (Sprite3_Glow.cpp), used as point lights by the
// post pass. World positions in game units, colors 0..~2.
struct EnhancedLights {
  static constexpr int kMax = 128;
  struct Light {
    float pos[3];
    float size;   // the glow's size, game units
    float color[3];
  };
  Light lights[kMax];
  int count = 0;
  void add(const float* pos, float size, float r, float g, float b) {
    if (count < kMax) {
      auto& l = lights[count++];
      l.pos[0] = pos[0];
      l.pos[1] = pos[1];
      l.pos[2] = pos[2];
      l.size = size;
      l.color[0] = r;
      l.color[1] = g;
      l.color[2] = b;
    }
  }
};
extern EnhancedLights g_enhanced_lights;

// The levels' own lamps (light-sphere): sent by the game each frame (pc-enhanced-set-lights),
// read by the renderer thread, hence the lock. Positions world, game units; w = radius.
struct EnhancedGameLights {
  static constexpr int kMax = 64;
  float pos[kMax][4];
  float color[kMax][4];
  int count = 0;
  std::mutex mutex;
};
extern EnhancedGameLights g_enhanced_game_lights;

// The in-game Enhanced menu (opened with the \ key): its open state, and the one request it hands
// to the game (polled each frame by pc-enhanced-poll-request). Kinds: 1 hour of the day,
// 2 continue point name, 3 teleport "x,y,z,yaw" (meters, degrees), 4 time running ("1") or
// frozen ("0"), 5 on the jetboard, 6 population ("0".."3").
// The time-of-day weights of the current frame (the background's itimes: 8 palettes, 4 channels,
// 0..255 where 64 = 1), captured by TFragment for the ray-traced reflections' hits.
struct EnhancedTimeOfDay {
  bool valid = false;
  s32 itimes[4][4] = {};
};
extern EnhancedTimeOfDay g_enhanced_tod;

struct EnhancedMenu {
  bool open = false;
  std::atomic<bool> minimap = true;  // the HUD's minimap (hud-map, pc-enhanced-minimap?)
  std::mutex mutex;
  int request = 0;
  std::string arg;
  void ask(int kind, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex);
    request = kind;
    arg = value;
  }
};
extern EnhancedMenu g_enhanced_menu;

// Explosions the game spawned (pc-enhanced-explosion, from explosion-init-by-other): world
// position and radius (game units), picked up by the post pass for a flash and a shockwave.
struct EnhancedExplosions {
  static constexpr int kMax = 16;
  // world position + radius, then (gun blasts) the direction it's fired + the kind (0: an
  // explosion, 1: the red gun's blast, 2: the red gun charging: sparks crackling on it)
  float pending[kMax][8];
  int count = 0;
  std::mutex mutex;
};
extern EnhancedExplosions g_enhanced_explosions;

// Is the Jak 2 plaza (Jak 3's custom level piazza) built? It's made from a Jak 2 disc by hand
// (local/blender/piazza_jak2.blend), so most installs don't have it: then no passages are cut
// into Haven and nothing tries to load it.
bool enhanced_has_piazza();
