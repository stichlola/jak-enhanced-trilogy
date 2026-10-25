#pragma once

#include <mutex>
#include <string>

/*!
 * Jak 3 Enhanced: values the game hands to the renderer through the kernel (kmachine.cpp).
 */

// The key light ("sun") direction of the current time of day, world space, as the game's
// time-of-day light-group 0 dir0. Set every frame by pc-enhanced-set-sun.
struct EnhancedSun {
  float dir[3] = {0.f, 1.f, 0.f};
  bool valid = false;
};
extern EnhancedSun g_enhanced_sun;

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
// frozen ("0").
struct EnhancedMenu {
  bool open = false;
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
