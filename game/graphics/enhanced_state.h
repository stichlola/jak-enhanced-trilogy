#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <vector>
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
  // 0 elsewhere, 1 on foot on soft ground (sand, dirt, snow), 2 driving, 3 on foot on other
  // ground, 4 on the board, 5 driving or boarding over water, 6 on the board in the air, 7 driving in
  // the air
  int state = 0;
  int lamp_toggles = 0;  // how many times the player switched Jak's lantern (mod 64)
  bool valid = false;
};
extern EnhancedPlayer g_enhanced_player;

// The jetboard's throttle: the controller's left trigger as pressed (0..1, read straight from the
// pad: the game only gets L2 as a button), and the boost the game made of it (0..1, eased), set by
// pc-enhanced-board-boost: the speed effects
extern std::atomic<float> g_enhanced_l2_trigger;
extern std::atomic<float> g_enhanced_board_boost;

// OpenGOAL's debug toolbar (ImGui's menu bar at the top) only in a debug start (-debug): a player's
// game never shows it, whatever debug-settings.json says (set by main from the arguments)
extern bool g_enhanced_debug_toolbar;

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

// Weather and night that change the play (the menu's "Weather and night affect play"), set every
// frame by the post pass: the sand in the air (the game's sandstorm or ours, 0..1), the fog (0..1)
// and the night (0..1). Read by the game: sand slows vehicles on sand and dirt
// (pc-enhanced-storm-drag), a thick fog or sandstorm hides the minimap, more marauders roam the
// desert at night (pc-enhanced-night?).
struct EnhancedPlayWeather {
  std::atomic<bool> enabled{true};
  std::atomic<float> sand{0.f};
  std::atomic<float> fog{0.f};
  std::atomic<float> night{0.f};
};
extern EnhancedPlayWeather g_enhanced_play_weather;

// Enhanced balance (Jak 3, the menu's switch, off by default; pc-enhanced-balance?): the guns apart
// from each other and the dark and light powers made worth using in a fight (the game's code reads
// it where each value is set: goal_src/jak3, ";; Enhanced balance")
extern std::atomic<bool> g_enhanced_balance;

// Beds for the nap (game/assets/enhanced/beds.json, marked from the Enhanced menu where Jak stands on
// one): napping within 3 m of one, Jak lies down on it, seen by a camera of its own. The running
// game's, world positions in game units (pc-enhanced-bed).
struct EnhancedBeds {
  std::mutex mutex;
  std::vector<std::array<float, 3>> pos;
};
extern EnhancedBeds g_enhanced_beds;

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
  // the trilogy's start menu (the launcher asks for it, JAK_ENHANCED_START_MENU): the three games,
  // shown once as the program starts. It takes the pad like the menu
  std::atomic<bool> start_menu = false;
  bool start_menu_shown = false;
  bool any_open() const { return open || start_menu; }
  // how many times the menu opened (pc-enhanced-menu-state: the game's Select opens the nap
  // window on release, unless Select + R3 opened this menu meanwhile)
  std::atomic<int> opens = 0;
  std::atomic<bool> minimap = true;  // the HUD's minimap (hud-map, pc-enhanced-minimap?)
  std::atomic<int> fov = 100;        // the field of view in play, percent (pc-enhanced-fov)
  // the subtitles' size, percent of Enhanced's default (pc-enhanced-subtitle-size): 100 small,
  // 120 medium, 138 large (the PS2 size), 170 extra large
  std::atomic<int> subtitle_size = 100;
  // the minimap's size on screen, percent of the PS2's (pc-enhanced-minimap-size): smaller by default
  std::atomic<int> minimap_size = 70;
  // photo mode (F2, the menu's button; pc-enhanced-photo): asked for, and on in the game (held still,
  // a free camera, no HUD); entered_at: when it came on (the hint shows a few seconds)
  std::atomic<bool> photo = false;
  std::atomic<bool> photo_active = false;
  // the in-game editor (F4, EnhancedEditor): photo mode with the mouse's tools on it
  std::atomic<bool> editor = false;
  bool editing() const { return editor && photo_active; }
  // asked to leave it (F4, Start, the menu): it leaves at once when everything is saved, else asks
  std::atomic<bool> editor_leave = false;
  std::atomic<double> photo_entered_at = 0.0;
  // the Enhanced shortcuts on the keyboard, SDL keycodes (the menu's Controls, saved in
  // enhanced-settings.json): the menu (\ always works too), photo mode, the nap, the next game.
  // listening: the shortcut the next key press sets (-1 none)
  static constexpr int kShortcuts = 4;
  static constexpr u32 kDefaultKeys[kShortcuts] = {0x4000003au /* F1 */, 0x4000003cu /* F3 */,
                                                   'n', 0x40000042u /* F9 */};
  std::atomic<u32> keys[kShortcuts] = {kDefaultKeys[0], kDefaultKeys[1], kDefaultKeys[2],
                                       kDefaultKeys[3]};
  std::atomic<int> listening = -1;
  // the one-time hints already shown (bits, see pc-enhanced-hint-once), saved in
  // enhanced-settings.json; hints_dirty: shown since the last save
  std::atomic<int> hints_shown = 0;
  std::atomic<bool> hints_dirty = false;
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

// the in-game editor's camera moves, applied by the game's photo mode camera (pc-enhanced-editor-cam,
// on the game's thread): its position (4 floats) and rotation (4 rows of 4) changed in place
void enhanced_editor_camera(float* trans, float* rot);
// into the editor, or asked out of it (F4, the menu's button)
void enhanced_editor_toggle();
// the editor's ground probes (pc-enhanced-editor-probe, on the game's thread: things let fall onto
// the game's collision). Probe i: 0 past the last, 1 to probe (out: x y z where it starts and the
// length down, game units; id: its id), 2 already answered; then its answer by id: the share of the
// way down to the first hit (negative: none)
int enhanced_editor_probe(int i, float* out, int* id);
void enhanced_editor_probe_hit(int id, float u);
// the placed objects' collision (pc-enhanced-collide-fill, on the game's thread, as its collide cache
// fills): their triangles touching a box (min x y z _, max x y z _, game units) and, if seg isn't
// null, near a line (start x y z, its sphere's radius; the move x y z), written stride floats apart
// as three x y z 1 vertices, outward the way the game's are; how many, max at most
int enhanced_collide_tris(const float* box, const float* seg, float* out, int stride, int max);
// the placed objects' gameplay, once a frame (pc-enhanced-gameplay, the game's thread): in io[0..3]
// Jak's foot (game units) and w his state (+1 on the ground, +2 dead); out io[4..6] how far the
// platform he stands on moved him, io[8..10] where to put him (a teleport, back to a checkpoint).
// What happens: 0 nothing, 1 health, 2 dark eco, 3 light eco, 4 ammo, 6 put him at io[8..10]
int enhanced_gameplay(float* io);
// the editor's "Put Jak Here": where Jak goes (game units) once back in the game
void enhanced_put_jak(const float* pos);

// the terrain's pictures (custom_assets/<game>/scenery: rock_detail, ground_detail, sand_*.png):
// how much their repeats are broken up (0..1), and a count bumped when one is replaced (read again)
struct EnhancedTerrain {
  std::atomic<float> anti_tile = 0.7f;
  std::atomic<int> version = 0;
};
extern EnhancedTerrain g_enhanced_terrain;

// Jak 3's pause map (bigmap.gc): which world map is loaded (its index in WOMAP.STR) and where its
// image sits in the game's memory, set as the map draws (pc-enhanced-bigmap-image). The renderer
// works out which window of which map each 256 x 416 upload is, and draws it from the HD maps
// (bigmap_hd.py) instead
struct EnhancedBigmap {
  std::atomic<int> map{-1};
  std::atomic<u32> image{0};
};
extern EnhancedBigmap g_enhanced_bigmap;

// Explosions the game spawned (pc-enhanced-explosion, from explosion-init-by-other): world
// position and radius (game units), picked up by the post pass for a flash and a shockwave.
struct EnhancedExplosions {
  static constexpr int kMax = 16;
  // world position + radius, then (gun blasts) the direction it's fired + the kind (0: an
  // explosion, 1: the red gun's blast, 2: the red gun charging: sparks crackling on it, 3: its
  // charged shot)
  float pending[kMax][8];
  int count = 0;
  std::mutex mutex;
};
extern EnhancedExplosions g_enhanced_explosions;

// Lightning that strikes the ground in a thunderstorm: the post pass picks where (world, game
// units: around Jak, mostly far) and asks; the game (pc-enhanced-strike) finds the ground there,
// sets off an explosion that hurts what it touches and says where it hit (pc-enhanced-strike-hit),
// and the bolt is drawn down to that point
struct EnhancedStrike {
  std::mutex mutex;
  bool asked = false;
  float at[4] = {};  // where (y: Jak's height), w: the blast's radius
  bool hit = false;
  float hit_pos[3] = {};
};
extern EnhancedStrike g_enhanced_strike;

// The vehicles' headlights near the camera, sent by the game every frame (pc-enhanced-headlight):
// world position (game units), strength; the way they point (unit)
struct EnhancedHeadlights {
  static constexpr int kMax = 16;
  float pending[kMax][8];
  int count = 0;
  std::mutex mutex;
};
extern EnhancedHeadlights g_enhanced_headlights;

// Enhanced: the fires and lamps the levels place (particle spawners: torches, braziers, barrel
// fires, gas lamps) near the camera this frame, world position (game units) and kind. Many of
// them make no glow the renderer could light from (a torch is just its flame), so they become
// lights of their own.
struct EnhancedFires {
  static constexpr int kMax = 96;
  enum Kind { FIRE = 0, LAMP = 1, BLUE = 2, CANDLE = 3 };
  float pending[kMax][3];
  int kind[kMax];
  int count = 0;
  std::mutex mutex;
};
extern EnhancedFires g_enhanced_fires;

// Is the Jak 2 plaza (Jak 3's custom level piazza) built? It's made from a Jak 2 disc by hand
// (local/blender/piazza_jak2.blend), so most installs don't have it: then no passages are cut
// into Haven and nothing tries to load it.
bool enhanced_has_piazza();
