#include "EnhancedEditor.h"

#include "EnhancedImport.h"
#include "EnhancedTextures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/dialogs.h"
#include "common/util/json_util.h"
#include "common/versions/versions.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/EnhancedNotes.h"
#include "game/graphics/opengl_renderer/loader/EnhancedGrass.h"
#include "game/graphics/opengl_renderer/loader/EnhancedScenery.h"
#include "game/runtime.h"

#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_internal.h"

namespace ImGui {
extern ImFont* g_ui_font;  // debug_gui.cpp: the editor's font
}
#include "third-party/json.hpp"
#include "third-party/stb_image/stb_image.h"
#include "third-party/stb_image/stb_image_write.h"

namespace {

constexpr float kMeter = 4096.f;
constexpr float kPi = 3.14159265f;

using V3 = std::array<float, 3>;
V3 operator+(const V3& a, const V3& b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
V3 operator-(const V3& a, const V3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 operator*(const V3& a, float s) {
  return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const V3& a, const V3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float length(const V3& a) {
  return std::sqrt(dot(a, a));
}
V3 normalize(const V3& a) {
  float l = length(a);
  return l > 0 ? a * (1.f / l) : a;
}
// turned by a about the vertical (+y)
V3 turn_y(const V3& v, float a) {
  float c = std::cos(a), s = std::sin(a);
  return {c * v[0] + s * v[2], v[1], -s * v[0] + c * v[2]};
}
// turned by a about the axis k (a unit vector)
V3 turn_axis(const V3& v, const V3& k, float a) {
  float c = std::cos(a), s = std::sin(a);
  return v * c + cross(k, v) * s + k * (dot(k, v) * (1.f - c));
}

// ---- turns: quaternions (x y z w), as the placed objects keep them
struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
// a after b (b's turn first)
Quat qmul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat qnorm(const Quat& q) {
  const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return l > 0 ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat();
}
// a about the axis k (a unit vector)
Quat qaxis(const V3& k, float a) {
  const float s = std::sin(a * 0.5f);
  return {k[0] * s, k[1] * s, k[2] * s, std::cos(a * 0.5f)};
}
V3 qrot(const Quat& q, const V3& v) {
  const V3 u = {q.x, q.y, q.z};
  const V3 t = cross(u, v) * 2.f;
  return v + t * q.w + cross(u, t);
}
Quat rot_of(const EnhancedPlaced& o) {
  return {o.rot[0], o.rot[1], o.rot[2], o.rot[3]};
}
void set_rot(EnhancedPlaced& o, const Quat& q) {
  const Quat n = qnorm(q);
  o.rot[0] = n.x;
  o.rot[1] = n.y;
  o.rot[2] = n.z;
  o.rot[3] = n.w;
}
// an object's point (its model's space, unscaled) in the world
V3 obj_point(const EnhancedPlaced& o, const V3& local) {
  const V3 s = {local[0] * o.scale[0], local[1] * o.scale[1], local[2] * o.scale[2]};
  return V3{o.pos[0], o.pos[1], o.pos[2]} + qrot(rot_of(o), s);
}
float max_scale(const EnhancedPlaced& o) {
  return std::max({std::abs(o.scale[0]), std::abs(o.scale[1]), std::abs(o.scale[2])});
}

// Blender's axes: its X the game's x, its Y the game's -z, its Z the game's up (y)
V3 to_blender(const V3& g) {
  return {g[0], -g[2], g[1]};
}
V3 from_blender(const V3& b) {
  return {b[0], b[2], -b[1]};
}
// a turn as Blender's XYZ Euler angles (radians), and back
V3 quat_to_euler(const Quat& g) {
  const Quat q = {g.x, -g.z, g.y, g.w};
  const float sp = std::clamp(2.f * (q.w * q.y - q.z * q.x), -1.f, 1.f);
  return {std::atan2(2.f * (q.w * q.x + q.y * q.z), 1.f - 2.f * (q.x * q.x + q.y * q.y)), std::asin(sp),
          std::atan2(2.f * (q.w * q.z + q.x * q.y), 1.f - 2.f * (q.y * q.y + q.z * q.z))};
}
Quat euler_to_quat(const V3& e) {
  const Quat q = qmul(qaxis({0, 0, 1}, e[2]), qmul(qaxis({0, 1, 0}, e[1]), qaxis({1, 0, 0}, e[0])));
  return qnorm({q.x, q.z, -q.y, q.w});
}

// ---- the camera's moves, from the mouse (render thread) to photo mode's camera (game thread)
struct CamMoves {
  bool any = false;
  V3 pivot = {0, 0, 0};  // turned about (game units)
  float pitch = 0.f;     // about axis (horizontal), first
  V3 axis = {1, 0, 0};
  float yaw = 0.f;       // then about the vertical
  V3 move = {0, 0, 0};   // then moved by (game units)
};
std::mutex g_cam_mutex;
CamMoves g_cam_moves;

// ---- the ground probes, asked here (render thread), answered on the game's thread with its own
// collision (pc-enhanced-editor-probe)
struct Probe {
  int id = 0;
  V3 from = {0, 0, 0};  // game units
  float len = 0.f;      // down
  bool answered = false;
  float u = -1.f;  // the share of the way down to the first hit, negative: none
};
std::mutex g_probe_mutex;
std::vector<Probe> g_probes;  // asked and not yet taken back (answered ones go when read)
int g_probe_next = 1;

// ---- the editor
enum Tool { kSelect, kGrass, kPlace, kLight, kMeasure, kTexture, kNote, kToolCount };
constexpr const char* kToolNames[kToolCount] = {"Select", "Grass",    "Objects", "Lights",
                                                "Measure", "Textures", "Notes"};

struct Pick {
  int kind = 0;  // 0 an object, 1 a light
  int idx = 0;
  bool operator==(const Pick& o) const { return kind == o.kind && idx == o.idx; }
};

struct State {
  std::vector<EnhancedPlaced> objects;
  std::vector<EnhancedLight> lights;
  std::vector<V3> lush, edge;  // the grass painting: x, z, the brush's radius (m)
  float fade = 12.f;
};

// the library: categories (the tabs), each of kinds (its groups). A model's kind is the tag in its
// file's name (user_<tag>_... imported by the user, game_<tag>_... from the game's own models),
// else guessed from the words in it (the kits'); at most `faces` faces when imported (scattered
// by the thousand, grass must stay light)
struct Kind {
  const char* name;
  int cat;
  std::vector<const char*> words;
  const char* tag;
  int faces;
};
constexpr const char* kCats[] = {"Scenery", "Plants", "Buildings", "Props", "Gameplay"};
constexpr int kCatCount = 5;
const std::vector<Kind> kKinds = {
    {"Grass", 0, {"grass", "tuft"}, "grass", 3000},
    {"Flowers", 0, {"flower", "leaf", "leaves"}, "flower", 3000},
    {"Rocks", 0, {"boulder", "rock", "stone"}, "rock", 8000},
    {"Pebbles", 0, {"pebble"}, "pebble", 2000},
    {"Ledges", 0, {"ledge", "slab"}, "ledge", 8000},
    {"Trees", 1, {"tree", "palm", "trunk"}, "tree", 20000},
    {"Bushes & Cacti", 1, {"bush", "cactus", "shrub", "fern"}, "bush", 10000},
    {"Crops", 1, {"cabbage", "crop", "marrow", "chili", "beetree"}, "crop", 10000},
    {"Houses", 2, {"house", "hut", "home", "building", "shack"}, "house", 40000},
    {"Walls & Gates", 2, {"wall", "gate", "door", "fence", "airlock"}, "wall", 30000},
    {"Towers & Statues", 2, {"tower", "windmill", "pillar", "statue", "column"}, "tower", 40000},
    {"Crates & Barrels", 3, {"crate", "barrel", "urn", "basket", "sack"}, "crate", 10000},
    {"Signs & Lights", 3, {"lamp", "flag", "banner", "sign", "awning", "arrow", "spotlight"}, "sign", 10000},
    {"Machines", 3, {"fan", "pump", "turbine", "machine", "generator", "turret"}, "machine", 20000},
    {"Platforms", 4, {"plat", "step", "bridge", "elevator"}, "plat", 20000},
    {"Eco & Pickups", 4, {"eco", "vent", "crystal", "ammo", "health"}, "eco", 10000},
    {"Other", 3, {}, "prop", 20000},
};

// where a model comes from: the user's (imported), the kits' (shipped), the game's own
enum Source { kSourceCustom, kSourceKit, kSourceGame, kSourceCount };
constexpr const char* kSourceNames[kSourceCount] = {"Custom", "Kit", "Game"};
int source_of(const std::string& model) {
  return model.rfind("user_", 0) == 0 ? kSourceCustom : model.rfind("game_", 0) == 0 ? kSourceGame : kSourceKit;
}

// importing a model of the user's: the file asked for, then made ready (its far version, its
// impostor) in the background: here for a .glb or .gltf, by Blender for OBJ and FBX
struct ModelImport {
  bool open = false;
  char src[512] = {};
  int kind = 0;
  bool guess_kind = true;  // several files: each one's kind from its name
  std::vector<std::string> files;  // several at once (the dialog's choice, or a folder's models)
  // the game's own models: each one's name and kind given (not from the file), in quarter meters
  std::vector<std::string> names;
  std::vector<int> kinds;
  float scale = 1.f;
  char game_dir[512] = {};  // where the decompiler put the levels' models (saved)
  int game_new = -1;        // how many of the game's listed models aren't imported yet (-1: not looked)
  bool quiet = false;       // made by itself in the background (the game's models): the tool left as it is
  bool auto_looked = false; // the game's models looked for once, when the editor first opens
  size_t next = 0;                  // the next of them to make
  int made = 0;
  std::vector<std::string> failures;
  std::future<std::vector<std::string>> picking;  // the file dialog, on its own thread: the game keeps drawing
  std::future<std::string> running;  // the result (json)
  std::string name;                  // the model being made
  std::string status;
  bool failed = false;
};

struct Thumb {
  GLuint tex = 0;
  bool tried = false;
};

struct Editor {
  bool loaded = false;
  bool was_on = false;
  Tool tool = kSelect;
  State s;
  std::vector<Pick> sel;
  std::vector<State> undo, redo;
  bool show_panel = true, show_tools = true;
  bool show_terrain = false;  // the terrain's pictures (View > Terrain Textures)
  float panel_w = 400.f;  // the side panel's width (at the UI's size 1), dragged by its left edge
  bool panel_resizing = false;

  // the view, from the post pass (a frame late)
  std::mutex mutex;
  EnhancedCamera cam;
  float proj[4] = {0, 0, 1, 1};
  bool have_cam = false;
  bool hover_ok = false, center_ok = false;
  V3 hover = {0, 0, 0}, center = {0, 0, 0};  // the world under the cursor, at the middle (game units)
  V3 hover_normal = {0, 1, 0};  // the surface's there (from the depths beside it)
  bool normal_ok = false;
  // Shift Tab: what's moved (G) goes on the surface under the cursor; and stands on it, turned to it
  bool snap_surface = false, snap_align = false;
  // the measuring tape (its tool): from a to b
  bool measuring = false, have_measure = false;
  V3 measure_a = {0, 0, 0}, measure_b = {0, 0, 0};
  // the objects brush: 0 places, 1 tints what it goes over
  int place_mode = 0;
  float tint_color[3] = {1.f, 0.75f, 0.55f};
  float tint_strength = 0.35f;
  // "Put Jak Here": where he goes once back in the game
  bool have_jak_at = false;
  V3 jak_at = {0, 0, 0};
  u64 hover_frame = 0;
  // the depth readback in flight
  GLuint pbo[2] = {0, 0};
  bool pending[2] = {false, false};
  EnhancedCamera pending_cam[2];
  float pending_proj[2][4] = {};
  float pending_uv[2][4][2] = {};  // under the cursor, the middle, beside the cursor (right, up)
  int cur = 0;
  float cursor[2] = {0.5f, 0.5f};  // 0..1 from the bottom left
  u64 frames = 0;

  // navigating: 0 no, 1 turning, 2 panning, 3 zooming
  int nav = 0;
  int nav_button = ImGuiMouseButton_Middle;
  V3 pivot = {0, 0, 0};
  // the ground under the camera (a probe each frame, from a little above it): the camera kept
  // over it
  int cam_probe = -1;
  V3 cam_probe_from = {0, 0, 0};
  bool cam_ground_ok = false;
  float cam_ground = 0.f;  // game units
  u64 cam_lift_frame = 0;

  // a modal move (G, R, S), Blender's: from where it started
  int modal = 0;
  bool modal_vertical = false;  // Z: up and down only
  State modal_start;  // what the move works from (switching G / R / S on the way: what was done)
  State modal_orig;   // before it all (cancelled: back to it; undo)
  ImVec2 modal_mouse;
  ImVec2 modal_center_px;
  V3 modal_center = {0, 0, 0};
  V3 modal_hover = {0, 0, 0};
  bool modal_have_hover = false;
  u64 modal_frame = 0;
  bool modal_dup = false;  // a duplicate's move: dropping it removes the copies
  float modal_angle = 0.f;  // R: the turn so far (carried past half a turn)

  // the brushes
  float grass_brush = 3.f, place_brush = 4.f;  // m
  int grass_mode = 0;                          // 0 tall grass, 1 its edge
  bool painting = false, paint_erase = false;
  V3 last_dab = {0, 0, 0};
  bool have_dab = false;
  bool resizing = false;  // F: the brush's size follows the mouse
  float resize_from = 0.f;
  ImVec2 resize_mouse;

  // placing
  std::vector<std::string> models;
  int model = -1;
  char search[64] = {};
  int kind_tab = 0;   // the category shown
  int sub_tab = -1;   // its kind shown (-1: all of them)
  int source_tab = -1;  // only the models from there (-1: all)
  int want_tab = -1;  // a kind's tab to bring up (an imported model)
  ModelImport imp;
  bool random_yaw = true;
  float yaw_jitter = 180.f;  // degrees either way, turned at random
  float spread = 1.f;        // how far over the brush they're strewn (1: all of it)
  // several models painted mixed (Ctrl+click in the gallery): their names, how often each comes
  // (weights), in patches of one model or all mixed
  std::vector<std::string> mix;
  std::unordered_map<std::string, float> mix_weight;
  float patch_size = 4.f;  // m
  float patchiness = 0.f;  // 0 all mixed, 1 each patch one model
  float scale_lo = 0.85f, scale_hi = 1.2f;
  int per_dab = 3;
  bool erase_any = false;
  std::unordered_map<std::string, Thumb> thumbs;
  std::unordered_map<std::string, std::array<float, 6>> bounds;  // model -> lo, hi (game units)
  std::mt19937 rng{1234};
  bool add_menu = false;

  // saving: what's on disk; Ctrl+S writes, leaving with changes asks
  State saved;
  bool dirty = false;
  bool ask_leave = false;
  // the UI: simple (big buttons, the essentials) or advanced (numbers, the scene's list, every option)
  bool advanced = false;
  bool settings_loaded = false;

  // gravity: what's placed with it on is let fall from above, things moved with it on fall from where
  // they're let go, End lets the selection fall; they land on the game's own collision, at once
  bool gravity = false;
  float drop_height = 2.f;  // m above the clicked ground
  struct Fall {
    size_t idx = 0;
    float vy = 0.f;      // m/s down
    float ground = 0.f;  // where its foot stops (game units)
    int probe = -1;
    bool have_ground = false;
    bool settle = false;  // not a fall: set on the ground at once (strewn things on a slope)
  };
  std::vector<Fall> falls;

  // the pad: the free camera's sticks move the view, a reticle in the middle points for the tools
  bool pad = false;
  bool pad_grab = false;  // L2 held: the selection follows the reticle

  // a modal move's constraint and typed value (Blender's: X / Y / Z an axis, numbers the amount)
  int axis = -1;  // -1 free, 0 x, 1 up (Blender's Z), 2 the world's z (Blender's Y)
  bool axis_local = false;  // the active object's own axis (the key pressed twice)
  bool axis_plane = false;  // Shift + the key: the other two (the plane across it)
  bool trackball = false;   // R twice: turned freely, as a ball under the mouse
  // the point things turn and scale about, as Blender's Pivot Point
  int pivot_mode = 0;
  V3 cursor3d = {0, 0, 0};  // Blender's 3D cursor (Shift + right click)
  bool have_cursor = false;
  ImVec2 right_from;  // where the right button went down (a click, not a drag: the cursor)
  bool pivot_popup = false;  // the period: the pivot menu at the mouse
  std::string typed;
  // Shift+A adds where the cursor was when it was pressed (as Blender's 3D cursor)
  V3 add_at = {0, 0, 0};
  // a box drawn with the left button on empty ground: what's in it is selected
  bool boxing = false;
  ImVec2 box_from;
  // Ctrl+C / Ctrl+V: objects around their middle
  std::vector<EnhancedPlaced> clip;
  bool show_help = false;
};
Editor g_ed;

fs::path placed_path() {
  return file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "placed.json";
}

// one object as the files keep it (positions in meters), and back
nlohmann::json placed_json(const EnhancedPlaced& p) {
  auto r = [](float v, double k) { return std::round((double)v * k) / k; };
  nlohmann::json j = {{"model", p.model},
                      {"pos", {r(p.pos[0] / kMeter, 100), r(p.pos[1] / kMeter, 100), r(p.pos[2] / kMeter, 100)}},
                      {"rot", {r(p.rot[0], 10000), r(p.rot[1], 10000), r(p.rot[2], 10000), r(p.rot[3], 10000)}},
                      {"scale", {r(p.scale[0], 1000), r(p.scale[1], 1000), r(p.scale[2], 1000)}},
                      {"tint", {r(p.tint[0], 100), r(p.tint[1], 100), r(p.tint[2], 100)}}};
  if (p.collide >= 0) {
    j["collide"] = p.collide;
  }
  if (p.motion || p.action) {
    j["play"] = {{"motion", p.motion},
                 {"move", {r(p.move[0] / kMeter, 100), r(p.move[1] / kMeter, 100), r(p.move[2] / kMeter, 100)}},
                 {"period", r(p.period, 100)},
                 {"action", p.action},
                 {"link", p.link}};
  }
  if (p.hidden) {
    j["hide"] = true;
  }
  if (p.locked) {
    j["lock"] = true;
  }
  if (!p.group.empty()) {
    j["group"] = p.group;
  }
  return j;
}

EnhancedPlaced placed_from_json(const nlohmann::json& o) {
  EnhancedPlaced p;
  p.model = o.at("model").get<std::string>();
  for (int k = 0; k < 3; k++) {
    p.pos[k] = o.at("pos")[k].get<float>() * kMeter;
    if (o.contains("tint")) {
      p.tint[k] = o["tint"][k].get<float>();
    }
  }
  // (a turn, else the older files' yaw about the vertical; a scale per axis, else one for all)
  if (o.contains("rot")) {
    set_rot(p, Quat{o["rot"][0].get<float>(), o["rot"][1].get<float>(), o["rot"][2].get<float>(),
                    o["rot"][3].get<float>()});
  } else {
    set_rot(p, qaxis({0, 1, 0}, o.value("yaw", 0.f) * kPi / 180.f));
  }
  if (o.contains("scale") && o["scale"].is_array()) {
    for (int k = 0; k < 3; k++) {
      p.scale[k] = o["scale"][k].get<float>();
    }
  } else {
    p.scale[0] = p.scale[1] = p.scale[2] = o.value("scale", 1.f);
  }
  p.collide = o.value("collide", -1);
  if (o.contains("play") && o["play"].is_object()) {
    const auto& g = o["play"];
    p.motion = std::clamp(g.value("motion", 0), 0, (int)kMotionCount - 1);
    if (g.contains("move") && g["move"].is_array()) {
      for (int k = 0; k < 3; k++) {
        p.move[k] = g["move"][k].get<float>() * kMeter;
      }
    }
    p.period = g.value("period", 4.f);
    p.action = std::clamp(g.value("action", 0), 0, (int)kActionCount - 1);
    p.link = g.value("link", 0);
  }
  p.hidden = o.value("hide", false);
  p.locked = o.value("lock", false);
  p.group = o.value("group", std::string());
  return p;
}

void load_objects(Editor& e) {
  e.s.objects.clear();
  if (!fs::exists(placed_path())) {
    return;
  }
  try {
    auto j = parse_commented_json(file_util::read_text_file(placed_path()), "placed.json");
    const std::string game = game_version_names[g_game_version];
    if (!j.contains(game)) {
      return;
    }
    for (const auto& o : j.at(game)) {
      e.s.objects.push_back(placed_from_json(o));
    }
    lg::info("[enhanced] editor: {} placed objects", e.s.objects.size());
  } catch (const std::exception& ex) {
    lg::error("[enhanced] placed.json: {}", ex.what());
  }
}

void save_objects(const Editor& e) {
  nlohmann::json j = nlohmann::json::object();
  if (fs::exists(placed_path())) {
    try {
      j = parse_commented_json(file_util::read_text_file(placed_path()), "placed.json");
    } catch (const std::exception&) {
      j = nlohmann::json::object();
    }
  }
  auto arr = nlohmann::json::array();
  for (const auto& p : e.s.objects) {
    arr.push_back(placed_json(p));
  }
  j[game_version_names[g_game_version]] = arr;
  try {
    file_util::write_text_file(placed_path(), j.dump(1));
  } catch (const std::exception& ex) {
    lg::error("[enhanced] placed.json: {}", ex.what());
  }
}

// what the renderer shows: the objects (those being moved hidden: the depth under the cursor is
// the ground's, not theirs)
void push_objects(const Editor& e) {
  // (hidden, H: not drawn nor solid while editing)
  auto enhanced_set_placed = [](std::vector<EnhancedPlaced> list) {
    std::erase_if(list, [](const EnhancedPlaced& o) { return o.hidden; });
    ::enhanced_set_placed(list);
  };
  // (what's falling or turned and scaled by hand isn't solid meanwhile: the probes would find the
  // thing itself)
  auto ghosts = [&](std::vector<EnhancedPlaced>& list, bool sel_too) {
    for (const auto& f : e.falls) {
      if (f.idx < list.size()) {
        list[f.idx].ghost = true;
      }
    }
    if (sel_too) {
      for (const auto& p : e.sel) {
        if (p.kind == 0 && p.idx < (int)list.size()) {
          list[p.idx].ghost = true;
        }
      }
    }
  };
  if (e.modal != 'G' || e.sel.empty()) {
    if (e.falls.empty() && !e.modal) {
      enhanced_set_placed(e.s.objects);
      return;
    }
    auto list = e.s.objects;
    ghosts(list, e.modal != 0);
    enhanced_set_placed(list);
    return;
  }
  std::vector<EnhancedPlaced> shown = e.s.objects;
  ghosts(shown, false);
  std::vector<EnhancedPlaced> kept;
  for (size_t i = 0; i < shown.size(); i++) {
    if (std::find(e.sel.begin(), e.sel.end(), Pick{0, (int)i}) == e.sel.end()) {
      kept.push_back(shown[i]);
    }
  }
  enhanced_set_placed(kept);
}


void push_grass(const Editor& e) {
  std::vector<std::array<float, 3>> lush(e.s.lush.begin(), e.s.lush.end());
  std::vector<std::array<float, 3>> edge(e.s.edge.begin(), e.s.edge.end());
  enhanced_grass_paint(lush, edge, e.s.fade, false);
}

// before a change: the state kept for undo
void remember(Editor& e) {
  e.undo.push_back(e.s);
  if (e.undo.size() > 200) {
    e.undo.erase(e.undo.begin());
  }
  e.redo.clear();
}

// after a change: shown (what changed), kept on disk at the next Save
void commit(Editor& e, bool objects, bool lights, bool grass) {
  if (objects) {
    push_objects(e);
  }
  if (lights) {
    enhanced_set_lights(e.s.lights, false);
  }
  if (grass) {
    push_grass(e);
  }
  e.dirty = true;
}

void restore(Editor& e, const State& st) {
  bool grass = st.lush != e.s.lush || st.edge != e.s.edge || st.fade != e.s.fade;
  e.s = st;
  e.sel.clear();
  e.falls.clear();
  commit(e, true, true, grass);
}

// Save: everything on disk
void save_all(Editor& e) {
  save_objects(e);
  enhanced_set_lights(e.s.lights, true);
  std::vector<std::array<float, 3>> lush(e.s.lush.begin(), e.s.lush.end());
  std::vector<std::array<float, 3>> edge(e.s.edge.begin(), e.s.edge.end());
  enhanced_grass_paint(lush, edge, e.s.fade, true);
  e.saved = e.s;
  e.dirty = false;
  lg::info("[enhanced] editor: saved ({} objects, {} lights)", e.s.objects.size(), e.s.lights.size());
}

// Don't save: back to what's on disk
void discard_all(Editor& e) {
  const bool grass = e.saved.lush != e.s.lush || e.saved.edge != e.s.edge || e.saved.fade != e.s.fade;
  e.s = e.saved;
  e.sel.clear();
  e.falls.clear();
  e.undo.clear();
  e.redo.clear();
  push_objects(e);
  enhanced_set_lights(e.s.lights, false);
  if (grass) {
    push_grass(e);
  }
  e.dirty = false;
}

void undo(Editor& e) {
  if (!e.undo.empty()) {
    e.redo.push_back(e.s);
    State st = e.undo.back();
    e.undo.pop_back();
    restore(e, st);
  }
}

void redo(Editor& e) {
  if (!e.redo.empty()) {
    e.undo.push_back(e.s);
    State st = e.redo.back();
    e.redo.pop_back();
    restore(e, st);
  }
}

// the editor's own settings (the UI's mode, gravity), kept with the game's settings
fs::path settings_path() {
  return file_util::get_user_settings_dir(g_game_version) / "enhanced-editor.json";
}

void load_settings(Editor& e) {
  try {
    if (fs::exists(settings_path())) {
      auto j = parse_commented_json(file_util::read_text_file(settings_path()), "enhanced-editor.json");
      e.advanced = j.value("advanced", e.advanced);
      e.gravity = j.value("gravity", e.gravity);
      e.panel_w = j.value("panel_width", e.panel_w);
      e.show_panel = j.value("panel_open", e.show_panel);
      e.snap_surface = j.value("snap_surface", e.snap_surface);
      e.snap_align = j.value("snap_align", e.snap_align);
      g_enhanced_terrain.anti_tile = j.value("anti_tile", g_enhanced_terrain.anti_tile.load());
      std::snprintf(e.imp.game_dir, sizeof(e.imp.game_dir), "%s",
                    j.value("game_models_dir", std::string()).c_str());
    }
  } catch (const std::exception& ex) {
    lg::error("[enhanced] enhanced-editor.json: {}", ex.what());
  }
}

void save_settings(const Editor& e) {
  try {
    file_util::write_text_file(settings_path(),
                               nlohmann::json{{"advanced", e.advanced},
                                               {"gravity", e.gravity},
                                               {"panel_width", e.panel_w},
                                               {"panel_open", e.show_panel},
                                               {"snap_surface", e.snap_surface},
                                               {"snap_align", e.snap_align},
                                               {"anti_tile", g_enhanced_terrain.anti_tile.load()},
                                               {"game_models_dir", std::string(e.imp.game_dir)}}
                                    .dump(1));
  } catch (const std::exception& ex) {
    lg::error("[enhanced] enhanced-editor.json: {}", ex.what());
  }
}

// a model's bounds around its foot (game units, unscaled), once it has loaded
const std::array<float, 6>* model_bounds(Editor& e, const std::string& model) {
  auto it = e.bounds.find(model);
  if (it != e.bounds.end()) {
    return &it->second;
  }
  std::array<float, 6> b;
  if (!enhanced_model_bounds(model, b.data(), b.data() + 3)) {
    return nullptr;
  }
  return &e.bounds.emplace(model, b).first->second;
}

// the UI's size: the editor's windows scaled with the screen (made for 1000 pixels high)
float ui_k() {
  return std::clamp(ImGui::GetIO().DisplaySize.y / 1000.f, 1.f, 2.5f);
}

// ---- the look: Blender's default dark theme
const ImVec4 kBlue = ImVec4(0.278f, 0.447f, 0.702f, 1.f);  // #4772b3, Blender's active and selected
const ImVec4 kBlueHi = ImVec4(0.337f, 0.514f, 0.784f, 1.f);
const ImVec4 kAlert = ImVec4(0.6f, 0.2f, 0.2f, 1.f);
const ImVec4 kKeyText = ImVec4(0.75f, 0.75f, 0.75f, 1.f);  // a hint's words next to its key

ImVec4 grey(float v, float a = 1.f) {
  return ImVec4(v, v, v, a);
}

void blender_style(ImGuiStyle& s, float k) {
  s.WindowPadding = ImVec2(10.f * k, 10.f * k);
  s.FramePadding = ImVec2(8.f * k, 4.f * k);
  s.ItemSpacing = ImVec2(8.f * k, 6.f * k);
  s.ItemInnerSpacing = ImVec2(6.f * k, 4.f * k);
  s.CellPadding = ImVec2(6.f * k, 3.f * k);
  s.IndentSpacing = 18.f * k;
  s.ScrollbarSize = 12.f * k;
  s.GrabMinSize = 10.f * k;
  s.WindowMinSize = ImVec2(32.f * k, 32.f * k);
  s.WindowRounding = 0.f;  // (the regions are square, as Blender's)
  s.ChildRounding = 4.f * k;
  s.FrameRounding = 4.f * k;
  s.PopupRounding = 6.f * k;
  s.ScrollbarRounding = 6.f * k;
  s.GrabRounding = 3.f * k;
  s.TabRounding = 4.f * k;
  s.WindowBorderSize = 1.f;
  s.ChildBorderSize = 1.f;
  s.PopupBorderSize = 1.f;
  s.FrameBorderSize = 0.f;
  s.WindowTitleAlign = ImVec2(0.f, 0.5f);
  s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
  s.SelectableTextAlign = ImVec2(0.f, 0.5f);

  auto* c = s.Colors;
  c[ImGuiCol_Text] = grey(0.9f);
  c[ImGuiCol_TextDisabled] = grey(0.6f);
  c[ImGuiCol_WindowBg] = grey(0.188f);  // #303030
  c[ImGuiCol_ChildBg] = grey(0.157f);   // #282828
  c[ImGuiCol_PopupBg] = grey(0.11f, 0.98f);
  c[ImGuiCol_Border] = grey(0.1f);
  c[ImGuiCol_BorderShadow] = grey(0.f, 0.f);
  c[ImGuiCol_FrameBg] = grey(0.33f);  // #545454, Blender's fields
  c[ImGuiCol_FrameBgHovered] = grey(0.4f);
  c[ImGuiCol_FrameBgActive] = grey(0.45f);
  c[ImGuiCol_TitleBg] = grey(0.137f);
  c[ImGuiCol_TitleBgActive] = grey(0.137f);
  c[ImGuiCol_TitleBgCollapsed] = grey(0.137f);
  c[ImGuiCol_MenuBarBg] = grey(0.137f);  // the top bar, #232323
  c[ImGuiCol_ScrollbarBg] = grey(0.f, 0.f);
  c[ImGuiCol_ScrollbarGrab] = grey(0.33f);
  c[ImGuiCol_ScrollbarGrabHovered] = grey(0.4f);
  c[ImGuiCol_ScrollbarGrabActive] = grey(0.48f);
  c[ImGuiCol_CheckMark] = grey(1.f);
  c[ImGuiCol_SliderGrab] = kBlue;
  c[ImGuiCol_SliderGrabActive] = kBlueHi;
  c[ImGuiCol_Button] = grey(0.33f);
  c[ImGuiCol_ButtonHovered] = grey(0.4f);
  c[ImGuiCol_ButtonActive] = kBlue;
  c[ImGuiCol_Header] = grey(0.239f);  // a panel's header, #3d3d3d
  c[ImGuiCol_HeaderHovered] = grey(0.29f);
  c[ImGuiCol_HeaderActive] = grey(0.33f);
  c[ImGuiCol_Separator] = grey(0.27f);
  c[ImGuiCol_SeparatorHovered] = kBlue;
  c[ImGuiCol_SeparatorActive] = kBlueHi;
  c[ImGuiCol_ResizeGrip] = grey(0.f, 0.f);
  c[ImGuiCol_ResizeGripHovered] = grey(0.f, 0.f);
  c[ImGuiCol_ResizeGripActive] = grey(0.f, 0.f);
  c[ImGuiCol_Tab] = grey(0.157f);
  c[ImGuiCol_TabHovered] = grey(0.29f);
  c[ImGuiCol_TabSelected] = kBlue;
  c[ImGuiCol_TabSelectedOverline] = kBlue;
  c[ImGuiCol_TableHeaderBg] = grey(0.239f);
  c[ImGuiCol_TableBorderStrong] = grey(0.1f);
  c[ImGuiCol_TableBorderLight] = grey(0.157f);
  c[ImGuiCol_TableRowBg] = grey(0.f, 0.f);
  c[ImGuiCol_TableRowBgAlt] = grey(1.f, 0.03f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(kBlue.x, kBlue.y, kBlue.z, 0.6f);
  c[ImGuiCol_NavCursor] = kBlueHi;
  c[ImGuiCol_ModalWindowDimBg] = grey(0.f, 0.45f);
}

// a button that's on or off (Blender's toggles and segmented choices: blue when on)
bool toggle_button(const char* label, bool on, ImVec2 size = ImVec2(0, 0)) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueHi);
  }
  const bool r = ImGui::Button(label, size);
  if (on) {
    ImGui::PopStyleColor(2);
  }
  return r;
}

// a property row: the name on the left in a narrow column (no empty room before it), the field
// filling the rest
void prop(const char* name, float col_width = 0.f) {
  const float x0 = ImGui::GetCursorPosX();
  const float col =
      col_width > 0.f ? col_width : std::min(ImGui::GetContentRegionAvail().x * 0.38f, 110.f * ui_k());
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(name);
  ImGui::SameLine(x0 + col);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

// a key as a keycap, then what it does (the status bar, the help)
void key_hint(const char* key, const char* what) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float k = ui_k();
  const ImVec2 pad(6.f * k, 1.f * k);
  const ImVec2 ts = ImGui::CalcTextSize(key);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(p, ImVec2(p.x + ts.x + 2 * pad.x, p.y + ts.y + 2 * pad.y),
                    ImGui::GetColorU32(grey(0.33f)), 4.f * k);
  dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(grey(1.f)), key);
  ImGui::Dummy(ImVec2(ts.x + 2 * pad.x, ts.y + 2 * pad.y));
  ImGui::SameLine(0.f, 6.f * k);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad.y);
  ImGui::TextColored(kKeyText, "%s", what);
}

// ---- the view
// a world point (game units) to the screen (pixels): false behind the camera
bool to_screen(const Editor& e, const V3& p, ImVec2* out, float* depth = nullptr) {
  const auto& cam = e.cam;
  V3 d = {p[0] - cam.trans[0], p[1] - cam.trans[1], p[2] - cam.trans[2]};
  V3 v;
  for (int c = 0; c < 3; c++) {
    v[c] = d[0] * cam.rot[0][c] + d[1] * cam.rot[1][c] + d[2] * cam.rot[2][c];
  }
  if (v[2] < 0.3f * kMeter) {
    return false;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  float nx = v[0] * e.proj[2] / v[2], ny = v[1] * e.proj[3] / v[2];
  *out = ImVec2((nx * 0.5f + 0.5f) * size.x, (1.f - (ny * 0.5f + 0.5f)) * size.y);
  if (depth) {
    *depth = v[2];
  }
  return true;
}

// the view's axes in the world: right and up as on screen, forward
V3 view_axis(const Editor& e, int c) {
  V3 a = {e.cam.rot[0][c], e.cam.rot[1][c], e.cam.rot[2][c]};
  if (c < 2 && e.proj[2 + c] < 0) {
    a = a * -1.f;
  }
  return a;
}

// the ray from the camera through a pixel (a unit direction)
V3 pixel_ray(const Editor& e, ImVec2 px) {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  float nx = px.x / std::max(size.x, 1.f) * 2.f - 1.f;
  float ny = (1.f - px.y / std::max(size.y, 1.f)) * 2.f - 1.f;
  V3 v = {nx / e.proj[2], ny / e.proj[3], 1.f};
  V3 w;
  for (int r = 0; r < 3; r++) {
    w[r] = e.cam.rot[r][0] * v[0] + e.cam.rot[r][1] * v[1] + e.cam.rot[r][2] * v[2];
  }
  return normalize(w);
}

// where the ray through a pixel meets the level y (false: it doesn't, ahead)
bool pixel_on_level(const Editor& e, ImVec2 px, float y, V3* out) {
  V3 d = pixel_ray(e, px);
  V3 o = {e.cam.trans[0], e.cam.trans[1], e.cam.trans[2]};
  if (std::abs(d[1]) < 1e-4f) {
    return false;
  }
  float t = (y - o[1]) / d[1];
  if (t <= 0 || t > 3000.f * kMeter) {
    return false;
  }
  *out = o + d * t;
  return true;
}

// world units per pixel at a depth (game units)
float per_pixel(const Editor& e, float depth) {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  return 2.f * depth / std::max(std::abs(e.proj[2]) * size.x, 1.f);
}

V3 cam_pos(const Editor& e) {
  return {e.cam.trans[0], e.cam.trans[1], e.cam.trans[2]};
}

// where the tools point: the mouse (the pen), or with the pad the reticle in the middle
ImVec2 pointer(const Editor& e) {
  const auto& io = ImGui::GetIO();
  return e.pad ? ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f) : io.MousePos;
}

// ---- gravity: things let fall land on the game's collision (its gravity, no easing)
constexpr float kGravity = 60.f;   // m/s/s (*standard-dynamics*)
constexpr float kFallMax = 40.f;   // m/s
constexpr float kProbeUp = 0.5f;   // m: the probe starts above the foot
constexpr float kProbeLen = 600.f;  // m

constexpr float kCamProbeUp = 4.f;     // m: the camera's probe starts this far above it
constexpr float kCamProbeLen = 300.f;  // m
constexpr float kCamClear = 0.6f;      // m: the camera kept this far over the ground

constexpr float kSettleUp = 3.f;    // m: settling, the probe starts this far above (the ground may be higher)

int ask_probe(const V3& from, float len) {
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  Probe p;
  p.id = g_probe_next++;
  p.from = from;
  p.len = len;
  g_probes.push_back(p);
  return p.id;
}

// its answer once there (taken back: the request goes)
bool probe_answer(int id, float* u) {
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  auto it = std::find_if(g_probes.begin(), g_probes.end(), [&](const Probe& p) { return p.id == id; });
  if (it == g_probes.end() || !it->answered) {
    return false;
  }
  *u = it->u;
  g_probes.erase(it);
  return true;
}

void forget_probe(int id) {
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  std::erase_if(g_probes, [&](const Probe& p) { return p.id == id; });
}

void drop(Editor& e, size_t idx) {
  if (idx >= e.s.objects.size()) {
    return;
  }
  std::erase_if(e.falls, [&](const Editor::Fall& f) {
    if (f.idx == idx) {
      forget_probe(f.probe);
    }
    return f.idx == idx;
  });
  Editor::Fall f;
  f.idx = idx;
  e.falls.push_back(f);
}

// set on the ground under it at once (up or down), found the same way
void settle(Editor& e, size_t idx) {
  drop(e, idx);
  e.falls.back().settle = true;
}

// these objects' falls forgotten (they're moved by hand, or gone)
void stop_falls(Editor& e, const std::vector<Pick>& picks) {
  std::erase_if(e.falls, [&](const Editor::Fall& f) {
    bool hit = std::find(picks.begin(), picks.end(), Pick{0, (int)f.idx}) != picks.end();
    if (hit) {
      forget_probe(f.probe);
    }
    return hit;
  });
}

// objects removed: the falls and the selection kept on the ones left (their indices move down)
void remove_objects(Editor& e, std::vector<int> gone) {
  std::sort(gone.begin(), gone.end());
  gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
  if (gone.empty()) {
    return;
  }
  std::vector<int> to(e.s.objects.size());
  for (size_t i = 0, g = 0, n = 0; i < to.size(); i++) {
    if (g < gone.size() && gone[g] == (int)i) {
      to[i] = -1;
      g++;
    } else {
      to[i] = (int)n++;
    }
  }
  for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
    e.s.objects.erase(e.s.objects.begin() + *it);
  }
  std::erase_if(e.falls, [&](Editor::Fall& f) {
    if (f.idx >= to.size() || to[f.idx] < 0) {
      forget_probe(f.probe);
      return true;
    }
    f.idx = to[f.idx];
    return false;
  });
  std::erase_if(e.sel, [&](Pick& p) {
    if (p.kind != 0) {
      return false;
    }
    if (p.idx >= (int)to.size() || to[p.idx] < 0) {
      return true;
    }
    p.idx = to[p.idx];
    return false;
  });
}

void drop_selected(Editor& e) {
  bool any = false;
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      if (!any) {
        remember(e);
        any = true;
      }
      drop(e, p.idx);
    }
  }
}

// a foot's height over its model's lowest point (the model's origin may not be at its bottom)
float foot_offset(Editor& e, const EnhancedPlaced& o);

void update_falls(Editor& e) {
  if (e.falls.empty() || e.modal) {
    return;
  }
  const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.1f);
  bool moved = false;
  for (size_t i = 0; i < e.falls.size();) {
    auto& f = e.falls[i];
    if (f.idx >= e.s.objects.size()) {
      e.falls.erase(e.falls.begin() + i);
      continue;
    }
    auto& o = e.s.objects[f.idx];
    if (!f.have_ground) {
      const V3 from = {o.pos[0], o.pos[1] + foot_offset(e, o) + (f.settle ? kSettleUp : kProbeUp) * kMeter,
                       o.pos[2]};
      if (f.probe < 0) {
        f.probe = ask_probe(from, kProbeLen * kMeter);
        i++;
        continue;
      }
      float u;
      if (!probe_answer(f.probe, &u)) {
        i++;
        continue;
      }
      if (u < 0.f) {
        // nothing under it: it stays where it is
        e.falls.erase(e.falls.begin() + i);
        continue;
      }
      // (the probe is a small sphere: its center stops its radius above the ground)
      f.ground = from[1] - u * kProbeLen * kMeter - 0.1f * kMeter - foot_offset(e, o);
      f.have_ground = true;
      if (f.settle) {
        o.pos[1] = f.ground;
        moved = true;
        e.falls.erase(e.falls.begin() + i);
        continue;
      }
    }
    f.vy = std::min(f.vy + kGravity * dt, kFallMax);
    o.pos[1] -= f.vy * dt * kMeter;
    moved = true;
    if (o.pos[1] <= f.ground) {
      o.pos[1] = f.ground;
      e.falls.erase(e.falls.begin() + i);
      e.dirty = true;
      continue;
    }
    i++;
  }
  if (moved) {
    push_objects(e);
  }
}

void add_move(const V3& move) {
  std::lock_guard<std::mutex> lock(g_cam_mutex);
  g_cam_moves.move = g_cam_moves.move + move;
  g_cam_moves.any = true;
}

// the camera kept over the ground: the game's collision under it, asked each frame from a little
// above it (a slope it was turned or moved into, it's lifted out of)
void keep_above_ground(Editor& e) {
  if (!e.have_cam) {
    return;
  }
  const V3 cam = cam_pos(e);
  if (e.cam_probe < 0) {
    e.cam_probe_from = cam + V3{0.f, kCamProbeUp * kMeter, 0.f};
    e.cam_probe = ask_probe(e.cam_probe_from, kCamProbeLen * kMeter);
    return;
  }
  float u;
  if (!probe_answer(e.cam_probe, &u)) {
    return;
  }
  e.cam_probe = -1;
  // (the probe is a small sphere: its center stops its radius above the ground)
  e.cam_ground_ok = u >= 0.f;
  e.cam_ground = e.cam_probe_from[1] - u * kCamProbeLen * kMeter - 0.1f * kMeter;
  // (moved since: where it was asked is no longer under it)
  const float dx = cam[0] - e.cam_probe_from[0], dz = cam[2] - e.cam_probe_from[2];
  if (!e.cam_ground_ok || dx * dx + dz * dz > kMeter * kMeter) {
    return;
  }
  // (once its last lift has reached the camera: lifts aren't added up)
  if (cam[1] < e.cam_ground + 0.5f * kCamClear * kMeter && e.frames > e.cam_lift_frame + 2) {
    add_move(V3{0.f, e.cam_ground + kCamClear * kMeter - cam[1], 0.f});
    e.cam_lift_frame = e.frames;
  }
}

// the middle button's drag: turning about the pivot (the scene follows the mouse, as Blender's
// turntable), panning, zooming
void navigate(Editor& e, ImVec2 delta) {
  const V3 cam = cam_pos(e);
  const V3 right = view_axis(e, 0), up = view_axis(e, 1), fwd = view_axis(e, 2);
  if (e.nav == 1) {
    const float k = 0.0065f;  // radians a pixel
    V3 rel = cam - e.pivot;
    // dragging right turns the scene right: the camera goes left around it
    float yaw = delta.x * k;
    if (dot(turn_y(rel, 0.01f) - rel, right) > 0) {
      yaw = -yaw;
    }
    // dragging down turns the scene's top toward the camera: it rises over it
    V3 axis = normalize(V3{fwd[2], 0.f, -fwd[0]});
    float pitch = delta.y * k;
    if (turn_axis(rel, axis, 0.01f)[1] < rel[1]) {
      pitch = -pitch;
    }
    // (never over the top or under)
    V3 f2 = turn_axis(fwd, axis, pitch);
    if (std::abs(f2[1]) > 0.985f) {
      pitch = 0;
    }
    // (and not down through the ground: from above what it turns about, it stays above it; nor
    // under the ground below the camera)
    const V3 rel2 = turn_axis(rel, axis, pitch);
    if (rel[1] > 0.f && rel2[1] < 0.5f * kMeter) {
      pitch = 0;
    }
    if (e.cam_ground_ok && rel2[1] < rel[1] && e.pivot[1] + rel2[1] < e.cam_ground + kCamClear * kMeter) {
      pitch = 0;
    }
    std::lock_guard<std::mutex> lock(g_cam_mutex);
    auto& m = g_cam_moves;
    m.pivot = e.pivot;
    m.axis = axis;
    m.pitch += pitch;
    m.yaw += yaw;
    m.any = true;
  } else if (e.nav == 2) {
    float depth = std::max(dot(e.pivot - cam, fwd), 1.f * kMeter);
    float pp = per_pixel(e, depth);
    add_move(right * (-delta.x * pp) + up * (delta.y * pp));
  } else if (e.nav == 3) {
    float dist = std::max(length(e.pivot - cam), 1.f * kMeter);
    add_move(fwd * (-delta.y * dist * 0.006f));
  }
}

void zoom_wheel(Editor& e, float wheel) {
  const V3 cam = cam_pos(e);
  const V3 fwd = view_axis(e, 2);
  float dist = e.center_ok ? length(e.center - cam) : 20.f * kMeter;
  dist = std::clamp(dist, 1.f * kMeter, 2000.f * kMeter);
  // (never past what's in the middle)
  float step = std::min(dist * 0.15f * std::abs(wheel), std::max(dist - 0.5f * kMeter, 0.f));
  add_move(fwd * (wheel > 0 ? step : -dist * 0.18f * std::abs(wheel)));
}

// the lowest (top) of an object's turned box over its foot (game units)
float box_y(Editor& e, const EnhancedPlaced& o, bool top) {
  const auto* b = model_bounds(e, o.model);
  if (!b) {
    return top ? 1.f * kMeter : 0.f;
  }
  float best = top ? -1e30f : 1e30f;
  for (int i = 0; i < 8; i++) {
    const V3 l = {(i & 1) ? (*b)[3] : (*b)[0], (i & 2) ? (*b)[4] : (*b)[1], (i & 4) ? (*b)[5] : (*b)[2]};
    const float y = obj_point(o, l)[1] - o.pos[1];
    best = top ? std::max(best, y) : std::min(best, y);
  }
  return best;
}

float foot_offset(Editor& e, const EnhancedPlaced& o) {
  return box_y(e, o, false);
}

// ---- picking: the object or light nearest the cursor, nearest the camera among those it's over
float object_radius(Editor& e, const EnhancedPlaced& o, V3* center) {
  const auto* b = model_bounds(e, o.model);
  float h = b ? (*b)[4] - (*b)[1] : 1.f * kMeter;
  float w = b ? std::max((*b)[3] - (*b)[0], (*b)[5] - (*b)[2]) : 1.f * kMeter;
  *center = obj_point(o, V3{b ? ((*b)[0] + (*b)[3]) * 0.5f : 0.f, b ? ((*b)[1] + (*b)[4]) * 0.5f : h * 0.5f,
                            b ? ((*b)[2] + (*b)[5]) * 0.5f : 0.f});
  return 0.5f * std::sqrt(h * h + w * w) * max_scale(o);
}

bool pick_at(Editor& e, ImVec2 mouse, Pick* out, bool lights_only = false) {
  float best = 1e30f;
  bool found = false;
  if (!lights_only) {
    for (size_t i = 0; i < e.s.objects.size(); i++) {
      if (e.s.objects[i].hidden || e.s.objects[i].locked) {
        continue;
      }
      V3 c;
      float r = object_radius(e, e.s.objects[i], &c);
      ImVec2 sp;
      float depth;
      if (!to_screen(e, c, &sp, &depth)) {
        continue;
      }
      float rpx = r / per_pixel(e, depth);
      float d = std::hypot(sp.x - mouse.x, sp.y - mouse.y);
      if (d < std::max(rpx * 0.8f, 14.f) && depth < best) {
        best = depth;
        *out = {0, (int)i};
        found = true;
      }
    }
  }
  for (size_t i = 0; i < e.s.lights.size(); i++) {
    const auto& l = e.s.lights[i];
    ImVec2 sp;
    float depth;
    if (!to_screen(e, V3{l.pos[0], l.pos[1], l.pos[2]}, &sp, &depth)) {
      continue;
    }
    if (std::hypot(sp.x - mouse.x, sp.y - mouse.y) < 14.f && depth < best) {
      best = depth;
      *out = {1, (int)i};
      found = true;
    }
  }
  return found;
}

V3 pick_pos(const Editor& e, const Pick& p) {
  if (p.kind == 0) {
    const auto& o = e.s.objects[p.idx];
    return {o.pos[0], o.pos[1], o.pos[2]};
  }
  const auto& l = e.s.lights[p.idx];
  return {l.pos[0], l.pos[1], l.pos[2]};
}

V3 sel_center(const Editor& e) {
  V3 c = {0, 0, 0};
  for (const auto& p : e.sel) {
    c = c + pick_pos(e, p);
  }
  return e.sel.empty() ? c : c * (1.f / e.sel.size());
}

// the pivot points (Blender's)
enum PivotMode { kPivotMedian, kPivotIndividual, kPivotCursor, kPivotActive, kPivotBounds, kPivotCount };
constexpr const char* kPivotNames[kPivotCount] = {"Median Point", "Individual Origins", "3D Cursor",
                                                  "Active Element", "Bounding Box Center"};

// what a move turns and scales about
V3 pivot_point(const Editor& e) {
  switch (e.pivot_mode) {
    case kPivotCursor:
      return e.have_cursor ? e.cursor3d : sel_center(e);
    case kPivotActive:
      return e.sel.empty() ? V3{0, 0, 0} : pick_pos(e, e.sel.back());
    case kPivotBounds: {
      V3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
      for (const auto& p : e.sel) {
        const V3 q = pick_pos(e, p);
        for (int k = 0; k < 3; k++) {
          lo[k] = std::min(lo[k], q[k]);
          hi[k] = std::max(hi[k], q[k]);
        }
      }
      return e.sel.empty() ? V3{0, 0, 0} : (lo + hi) * 0.5f;
    }
    default:
      return sel_center(e);
  }
}

// the selected objects' collision: Auto (the model's own way), On, Off
void collision_combo(Editor& e) {
  int first = -2;
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      first = e.s.objects[p.idx].collide;
      break;
    }
  }
  if (first == -2) {
    return;
  }
  static const char* kNames[3] = {"Auto", "Off", "On"};
  int cur = first + 1;
  prop("Collision");
  if (ImGui::Combo("##Collision", &cur, kNames, 3)) {
    remember(e);
    for (const auto& p : e.sel) {
      if (p.kind == 0) {
        e.s.objects[p.idx].collide = cur - 1;
      }
    }
    commit(e, true, false, false);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Whether Jak stands on it and bumps into it.\nAuto: big solid models yes (rocks, "
                      "houses), small or see-through ones no (grass, pebbles).");
  }
}

// the selected objects' gameplay: how they move (in the game, still while editing) and what Jak
// touching them does
void gameplay_section(Editor& e) {
  std::vector<EnhancedPlaced*> objs;
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      objs.push_back(&e.s.objects[p.idx]);
    }
  }
  if (objs.empty()) {
    return;
  }
  ImGui::SeparatorText("Gameplay");
  auto& o = *objs[0];
  bool changed = false, done = false;
  auto track = [&](bool edit) {
    if (ImGui::IsItemActivated()) {
      remember(e);
    }
    done |= ImGui::IsItemDeactivatedAfterEdit();
    changed |= edit;
  };
  static const char* kMotions[kMotionCount] = {"Still", "Back and Forth", "Turning", "Bobbing"};
  static const char* kActions[kActionCount] = {"None",     "Health",     "Dark Eco",  "Light Eco",
                                               "Ammo",     "Eco Vent",   "Checkpoint", "Teleport"};
  int motion = o.motion, action = o.action;
  prop("Motion");
  if (ImGui::Combo("##Motion", &motion, kMotions, kMotionCount)) {
    remember(e);
    for (auto* p : objs) {
      p->motion = motion;
      // (a platform that moves: up 3 m by default)
      if (motion != kMotionNone && motion != kMotionSpin && !p->move[0] && !p->move[1] && !p->move[2]) {
        p->move[1] = 3.f * kMeter;
      }
    }
    changed = done = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("How it moves while playing (still while editing).\nJak standing on it goes with it.");
  }
  if (o.motion != kMotionNone) {
    if (o.motion != kMotionSpin) {
      // (Blender's axes, meters)
      const V3 bm = to_blender(V3{o.move[0], o.move[1], o.move[2]} * (1.f / kMeter));
      float mv[3] = {bm[0], bm[1], bm[2]};
      prop("Move");
      const bool ed = ImGui::DragFloat3("##Move", mv, 0.05f, -200.f, 200.f, "%.2f m");
      track(ed);
      if (ed) {
        const V3 g = from_blender(V3{mv[0], mv[1], mv[2]}) * kMeter;
        for (auto* p : objs) {
          for (int k = 0; k < 3; k++) {
            p->move[k] = g[k];
          }
        }
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(o.motion == kMotionMove ? "How far it goes (and comes back)" : "How far it bobs each way");
      }
    }
    float period = o.period;
    prop("Period");
    const bool ed = ImGui::DragFloat("##Period", &period, 0.05f, 0.2f, 120.f, "%.1f s");
    track(ed);
    if (ed) {
      for (auto* p : objs) {
        p->period = std::max(period, 0.2f);
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Seconds for a whole cycle (a turn, there and back)");
    }
  }
  prop("Action");
  if (ImGui::Combo("##Action", &action, kActions, kActionCount)) {
    remember(e);
    for (auto* p : objs) {
      p->action = action;
    }
    changed = done = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("What Jak touching it does.\nHealth, eco, ammo: taken, back after 30 s. Eco Vent: dark eco "
                      "while he's near.\nCheckpoint: back here after dying. Teleport: to the other teleport with the "
                      "same Link.");
  }
  if (o.action == kActionTeleport) {
    int link = o.link;
    prop("Link");
    const bool ed = ImGui::InputInt("##Link", &link);
    if (ed) {
      remember(e);
      for (auto* p : objs) {
        p->link = link;
      }
      changed = done = true;
    }
    int others = 0;
    for (const auto& p : e.s.objects) {
      others += &p != &o && p.action == kActionTeleport && p.link == o.link;
    }
    if (others == 0) {
      ImGui::TextDisabled("No other teleport with Link %d yet", o.link);
    }
  }
  if (changed || done) {
    commit(e, done, false, false);
  }
}

// Blender's Alt+R / Alt+S: the selected objects' turn, or their scale, back to none
void clear_transform(Editor& e, bool turn) {
  bool any = false;
  for (const auto& p : e.sel) {
    any |= p.kind == 0;
  }
  if (!any) {
    return;
  }
  remember(e);
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      auto& o = e.s.objects[p.idx];
      if (turn) {
        set_rot(o, Quat());
      } else {
        o.scale[0] = o.scale[1] = o.scale[2] = 1.f;
      }
    }
  }
  commit(e, true, false, false);
}

bool icon_toggle(const char* id, bool eye, bool on);

// H (1: the selected hidden), Shift H (2: the others), Alt H (0: all shown again, and selected)
void hide_objects(Editor& e, int mode) {
  remember(e);
  if (mode == 0) {
    for (size_t i = 0; i < e.s.objects.size(); i++) {
      if (e.s.objects[i].hidden) {
        e.s.objects[i].hidden = false;
        e.sel.push_back({0, (int)i});
      }
    }
  } else {
    for (size_t i = 0; i < e.s.objects.size(); i++) {
      const bool sel = std::find(e.sel.begin(), e.sel.end(), Pick{0, (int)i}) != e.sel.end();
      if (sel == (mode == 1)) {
        e.s.objects[i].hidden = true;
      }
    }
    std::erase_if(e.sel, [&](const Pick& p) { return p.kind == 0 && e.s.objects[p.idx].hidden; });
  }
  commit(e, true, false, false);
}

// Ctrl G: the selected objects in a new group (the outliner shows it, its eye and lock for them all)
void group_selected(Editor& e) {
  bool any = false;
  for (const auto& p : e.sel) {
    any |= p.kind == 0;
  }
  if (!any) {
    return;
  }
  std::string name;
  for (int n = 1;; n++) {
    name = fmt::format("Group {}", n);
    if (std::none_of(e.s.objects.begin(), e.s.objects.end(), [&](const auto& o) { return o.group == name; })) {
      break;
    }
  }
  remember(e);
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      e.s.objects[p.idx].group = name;
    }
  }
  commit(e, false, false, false);
}

// J: Jak there (where the cursor points, else the middle of the view) when back in the game
void put_jak_here(Editor& e) {
  if (!e.hover_ok && !e.center_ok) {
    return;
  }
  e.jak_at = e.hover_ok ? e.hover : e.center;
  e.have_jak_at = true;
  const float at[3] = {e.jak_at[0], e.jak_at[1] + 0.3f * kMeter, e.jak_at[2]};
  enhanced_put_jak(at);
}

void delete_selected(Editor& e) {
  if (e.sel.empty()) {
    return;
  }
  remember(e);
  std::vector<int> objs, lights;
  for (const auto& p : e.sel) {
    (p.kind == 0 ? objs : lights).push_back(p.idx);
  }
  std::sort(lights.rbegin(), lights.rend());
  remove_objects(e, objs);
  for (int i : lights) {
    e.s.lights.erase(e.s.lights.begin() + i);
  }
  e.sel.clear();
  commit(e, !objs.empty(), !lights.empty(), false);
}

void start_modal(Editor& e, int kind) {
  if (e.sel.empty()) {
    return;
  }
  stop_falls(e, e.sel);
  e.modal = kind;
  e.modal_vertical = false;
  e.axis = -1;
  e.axis_local = e.axis_plane = e.trackball = false;
  e.typed.clear();
  e.modal_start = e.s;
  e.modal_orig = e.s;
  e.modal_mouse = pointer(e);
  e.modal_center = pivot_point(e);
  e.modal_angle = 0.f;
  e.modal_have_hover = false;
  e.modal_frame = e.frames;
  if (!to_screen(e, e.modal_center, &e.modal_center_px)) {
    e.modal_center_px = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
  }
  push_objects(e);
}

void duplicate_selected(Editor& e) {
  if (e.sel.empty()) {
    return;
  }
  remember(e);
  std::vector<Pick> copies;
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      e.s.objects.push_back(e.s.objects[p.idx]);
      copies.push_back({0, (int)e.s.objects.size() - 1});
    } else {
      e.s.lights.push_back(e.s.lights[p.idx]);
      copies.push_back({1, (int)e.s.lights.size() - 1});
    }
  }
  e.sel = copies;
  start_modal(e, 'G');
  e.modal_dup = true;
}

// the modal move this frame, from the mouse
// the typed amount (Blender's numeric input): false while there's none
bool typed_value(const Editor& e, float* v) {
  if (e.typed.empty() || e.typed == "-" || e.typed == ".") {
    return false;
  }
  *v = std::strtof(e.typed.c_str(), nullptr);
  return true;
}

// the axis a move is held to (a unit vector): the world's, or the active object's own
V3 constraint_axis(const Editor& e) {
  V3 a = {0, 0, 0};
  a[std::max(e.axis, 0)] = 1.f;
  if (e.axis_local && !e.sel.empty() && e.sel.back().kind == 0) {
    a = normalize(qrot(rot_of(e.modal_start.objects[e.sel.back().idx]), a));
  }
  return a;
}

// where the ray through a pixel meets the plane through c across n (false: along it, or behind)
bool pixel_on_plane(const Editor& e, ImVec2 px, const V3& c, const V3& n, V3* out) {
  const V3 d = pixel_ray(e, px), o = cam_pos(e);
  const float den = dot(d, n);
  if (std::abs(den) < 1e-4f) {
    return false;
  }
  const float t = dot(c - o, n) / den;
  if (t <= 0 || t > 3000.f * kMeter) {
    return false;
  }
  *out = o + d * t;
  return true;
}

// a turn's sign on screen: which way a turn by +a about k (through c) goes round c, as seen
// (+1: as the mouse's angle grows, screen y down)
float screen_turn_sign(const Editor& e, const V3& c, const V3& k) {
  V3 side = cross(k, view_axis(e, 2));
  if (length(side) < 1e-3f) {
    side = view_axis(e, 0);
  }
  side = normalize(side) * kMeter;
  ImVec2 c0, a0, a1;
  if (!to_screen(e, c, &c0) || !to_screen(e, c + side, &a0) || !to_screen(e, c + qrot(qaxis(k, 0.05f), side), &a1)) {
    return 1.f;
  }
  const float d = std::atan2(a1.y - c0.y, a1.x - c0.x) - std::atan2(a0.y - c0.y, a0.x - c0.x);
  return std::remainder(d, 2.f * kPi) >= 0.f ? 1.f : -1.f;
}

// the modal move this frame, from the mouse (Blender's G, R and S)
void update_modal(Editor& e) {
  const ImVec2 mouse = pointer(e);
  const State& st = e.modal_start;
  const bool snap = ImGui::GetIO().KeyCtrl;
  float typed;
  const bool has_typed = typed_value(e, &typed);
  const V3 c = e.modal_center;
  const V3 ax = constraint_axis(e);
  const bool along = e.axis >= 0 && !e.axis_plane, across = e.axis >= 0 && e.axis_plane;
  const bool individual = e.pivot_mode == kPivotIndividual;
  e.modal_vertical = along && e.axis == 1 && !e.axis_local;
  const V3 right = view_axis(e, 0), up = view_axis(e, 1), fwd = view_axis(e, 2);
  const float depth = std::max(dot(c - cam_pos(e), fwd), 1.f * kMeter);
  const float pp = per_pixel(e, depth);
  const ImVec2 dm(mouse.x - e.modal_mouse.x, mouse.y - e.modal_mouse.y);
  if (e.modal == 'G') {
    // in the view's plane through the pivot; along the axis; or across it (its plane)
    V3 delta = right * (dm.x * pp) + up * (-dm.y * pp);
    if (has_typed) {
      delta = (e.axis >= 0 ? ax : V3{1, 0, 0}) * (typed * kMeter);
    } else if (along) {
      ImVec2 s0, s1;
      float t = dot(delta, ax);
      if (to_screen(e, c, &s0) && to_screen(e, c + ax * kMeter, &s1)) {
        const ImVec2 s(s1.x - s0.x, s1.y - s0.y);
        const float len2 = s.x * s.x + s.y * s.y;
        // (the mouse's way along the axis as it's drawn; seen end on, the view's plane)
        if (len2 > 4.f) {
          t = (dm.x * s.x + dm.y * s.y) / len2 * kMeter;
        }
      }
      if (snap) {
        t = std::round(t / (0.5f * kMeter)) * 0.5f * kMeter;
      }
      delta = ax * t;
    } else if (across) {
      V3 h0, h1;
      if (pixel_on_plane(e, e.modal_mouse, c, ax, &h0) && pixel_on_plane(e, mouse, c, ax, &h1)) {
        delta = h1 - h0;
      } else {
        delta = delta - ax * dot(delta, ax);
      }
    }
    if (snap && !has_typed && !along) {
      // (Ctrl: by half meters)
      for (auto& d : delta) {
        d = std::round(d / (0.5f * kMeter)) * 0.5f * kMeter;
      }
    }
    // (Shift Tab: the first one's foot on the surface under the cursor, the others kept round it;
    // aligned: each stood up along the surface's normal)
    const bool on_surface = e.snap_surface && !has_typed && e.axis < 0 && e.hover_ok && !e.sel.empty();
    if (on_surface) {
      const auto& p0 = e.sel[0];
      const float* a = p0.kind == 0 ? st.objects[p0.idx].pos : st.lights[p0.idx].pos;
      delta = e.hover - V3{a[0], a[1], a[2]};
    }
    for (const auto& p : e.sel) {
      float* pos = p.kind == 0 ? e.s.objects[p.idx].pos : e.s.lights[p.idx].pos;
      const float* pos0 = p.kind == 0 ? st.objects[p.idx].pos : st.lights[p.idx].pos;
      for (int k = 0; k < 3; k++) {
        pos[k] = pos0[k] + delta[k];
      }
      if (p.kind == 0) {
        auto& o = e.s.objects[p.idx];
        if (on_surface && e.snap_align && e.normal_ok) {
          const V3 up = {0, 1, 0}, n = e.hover_normal;
          const V3 axis = cross(up, n);
          const float s = length(axis), c = std::clamp(dot(up, n), -1.f, 1.f);
          set_rot(o, s > 1e-4f ? qmul(qaxis(axis * (1.f / s), std::atan2(s, c)), rot_of(st.objects[p.idx]))
                               : rot_of(st.objects[p.idx]));
        } else {
          set_rot(o, rot_of(st.objects[p.idx]));
        }
      }
    }
  } else if (e.modal == 'R') {
    Quat dq;
    if (e.trackball && !has_typed) {
      // R twice: as a ball under the mouse (across: about the view's up, up and down: its right)
      const float k = 0.01f;
      const V3 front = c - fwd * kMeter;
      ImVec2 f0, f1;
      float sx = 1.f, sy = 1.f;
      if (to_screen(e, front, &f0) && to_screen(e, c + qrot(qaxis(up, 0.05f), front - c), &f1)) {
        sx = f1.x > f0.x ? 1.f : -1.f;
      }
      if (to_screen(e, front, &f0) && to_screen(e, c + qrot(qaxis(right, 0.05f), front - c), &f1)) {
        sy = f1.y > f0.y ? 1.f : -1.f;
      }
      float ax_a = dm.x * k * sx, ay_a = dm.y * k * sy;
      if (snap) {
        ax_a = std::round(ax_a / (kPi / 36.f)) * (kPi / 36.f);
        ay_a = std::round(ay_a / (kPi / 36.f)) * (kPi / 36.f);
      }
      dq = qmul(qaxis(up, ax_a), qaxis(right, ay_a));
    } else {
      // about the view's axis (toward the camera), or the axis it's held to: the mouse's angle
      // round the pivot on screen
      const V3 k = e.axis >= 0 ? ax : fwd * -1.f;
      const float a0 = std::atan2(e.modal_mouse.y - e.modal_center_px.y, e.modal_mouse.x - e.modal_center_px.x);
      const float a1 = std::atan2(mouse.y - e.modal_center_px.y, mouse.x - e.modal_center_px.x);
      float a = std::remainder(a1 - a0, 2.f * kPi) * screen_turn_sign(e, c, k);
      if (has_typed) {
        a = typed * kPi / 180.f;
      } else if (snap) {
        // (Ctrl: by 5 degrees, as Blender's)
        a = std::round(a / (kPi / 36.f)) * (kPi / 36.f);
      }
      // (the turn carried round: past half a turn it goes on, not back)
      e.modal_angle = has_typed ? a : e.modal_angle + std::remainder(a - e.modal_angle, 2.f * kPi);
      dq = qaxis(k, has_typed ? a : e.modal_angle);
    }
    for (const auto& p : e.sel) {
      if (p.kind == 0) {
        const auto& o0 = st.objects[p.idx];
        auto& o = e.s.objects[p.idx];
        set_rot(o, qmul(dq, rot_of(o0)));
        if (!individual) {
          const V3 rel = qrot(dq, V3{o0.pos[0], o0.pos[1], o0.pos[2]} - c);
          for (int k = 0; k < 3; k++) {
            o.pos[k] = c[k] + rel[k];
          }
        }
      } else if (!individual) {
        const auto& l0 = st.lights[p.idx];
        const V3 rel = qrot(dq, V3{l0.pos[0], l0.pos[1], l0.pos[2]} - c);
        for (int k = 0; k < 3; k++) {
          e.s.lights[p.idx].pos[k] = c[k] + rel[k];
        }
      }
    }
  } else if (e.modal == 'S') {
    // the mouse's distance from the pivot on screen, against where it started
    const float d0 = std::hypot(e.modal_mouse.x - e.modal_center_px.x, e.modal_mouse.y - e.modal_center_px.y);
    const float d1 = std::hypot(mouse.x - e.modal_center_px.x, mouse.y - e.modal_center_px.y);
    float f = std::clamp(d1 / std::max(d0, 4.f), 0.f, 50.f);
    if (has_typed) {
      f = typed;
    } else if (snap) {
      f = std::round(f * 10.f) / 10.f;
    }
    // (a vector scaled: the whole of it; along the axis; or across it)
    auto scaled = [&](const V3& v) {
      if (along) {
        return v + ax * (dot(v, ax) * (f - 1.f));
      }
      if (across) {
        const V3 on = ax * dot(v, ax);
        return on + (v - on) * f;
      }
      return v * f;
    };
    for (const auto& p : e.sel) {
      if (p.kind == 0) {
        const auto& o0 = st.objects[p.idx];
        auto& o = e.s.objects[p.idx];
        // (each of its own axes by how much the scale stretches it: exact when they line up)
        const Quat q = rot_of(o0);
        for (int k = 0; k < 3; k++) {
          V3 a = {0, 0, 0};
          a[k] = 1.f;
          const float g = e.axis >= 0 ? length(scaled(qrot(q, a))) : f;
          o.scale[k] = std::clamp(o0.scale[k] * g, 0.01f, 100.f);
        }
        if (!individual) {
          const V3 rel = scaled(V3{o0.pos[0], o0.pos[1], o0.pos[2]} - c);
          for (int k = 0; k < 3; k++) {
            o.pos[k] = c[k] + rel[k];
          }
        }
      } else {
        const auto& l0 = st.lights[p.idx];
        auto& l = e.s.lights[p.idx];
        if (e.axis < 0) {
          l.radius = std::clamp(l0.radius * f, 0.3f * kMeter, 60.f * kMeter);
        }
        if (!individual) {
          const V3 rel = scaled(V3{l0.pos[0], l0.pos[1], l0.pos[2]} - c);
          for (int k = 0; k < 3; k++) {
            l.pos[k] = c[k] + rel[k];
          }
        }
      }
    }
  }
  push_objects(e);
  enhanced_set_lights(e.s.lights, false);
}

// another of G / R / S on the way (Blender's): what was done kept, the new one from here
void switch_modal(Editor& e, int kind) {
  e.modal_start = e.s;
  e.modal = kind;
  e.axis = -1;
  e.axis_local = e.axis_plane = e.trackball = false;
  e.modal_angle = 0.f;
  e.typed.clear();
  e.modal_mouse = pointer(e);
  e.modal_center = pivot_point(e);
  if (!to_screen(e, e.modal_center, &e.modal_center_px)) {
    e.modal_center_px = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
  }
}

void end_modal(Editor& e, bool keep) {
  const int kind = e.modal;
  e.modal = 0;
  if (keep) {
    if (!e.modal_dup) {
      // (the state before it, for undo)
      e.undo.push_back(e.modal_orig);
      e.redo.clear();
    }
    commit(e, true, true, false);
    if (e.gravity && kind == 'G' && !e.modal_vertical) {
      for (const auto& p : e.sel) {
        if (p.kind == 0) {
          drop(e, p.idx);
        }
      }
    }
  } else {
    e.s = e.modal_orig;
    if (e.modal_dup) {
      // dropping a duplicate's move: no duplicate (as Blender's, minus the copy left in place)
      e.s = e.undo.back();
      e.undo.pop_back();
      e.sel.clear();
    }
    push_objects(e);
    enhanced_set_lights(e.s.lights, false);
  }
  e.modal_dup = false;
  e.typed.clear();
  e.axis = -1;
  e.axis_local = e.axis_plane = e.trackball = false;
}

// ---- brushes
void grass_dab(Editor& e, const V3& at) {
  auto& list = e.grass_mode == 0 ? e.s.lush : e.s.edge;
  const float r = e.grass_brush;
  const V3 m = {at[0] / kMeter, 0.f, at[2] / kMeter};
  if (e.paint_erase) {
    std::erase_if(list, [&](const V3& p) { return std::hypot(p[0] - m[0], p[1] - m[2]) < r + p[2] * 0.5f; });
  } else {
    list.push_back({m[0], m[2], r});
  }
}

// the models the brush paints: the chosen one, or the mix
std::vector<std::string> brush_models(const Editor& e) {
  if (e.mix.size() > 1) {
    return e.mix;
  }
  if (e.model >= 0 && e.model < (int)e.models.size()) {
    return {e.models[e.model]};
  }
  return {};
}

float mix_weight(const Editor& e, const std::string& m) {
  auto it = e.mix_weight.find(m);
  return it == e.mix_weight.end() ? 1.f : it->second;
}

// which of the mix goes at a place: by its weights, at random or (patchiness) the same one over a
// patch
std::string pick_model(Editor& e, const V3& at) {
  const auto set = brush_models(e);
  if (set.size() <= 1) {
    return set.empty() ? "" : set[0];
  }
  std::uniform_real_distribution<float> u(0.f, 1.f);
  float r = u(e.rng);
  if (u(e.rng) < e.patchiness) {
    // (the patch's own number: a hash of the cell it's in)
    const int cx = (int)std::floor(at[0] / (e.patch_size * kMeter)), cz = (int)std::floor(at[2] / (e.patch_size * kMeter));
    u32 h = (u32)cx * 73856093u ^ (u32)cz * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    r = (h & 0xffffff) / float(0x1000000);
  }
  float total = 0.f;
  for (const auto& m : set) {
    total += mix_weight(e, m);
  }
  if (total <= 0.f) {
    return set[0];
  }
  r *= total;
  for (const auto& m : set) {
    r -= mix_weight(e, m);
    if (r < 0.f) {
      return m;
    }
  }
  return set.back();
}

void place_one(Editor& e, const V3& at) {
  const std::string model = pick_model(e, at);
  if (model.empty()) {
    return;
  }
  std::uniform_real_distribution<float> u(0.f, 1.f);
  EnhancedPlaced p;
  p.model = model;
  for (int k = 0; k < 3; k++) {
    p.pos[k] = at[k];
  }
  set_rot(p, qaxis({0, 1, 0}, e.random_yaw ? (u(e.rng) * 2.f - 1.f) * e.yaw_jitter * kPi / 180.f : 0.f));
  p.scale[0] = p.scale[1] = p.scale[2] = e.scale_lo + (e.scale_hi - e.scale_lo) * u(e.rng);
  // (the game's gameplay models doing what they did there)
  const auto has = [&](const char* w) { return model.find(w) != std::string::npos; };
  if (has("eco_vent")) {
    p.action = kActionVent;
  } else if (has("gun_ammo")) {
    p.action = kActionAmmo;
  } else if (has("eco_crystal_light")) {
    p.action = kActionLightEco;
  } else if (has("eco_crystal") || has("eco_canister")) {
    p.action = kActionDarkEco;
  } else if (has("warp_gate")) {
    p.action = kActionTeleport;
  }
  if (e.gravity) {
    p.pos[1] += e.drop_height * kMeter;
  }
  e.s.objects.push_back(p);
  if (e.gravity) {
    drop(e, e.s.objects.size() - 1);
  }
}

void place_dab(Editor& e, const V3& at, bool first) {
  if (e.place_mode == 1) {
    // (the tint brush: the color laid over what it goes over, more in the middle; Ctrl: back to none)
    const float r = e.place_brush * kMeter;
    for (auto& o : e.s.objects) {
      if (o.hidden || o.locked) {
        continue;
      }
      const float d = std::hypot(o.pos[0] - at[0], o.pos[2] - at[2]);
      if (d > r) {
        continue;
      }
      const float w = e.tint_strength * (1.f - 0.6f * d / r);
      for (int k = 0; k < 3; k++) {
        const float to = e.paint_erase ? 1.f : e.tint_color[k];
        o.tint[k] += (to - o.tint[k]) * w;
      }
    }
    return;
  }
  if (e.paint_erase) {
    const float r = e.place_brush * kMeter;
    const auto only = brush_models(e);
    std::vector<int> gone;
    for (size_t i = 0; i < e.s.objects.size(); i++) {
      const auto& o = e.s.objects[i];
      // (no model chosen: any; else the brush's ones)
      if (std::hypot(o.pos[0] - at[0], o.pos[2] - at[2]) < r &&
          (e.erase_any || only.empty() || std::find(only.begin(), only.end(), o.model) != only.end())) {
        gone.push_back((int)i);
      }
    }
    remove_objects(e, gone);
    return;
  }
  if (first) {
    place_one(e, at);
    return;
  }
  // strewn within the brush
  std::uniform_real_distribution<float> u(0.f, 1.f);
  for (int i = 0; i < e.per_dab; i++) {
    float a = u(e.rng) * 2.f * kPi, d = std::sqrt(u(e.rng)) * e.spread * e.place_brush * kMeter;
    const size_t n = e.s.objects.size();
    place_one(e, at + V3{std::cos(a) * d, 0.f, std::sin(a) * d});
    // (each on the ground under it, not at the cursor's height)
    if (!e.gravity && e.s.objects.size() > n) {
      settle(e, e.s.objects.size() - 1);
    }
  }
}

// a circle on the ground, as seen (its points projected)
void draw_ground_circle(const Editor& e, ImDrawList* dl, const V3& c, float r, ImU32 col, float thick) {
  ImVec2 pts[48];
  int n = 0;
  for (int i = 0; i < 48; i++) {
    float a = i / 48.f * 2.f * kPi;
    if (to_screen(e, c + V3{std::cos(a) * r, 0.f, std::sin(a) * r}, &pts[n])) {
      n++;
    }
  }
  if (n > 2) {
    dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, thick);
  }
}

// the box around an object, as seen
void draw_box(Editor& e, ImDrawList* dl, const EnhancedPlaced& o, ImU32 col) {
  const auto* b = model_bounds(e, o.model);
  if (!b) {
    return;
  }
  V3 corners[8];
  for (int i = 0; i < 8; i++) {
    V3 l = {(i & 1) ? (*b)[3] : (*b)[0], (i & 2) ? (*b)[4] : (*b)[1], (i & 4) ? (*b)[5] : (*b)[2]};
    corners[i] = obj_point(o, l);
  }
  static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                   {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& ed : edges) {
    ImVec2 a, bb;
    if (to_screen(e, corners[ed[0]], &a) && to_screen(e, corners[ed[1]], &bb)) {
      dl->AddLine(a, bb, col, 1.5f);
    }
  }
}

// a model's picture: its impostor's view from three quarters above (none: no picture)
GLuint thumb(Editor& e, const std::string& model, ImVec2* uv0, ImVec2* uv1, int* budget) {
  auto& t = e.thumbs[model];
  if (!t.tried && *budget > 0) {
    (*budget)--;
    t.tried = true;
    auto path = file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] /
                "scenery" / (fs::path(model).stem().string() + "_imp.png");
    int w = 0, h = 0;
    u8* px = fs::exists(path) ? stbi_load(path.string().c_str(), &w, &h, nullptr, 4) : nullptr;
    if (px) {
      glGenTextures(1, &t.tex);
      glBindTexture(GL_TEXTURE_2D, t.tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glBindTexture(GL_TEXTURE_2D, 0);
      stbi_image_free(px);
    }
  }
  // (8 x 8 views: row 6, column 1)
  *uv0 = ImVec2(1.f / 8.f, 6.f / 8.f);
  *uv1 = ImVec2(2.f / 8.f, 7.f / 8.f);
  return t.tex;
}

int kind_of(const std::string& model) {
  // (the tag its name carries)
  if (source_of(model) != kSourceKit) {
    const size_t a = model.find('_'), b = model.find('_', a + 1);
    if (b != std::string::npos) {
      const std::string tag = model.substr(a + 1, b - a - 1);
      for (size_t k = 0; k < kKinds.size(); k++) {
        if (tag == kKinds[k].tag) {
          return (int)k;
        }
      }
    }
  }
  std::string low = model;
  std::transform(low.begin(), low.end(), low.begin(), ::tolower);
  for (size_t k = 0; k + 1 < kKinds.size(); k++) {
    for (const char* w : kKinds[k].words) {
      if (low.find(w) != std::string::npos) {
        return (int)k;
      }
    }
  }
  return (int)kKinds.size() - 1;
}

bool matches(const std::string& model, const char* search) {
  if (!search[0]) {
    return true;
  }
  std::string a = model, b = search;
  std::transform(a.begin(), a.end(), a.begin(), ::tolower);
  std::transform(b.begin(), b.end(), b.begin(), ::tolower);
  return a.find(b) != std::string::npos;
}

std::string pretty(const std::string& model) {
  std::string n = fs::path(model).stem().string();
  if (n.rfind("desert_", 0) == 0) {
    n = n.substr(7);
  } else if (source_of(n) != kSourceKit) {
    // (user_<tag>_ / game_<tag>_: the name after them)
    const size_t b = n.find('_', n.find('_') + 1);
    if (b != std::string::npos) {
      n = n.substr(b + 1);
    }
  }
  std::replace(n.begin(), n.end(), '_', ' ');
  return n;
}

// is a model in the browser now (its category, kind and source; searching: every one that matches)
bool in_browser(const Editor& e, const std::string& m) {
  if (!matches(m, e.search)) {
    return false;
  }
  if (e.source_tab >= 0 && source_of(m) != e.source_tab) {
    return false;
  }
  if (e.search[0]) {
    return true;
  }
  const int k = kind_of(m);
  return kKinds[k].cat == e.kind_tab && (e.sub_tab < 0 || e.sub_tab == k);
}

// the models' browser: a search, categories as tabs, their kinds and where they come from, pictures
// to click (the user's own first)
void model_browser(Editor& e, float width) {
  ImGui::SetNextItemWidth(width);
  ImGui::InputTextWithHint("##search", "Search all models...", e.search, sizeof(e.search));
  if (e.want_tab >= 0) {
    e.sub_tab = e.want_tab;
    e.source_tab = -1;
  }
  if (ImGui::BeginTabBar("##cats", ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int c = 0; c < kCatCount; c++) {
      const bool want = e.want_tab >= 0 && kKinds[e.want_tab].cat == c;
      if (ImGui::BeginTabItem(kCats[c], nullptr, want ? ImGuiTabItemFlags_SetSelected : 0)) {
        if (e.kind_tab != c) {
          e.kind_tab = c;
          e.sub_tab = want ? e.want_tab : -1;
        }
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
    e.want_tab = -1;
  }
  // (its kinds, then where they come from: small buttons that wrap)
  auto chip = [&](const char* label, bool on) {
    const float w = ImGui::CalcTextSize(label).x + 2 * ImGui::GetStyle().FramePadding.x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w < right) {
      ImGui::SameLine();
    }
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
    }
    const bool click = ImGui::SmallButton(label);
    if (on) {
      ImGui::PopStyleColor();
    }
    return click;
  };
  if (!e.search[0]) {
    if (ImGui::SmallButton(e.sub_tab < 0 ? "[All]" : "All")) {
      e.sub_tab = -1;
    }
    for (size_t k = 0; k < kKinds.size(); k++) {
      if (kKinds[k].cat == e.kind_tab && chip(kKinds[k].name, e.sub_tab == (int)k)) {
        e.sub_tab = e.sub_tab == (int)k ? -1 : (int)k;
      }
    }
  }
  ImGui::TextDisabled("From:");
  ImGui::SameLine();
  if (ImGui::SmallButton(e.source_tab < 0 ? "[Any]" : "Any")) {
    e.source_tab = -1;
  }
  for (int s = 0; s < kSourceCount; s++) {
    if (chip(kSourceNames[s], e.source_tab == s)) {
      e.source_tab = e.source_tab == s ? -1 : s;
    }
  }
  if (e.imp.quiet && e.imp.running.valid()) {
    ImGui::TextDisabled("Getting the game's models ready: %d of %d", (int)e.imp.next, (int)e.imp.files.size());
  }
  int budget = 4;  // pictures read a frame
  // (as many a row as fit about 100 pixels each, each picture filling its share of the panel: an
  // image button adds its padding)
  const ImGuiStyle& st = ImGui::GetStyle();
  const int per_row = std::clamp((int)((width - st.ScrollbarSize) / (100.f * ui_k())), 2, 8);
  const float cell = (width - st.ScrollbarSize - (per_row - 1) * st.ItemSpacing.x) / per_row - 2 * st.FramePadding.x;
  int n = 0;
  // (the user's first, then the kits', then the game's)
  std::vector<size_t> order;
  for (int s = 0; s < kSourceCount; s++) {
    for (size_t i = 0; i < e.models.size(); i++) {
      if (source_of(e.models[i]) == s && in_browser(e, e.models[i])) {
        order.push_back(i);
      }
    }
  }
  const int shown = (int)order.size();
  const float row_h = cell + 2 * st.FramePadding.y + ImGui::GetTextLineHeightWithSpacing() + st.ItemSpacing.y;
  // (at most three rows and a bit, the rest scrolled; fewer when there are fewer)
  const int rows = (shown + per_row - 1) / per_row;
  const float grid_h = rows > 3 ? 3.4f * row_h : std::max(rows, 1) * row_h + st.ItemSpacing.y;
  ImGui::TextDisabled(e.mix.size() > 1 ? "%d models mixed  (Ctrl/Shift+click: add or remove)"
                                         : "Click: choose  -  Ctrl/Shift+click: choose several",
                      (int)e.mix.size());
  ImGui::BeginChild("##models", ImVec2(width, grid_h), ImGuiChildFlags_FrameStyle);
  int last_source = -1;
  for (size_t i : order) {
    const auto& m = e.models[i];
    // (each source's own rows, under its name)
    if (source_of(m) != last_source) {
      last_source = source_of(m);
      if (n % per_row) {
        n = 0;
      }
      ImGui::TextDisabled("%s", kSourceNames[last_source]);
    }
    if (n % per_row) {
      ImGui::SameLine();
    }
    n++;
    ImGui::PushID((int)i);
    ImGui::BeginGroup();
    ImVec2 uv0, uv1;
    GLuint tex = thumb(e, m, &uv0, &uv1, &budget);
    const bool in_mix = e.mix.size() > 1 && std::find(e.mix.begin(), e.mix.end(), m) != e.mix.end();
    const bool chosen = e.model == (int)i || in_mix;
    if (chosen) {
      ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
    }
    bool click = tex ? ImGui::ImageButton("##b", (ImTextureID)(intptr_t)tex, ImVec2(cell, cell), uv0, uv1)
                     : ImGui::Button("?", ImVec2(cell + 2 * st.FramePadding.x, cell + 2 * st.FramePadding.y));
    if (chosen) {
      ImGui::PopStyleColor();
    }
    if (click && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)) {
      // (Ctrl or Shift: one more in the mix, or one out of it)
      if (e.mix.empty() && e.model >= 0 && e.model < (int)e.models.size()) {
        e.mix.push_back(e.models[e.model]);
      }
      const auto it = std::find(e.mix.begin(), e.mix.end(), m);
      if (it == e.mix.end()) {
        e.mix.push_back(m);
        e.model = (int)i;
      } else if (e.mix.size() > 1) {
        e.mix.erase(it);
        const auto first = std::find(e.models.begin(), e.models.end(), e.mix[0]);
        e.model = first == e.models.end() ? -1 : (int)(first - e.models.begin());
      }
      e.tool = kPlace;
    } else if (click) {
      e.model = (int)i;
      e.mix.clear();
      if (e.tool != kPlace) {
        e.tool = kPlace;
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\n%s, %s\nCtrl+click or Shift+click: add it to the mix (again: take it out)",
                        pretty(m).c_str(), kSourceNames[source_of(m)], kKinds[kind_of(m)].name);
    }
    // (the name cut to the picture's width)
    std::string label = pretty(m);
    const float room = cell + 2 * st.FramePadding.x;
    if (ImGui::CalcTextSize(label.c_str()).x > room) {
      while (label.size() > 1 && ImGui::CalcTextSize((label + "...").c_str()).x > room) {
        label.pop_back();
      }
      label += "...";
    }
    ImGui::TextUnformatted(label.c_str());
    ImGui::EndGroup();
    ImGui::PopID();
  }
  if (!n) {
    ImGui::TextDisabled("No models");
  }
  ImGui::EndChild();
}

// ---- importing the user's models
// Blender: JAK_ENHANCED_BLENDER, else BLENDER, else the newest installed one
std::string find_blender() {
  for (const char* var : {"JAK_ENHANCED_BLENDER", "BLENDER"}) {
    const char* v = std::getenv(var);
    if (v && fs::exists(v)) {
      return v;
    }
  }
  std::error_code ec;
#ifdef _WIN32
  fs::path best;
  for (const auto& d : fs::directory_iterator("C:/Program Files/Blender Foundation", ec)) {
    const auto exe = d.path() / "blender.exe";
    if (fs::exists(exe) && (best.empty() || d.path().filename() > best.parent_path().filename())) {
      best = exe;
    }
  }
  return best.string();
#else
  for (const char* p : {"/usr/bin/blender", "/usr/local/bin/blender", "/snap/bin/blender"}) {
    if (fs::exists(p)) {
      return p;
    }
  }
  return "";
#endif
}

// a model's name from its file: user_<kind's tag>_<file's name>, lower case, letters, digits and _
std::string import_name(const std::string& src, int kind) {
  std::string stem;
  for (char c : fs::path(src).stem().string()) {
    stem += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '_';
  }
  const std::string base = fmt::format("user_{}_{}", kKinds[kind].tag, stem);
  const auto dir = file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] / "scenery";
  std::string name = base;
  for (int n = 2; fs::exists(dir / (name + ".glb")); n++) {
    name = fmt::format("{}_{}", base, n);
  }
  return name;
}

// Blender's run (scripts/enhanced/scenery_import.py), on its own thread: its RESULT line's json
// a .glb or .gltf: made ready here, in the game (EnhancedImport)
bool in_game_format(const std::string& src) {
  std::string ext = fs::path(src).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == ".glb" || ext == ".gltf";
}

std::string run_import(const std::string& blender, const std::string& src, const std::string& name, int faces) {
  const auto root = file_util::get_jak_project_dir();
  const auto script = root / "scripts" / "enhanced" / "scenery_import.py";
  const auto out = root / "custom_assets" / game_version_names[g_game_version] / "scenery";
  if (in_game_format(src)) {
    const auto r = enhanced_import_model(src, out.string(), name, faces);
    if (!r.ok) {
      return nlohmann::json{{"ok", false}, {"error", r.error}}.dump();
    }
    return nlohmann::json{{"ok", true},
                          {"faces_in", r.faces_in},
                          {"faces", r.faces},
                          {"lod_faces", r.lod_faces},
                          {"materials", r.materials},
                          {"size", {r.size[0], r.size[1], r.size[2]}}}
        .dump();
  }
  // (OBJ, FBX: Blender)
  const auto log = fs::temp_directory_path() / "jak_enhanced_import.log";
  std::string cmd = fmt::format(R"("{}" -b --factory-startup -P "{}" -- "{}" "{}" {} --max-faces {} > "{}" 2>&1)", blender,
                                script.string(), src, out.string(), name, faces, log.string());
#ifdef _WIN32
  cmd = "\"" + cmd + "\"";  // (cmd /c takes the whole line in quotes)
#endif
  std::system(cmd.c_str());
  std::string result;
  std::istringstream lines(fs::exists(log) ? file_util::read_text_file(log) : "");
  for (std::string line; std::getline(lines, line);) {
    if (line.rfind("RESULT ", 0) == 0) {
      result = line.substr(7);
    }
  }
  if (result.empty()) {
    result = nlohmann::json{{"ok", false}, {"error", "Blender gave no result, see " + log.string()}}.dump();
  }
  return result;
}

// the models a folder holds (the formats the import reads)
std::vector<std::string> models_in(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(dir, ec)) {
    std::string ext = f.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (f.is_regular_file() && (ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".fbx")) {
      out.push_back(f.path().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// the next of the files being imported started (the kind its name says, when several; the game's
// models their own names and kinds)
void start_import(Editor& e, const std::string& blender) {
  auto& m = e.imp;
  const size_t i = m.next++;
  const std::string src = m.files[i];
  const bool given = i < m.names.size();
  const int kind = given ? m.kinds[i]
                   : m.files.size() > 1 && m.guess_kind
                       ? std::min(kind_of(fs::path(src).filename().string()), (int)kKinds.size() - 1)
                       : m.kind;
  m.name = given ? m.names[i] : import_name(src, kind);
  const float scale = given ? m.scale : 1.f;
  const int faces = kKinds[kind].faces;
  m.running = std::async(std::launch::async, [blender, src, name = m.name, faces, scale] {
    if (scale != 1.f) {
      const auto out = file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] / "scenery";
      const auto r = enhanced_import_model(src, out.string(), name, faces, scale);
      return r.ok ? nlohmann::json{{"ok", true}, {"faces_in", r.faces_in}, {"faces", r.faces},
                                   {"lod_faces", r.lod_faces}, {"materials", r.materials}}.dump()
                  : nlohmann::json{{"ok", false}, {"error", r.error}}.dump();
    }
    return run_import(blender, src, name, faces);
  });
}

// the game's own models (game_models.json) found in the decompiler's levels output: their files,
// the names they get (game_<kind's tag>_<name>), their kinds; only the ones not imported yet
struct GameModels {
  std::vector<std::string> files, names;
  std::vector<int> kinds;
  int listed = 0;
};
GameModels find_game_models(const std::string& dir) {
  GameModels g;
  nlohmann::json list;
  try {
    list = parse_commented_json(
        file_util::read_text_file(file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "game_models.json"),
        "game_models.json");
  } catch (const std::exception& ex) {
    lg::error("[enhanced] game_models.json: {}", ex.what());
    return g;
  }
  // (each art group's file, in whichever level has it)
  std::unordered_map<std::string, std::string> found;
  std::error_code ec;
  for (const auto& lvl : fs::directory_iterator(dir, ec)) {
    if (!lvl.is_directory()) {
      continue;
    }
    for (const auto& f : fs::directory_iterator(lvl.path(), ec)) {
      const auto n = f.path().filename().string();
      if (n.size() > 9 && n.compare(n.size() - 9, 9, "-lod0.glb") == 0) {
        found.emplace(n.substr(0, n.size() - 9), f.path().string());
      }
    }
  }
  const auto out = file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] / "scenery";
  for (auto it = list.begin(); it != list.end(); ++it) {
    g.listed++;
    const auto at = found.find(it.key());
    if (at == found.end()) {
      continue;
    }
    int kind = (int)kKinds.size() - 1;
    for (size_t k = 0; k < kKinds.size(); k++) {
      if (it.value().get<std::string>() == kKinds[k].tag) {
        kind = (int)k;
      }
    }
    std::string stem = it.key();
    std::replace(stem.begin(), stem.end(), '-', '_');
    const std::string name = fmt::format("game_{}_{}", kKinds[kind].tag, stem);
    if (fs::exists(out / (name + ".glb"))) {
      continue;
    }
    g.files.push_back(at->second);
    g.names.push_back(name);
    g.kinds.push_back(kind);
  }
  return g;
}

// where the decompiler's levels output may be: the one set, else next to the game
std::string game_models_dir(const ModelImport& m) {
  if (m.game_dir[0]) {
    return m.game_dir;
  }
  if (const char* env = std::getenv("JAK_ENHANCED_GAME_MODELS"); env && *env) {
    return env;
  }
  for (const auto& d : {file_util::get_jak_project_dir() / "decompiler_out" / "jak3" / "levels",
                        file_util::get_jak_project_dir() / "decompiler_out" / "jak3_pal" / "levels"}) {
    if (fs::exists(d)) {
      return d.string();
    }
  }
  return "";
}

// the terrain's pictures: each may be replaced by one of the user's (the shipped one kept as
// <name>.orig.png, back with Restore), their repeats broken up by the composite (anti_tile)
struct TerrainSlot {
  const char* file;
  const char* name;
  const char* what;
};
constexpr TerrainSlot kTerrain[5] = {{"rock_detail.png", "Rock", "Cliffs' and boulders' close-up grain"},
                                     {"ground_detail.png", "Rocky Ground", "Stones and gravel"},
                                     {"sand_fine.png", "Fine Sand", "Rippled, the dunes"},
                                     {"sand_coarse.png", "Coarse Sand", "With grit and pebbles"},
                                     {"sand_crust.png", "Sand Crust", "Dry, cracked"}};

fs::path terrain_dir() {
  return file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] / "scenery";
}

void terrain_window(Editor& e) {
  static GLuint thumbs[5] = {};
  static int thumbs_version = -1;
  static std::future<std::string> job;  // a replacement being written: its error, if any
  static std::future<std::string> picking;
  static int picking_slot = -1;
  static std::string status;
  if (job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    status = job.get();
    g_enhanced_terrain.version++;
  }
  if (picking.valid() && picking.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const std::string src = picking.get();
    const int slot = picking_slot;
    if (!src.empty() && slot >= 0) {
      status = fmt::format("Making {} ready...", kTerrain[slot].name);
      // (as a PNG under the shipped one's name; that one kept the first time)
      job = std::async(std::launch::async, [src, slot] {
        const auto dst = terrain_dir() / kTerrain[slot].file;
        auto orig = dst;
        orig.replace_extension(".orig.png");
        std::error_code ec;
        if (fs::exists(dst) && !fs::exists(orig)) {
          fs::copy_file(dst, orig, ec);
        }
        int w = 0, h = 0, n = 0;
        u8* px = stbi_load(src.c_str(), &w, &h, &n, 4);
        if (!px) {
          return fmt::format("Couldn't read {}", src);
        }
        const bool ok = stbi_write_png(dst.string().c_str(), w, h, 4, px, w * 4) != 0;
        stbi_image_free(px);
        return ok ? fmt::format("{} replaced ({}x{})", kTerrain[slot].name, w, h)
                  : fmt::format("Couldn't write {}", dst.string());
      });
    }
  }
  if (!e.show_terrain) {
    return;
  }
  // (the pictures, small, read again when one changes)
  if (thumbs_version != g_enhanced_terrain.version.load()) {
    thumbs_version = g_enhanced_terrain.version.load();
    for (int i = 0; i < 5; i++) {
      if (thumbs[i]) {
        glDeleteTextures(1, &thumbs[i]);
        thumbs[i] = 0;
      }
      int w = 0, h = 0, n = 0;
      u8* px = stbi_load((terrain_dir() / kTerrain[i].file).string().c_str(), &w, &h, &n, 4);
      if (!px) {
        continue;
      }
      // (a corner of it, 512 at most: what a tile of ground shows)
      const int s = std::min({w, h, 512});
      std::vector<u8> crop((size_t)s * s * 4);
      for (int y = 0; y < s; y++) {
        std::memcpy(&crop[(size_t)y * s * 4], px + ((size_t)y * w) * 4, (size_t)s * 4);
      }
      stbi_image_free(px);
      glGenTextures(1, &thumbs[i]);
      glBindTexture(GL_TEXTURE_2D, thumbs[i]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s, s, 0, GL_RGBA, GL_UNSIGNED_BYTE, crop.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glGenerateMipmap(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
  const float k = ui_k();
  ImGui::SetNextWindowSize(ImVec2(560.f * k, 0.f), ImGuiCond_Appearing);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.4f, size.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::Begin("Terrain Textures", &e.show_terrain, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  float anti = g_enhanced_terrain.anti_tile.load();
  prop("Anti-tiling");
  if (ImGui::SliderFloat("##anti", &anti, 0.f, 1.f, "%.2f")) {
    g_enhanced_terrain.anti_tile = anti;
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    save_settings(e);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Breaks up the pictures' repeats: each patch of ground shows another part of them");
  }
  ImGui::Separator();
  const float thumb = 84.f * k;
  const bool busy = job.valid() || picking.valid();
  for (int i = 0; i < 5; i++) {
    ImGui::PushID(i);
    if (thumbs[i]) {
      ImGui::Image((ImTextureID)(intptr_t)thumbs[i], ImVec2(thumb, thumb));
    } else {
      ImGui::Dummy(ImVec2(thumb, thumb));
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(kBlueHi, "%s", kTerrain[i].name);
    ImGui::TextDisabled("%s", kTerrain[i].what);
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Replace...")) {
      picking_slot = i;
      picking = std::async(std::launch::async, [] {
        const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp"};
        const char* r = tinyfd_openFileDialog("A picture for the ground (it repeats: seamless is best)", "", 5,
                                              filters, "Pictures", 0);
        return std::string(r ? r : "");
      });
    }
    auto orig = terrain_dir() / kTerrain[i].file;
    orig.replace_extension(".orig.png");
    if (fs::exists(orig)) {
      ImGui::SameLine();
      if (ImGui::Button("Restore")) {
        std::error_code ec;
        fs::copy_file(orig, terrain_dir() / kTerrain[i].file, fs::copy_options::overwrite_existing, ec);
        fs::remove(orig, ec);
        status = fmt::format("{}: the original back", kTerrain[i].name);
        g_enhanced_terrain.version++;
      }
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::PopID();
  }
  if (!status.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", status.c_str());
  }
  ImGui::End();
}

void import_window(Editor& e) {
  auto& m = e.imp;
  static const std::string blender = find_blender();
  // (the dialog's answer)
  if (m.picking.valid() && m.picking.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto paths = m.picking.get();
    if (!paths.empty()) {
      m.files = paths;
      m.names.clear();
      std::snprintf(m.src, sizeof(m.src), "%s",
                    paths.size() == 1 ? paths[0].c_str() : fmt::format("{} files", paths.size()).c_str());
      // (the kind guessed from the file's name)
      m.kind = std::min(kind_of(fs::path(paths[0]).filename().string()), (int)kKinds.size() - 1);
      m.status.clear();
    }
  }
  if (m.running.valid() && m.running.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      const auto j = nlohmann::json::parse(m.running.get());
      if (!j.value("ok", false)) {
        m.failures.push_back(m.name + ": " + j.value("error", std::string("?")));
      } else {
        m.made++;
        const int in = j.value("faces_in", 0), out = j.value("faces", 0);
        m.status = in > out ? fmt::format("Imported {}: {} faces reduced to {} (far version {})", m.name, in, out,
                                          j.value("lod_faces", 0))
                            : fmt::format("Imported {}: {} faces (far version {})", m.name, out, j.value("lod_faces", 0));
        if (j.value("materials", 1) > 1) {
          m.status += fmt::format("\nIts {} materials' textures were joined in one.", j.value("materials", 1));
        }
        // (ready to place: the last one made; the ones made by themselves only shown)
        e.models = enhanced_scenery_models();
        const auto it = std::find(e.models.begin(), e.models.end(), m.name + ".glb");
        if (it != e.models.end() && !m.quiet) {
          e.model = (int)(it - e.models.begin());
          e.tool = kPlace;
          e.want_tab = kind_of(m.name);
        }
      }
    } catch (const std::exception& ex) {
      m.failures.push_back(m.name + ": " + ex.what());
    }
    if (m.next < m.files.size()) {
      start_import(e, blender);
    } else {
      // (all done: what came of them)
      if (m.files.size() > 1) {
        m.status = fmt::format("Imported {} of {} models", m.made, m.files.size());
      } else if (!m.failures.empty()) {
        m.status.clear();
      }
      for (const auto& f : m.failures) {
        m.status += (m.status.empty() ? "Import failed: " : "\nFailed: ") + f;
      }
      lg::info("editor: {}", m.status);
      m.quiet = false;
    }
  }
  // (the game's own models ready by themselves: the ones not in the library yet made in the
  // background the first time the editor opens, a few minutes, placeable as each is done)
  if (!m.auto_looked && !m.running.valid()) {
    m.auto_looked = true;
    const std::string dir = game_models_dir(m);
    if (!dir.empty() && fs::exists(dir)) {
      auto g = find_game_models(dir);
      if (!g.files.empty()) {
        lg::info("editor: making {} of the game's models ready in the background", g.files.size());
        m.files = g.files;
        m.names = g.names;
        m.kinds = g.kinds;
        m.scale = 0.25f;
        std::snprintf(m.src, sizeof(m.src), "%d game models", (int)m.files.size());
        m.status.clear();
        m.failures.clear();
        m.made = 0;
        m.next = 0;
        m.game_new = 0;
        m.quiet = true;
        start_import(e, blender);
      }
    }
  }
  if (!m.open) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(760.f * ui_k(), 0.f), ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * ui_k());
  ImGui::Begin("Import Models", &m.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  const bool busy = m.running.valid();
  ImGui::BeginDisabled(busy);
  const float labels = 70.f * ui_k();
  const float buttons = 110.f * ui_k();
  prop("File", labels);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 2.f * buttons - ImGui::GetStyle().ItemSpacing.x * 2.f);
  // (typed: one file; several come from the dialog or a folder)
  if (ImGui::InputTextWithHint("##src", "a .glb, .gltf, .obj or .fbx", m.src, sizeof(m.src))) {
    m.files = {m.src};
    m.names.clear();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(m.picking.valid());
  if (ImGui::Button("Files...", ImVec2(buttons, 0.f))) {
    m.picking = std::async(std::launch::async, [] {
      const char* filters[] = {"*.glb", "*.gltf", "*.obj", "*.fbx"};
      const char* r = tinyfd_openFileDialog("Import Models", "", 4, filters, "3D models", 1);
      // (several: separated by |)
      std::vector<std::string> out;
      std::istringstream in(r ? r : "");
      for (std::string f; std::getline(in, f, '|');) {
        if (!f.empty()) {
          out.push_back(f);
        }
      }
      return out;
    });
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("One model, or several (Ctrl or Shift click)");
  }
  ImGui::SameLine();
  if (ImGui::Button("Folder...", ImVec2(buttons, 0.f))) {
    m.picking = std::async(std::launch::async, [] {
      const char* r = tinyfd_selectFolderDialog("Import every model in a folder", "");
      return r ? models_in(r) : std::vector<std::string>();
    });
    m.names.clear();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Every model in a folder");
  }
  ImGui::EndDisabled();
  const bool several = m.files.size() > 1;
  prop("Kind", labels);
  if (several) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 2.f * buttons - ImGui::GetStyle().ItemSpacing.x * 2.f);
  }
  ImGui::BeginDisabled(several && m.guess_kind);
  const std::string kind_label = fmt::format("{} / {}", kCats[kKinds[m.kind].cat], kKinds[m.kind].name);
  if (ImGui::BeginCombo("##kind", several && m.guess_kind ? "From each file's name" : kind_label.c_str(),
                        ImGuiComboFlags_HeightLarge)) {
    for (int c = 0; c < kCatCount; c++) {
      ImGui::SeparatorText(kCats[c]);
      for (size_t k = 0; k < kKinds.size(); k++) {
        if (kKinds[k].cat == c && ImGui::Selectable(kKinds[k].name, m.kind == (int)k)) {
          m.kind = (int)k;
        }
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (several) {
    ImGui::SameLine();
    ImGui::Checkbox("From Names", &m.guess_kind);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Each file's kind from its name (grass, rock, pebble, ledge...)");
    }
    // (which ones)
    ImGui::BeginChild("##files", ImVec2(0.f, std::min(6, (int)m.files.size()) * ImGui::GetTextLineHeightWithSpacing() + 8.f * ui_k()),
                      ImGuiChildFlags_FrameStyle);
    for (size_t i = 0; i < m.files.size(); i++) {
      const auto name = fs::path(m.files[i]).filename().string();
      const int k = i < m.kinds.size() && i < m.names.size() ? m.kinds[i]
                    : m.guess_kind                             ? std::min(kind_of(name), (int)kKinds.size() - 1)
                                                               : m.kind;
      if (busy && i + 1 == m.next) {
        ImGui::TextColored(kBlueHi, "%s  (%s)", name.c_str(), kKinds[k].name);
      } else {
        ImGui::Text("%s  (%s)", name.c_str(), kKinds[k].name);
      }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Each kind has its most faces (grass %d, rocks %d, houses %d...): more are reduced.",
                        kKinds[0].faces, kKinds[2].faces, kKinds[8].faces);
  } else {
    ImGui::TextDisabled("Up to %d faces (more are reduced). Its far version and impostor are made too.",
                        kKinds[m.kind].faces);
  }
  ImGui::EndDisabled();
  ImGui::Spacing();
  // (.glb and .gltf are made ready in the game; OBJ and FBX need Blender)
  bool exist = !m.files.empty(), needs_blender = false;
  for (const auto& f : m.files) {
    exist &= fs::exists(f);
    needs_blender |= !in_game_format(f);
  }
  if (needs_blender && blender.empty()) {
    ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f),
                       "OBJ and FBX need Blender: install it (or set JAK_ENHANCED_BLENDER), or export a .glb.");
  }
  const bool can = !busy && exist && (!needs_blender || !blender.empty());
  ImGui::BeginDisabled(!can);
  ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueHi);
  if (ImGui::Button(several ? "Import All" : "Import", ImVec2(160.f * ui_k(), 0.f))) {
    m.status.clear();
    m.failures.clear();
    m.made = 0;
    m.next = 0;
    start_import(e, blender);
  }
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (busy) {
    static const char* kSpin = "|/-\\";
    const std::string what =
        several ? fmt::format("{} of {}: {}", m.next, m.files.size(), m.name) : m.name;
    ImGui::Text("%c  Making %s ready...", kSpin[(e.frames / 8) % 4], what.c_str());
  } else if (!m.status.empty()) {
    ImGui::TextColored(!m.failures.empty() ? ImVec4(1.f, 0.5f, 0.4f, 1.f) : ImVec4(0.6f, 0.9f, 0.6f, 1.f), "%s",
                       m.status.c_str());
  } else if (!m.files.empty() && !exist) {
    ImGui::TextDisabled("No such file");
  }

  // (the line above may have ended on SameLine with nothing after it)
  ImGui::Dummy(ImVec2(0.f, 0.f));
  ImGui::SeparatorText("Game Models");
  ImGui::TextWrapped("Jak 3's own props, plants, buildings and platforms, from the decompiler's levels output "
                     "(extractor with \"rip_levels\"): each goes in its kind under \"Game\".");
  ImGui::BeginDisabled(busy);
  prop("Folder", labels);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons - ImGui::GetStyle().ItemSpacing.x);
  std::string dir = game_models_dir(m);
  char shown_dir[512];
  std::snprintf(shown_dir, sizeof(shown_dir), "%s", dir.c_str());
  if (ImGui::InputTextWithHint("##gamedir", "decompiler_out/jak3/levels", shown_dir, sizeof(shown_dir))) {
    std::snprintf(m.game_dir, sizeof(m.game_dir), "%s", shown_dir);
    m.game_new = -1;
    save_settings(e);
  }
  ImGui::SameLine();
  static std::future<std::string> picking_dir;
  if (picking_dir.valid() && picking_dir.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto d = picking_dir.get();
    if (!d.empty()) {
      std::snprintf(m.game_dir, sizeof(m.game_dir), "%s", d.c_str());
      m.game_new = -1;
      save_settings(e);
    }
  }
  ImGui::BeginDisabled(picking_dir.valid());
  if (ImGui::Button("Browse...", ImVec2(buttons, 0.f))) {
    picking_dir = std::async(std::launch::async, [] {
      const char* r = tinyfd_selectFolderDialog("The decompiler's levels folder (decompiler_out/jak3/levels)", "");
      return std::string(r ? r : "");
    });
  }
  ImGui::EndDisabled();
  if (m.game_new < 0 && !dir.empty()) {
    m.game_new = (int)find_game_models(dir).files.size();
  }
  ImGui::BeginDisabled(m.game_new <= 0);
  ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueHi);
  const std::string label = m.game_new > 0 ? fmt::format("Import {} Game Models", m.game_new) : "Import Game Models";
  if (ImGui::Button(label.c_str(), ImVec2(220.f * ui_k(), 0.f))) {
    auto g = find_game_models(dir);
    m.files = g.files;
    m.names = g.names;
    m.kinds = g.kinds;
    m.scale = 0.25f;
    std::snprintf(m.src, sizeof(m.src), "%d game models", (int)m.files.size());
    m.status.clear();
    m.failures.clear();
    m.made = 0;
    m.next = 0;
    m.game_new = 0;
    if (!m.files.empty()) {
      start_import(e, blender);
    }
  }
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (dir.empty() || !fs::exists(dir)) {
    ImGui::TextDisabled("Choose the folder first");
  } else if (m.game_new == 0) {
    ImGui::TextDisabled("All of them are in the library");
  }
  ImGui::EndDisabled();
  ImGui::End();
}

void frame_selection(Editor& e);
float status_height();

// the side panel: the tool's settings, what's selected
constexpr float kPanelMin = 300.f, kPanelMax = 900.f;

float panel_width(const Editor& e) {
  return std::clamp(e.panel_w, kPanelMin, std::min(kPanelMax, ImGui::GetIO().DisplaySize.x * 0.6f / ui_k())) *
         ui_k();
}

// the tab on the panel's left edge (on the screen's right edge when it's closed), as Blender's:
// a chevron, clicked to close or open it
void panel_tab(Editor& e) {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float k = ui_k();
  const ImVec2 tab(20.f * k, 64.f * k);
  const float right = e.show_panel ? size.x - panel_width(e) : size.x;
  const float y = ImGui::GetFrameHeight() + 40.f * k;
  ImGui::SetNextWindowPos(ImVec2(right - tab.x, y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(tab, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
  ImGui::Begin("##panel_tab", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool click = ImGui::InvisibleButton("##tab", tab);
  const bool hot = ImGui::IsItemHovered();
  if (hot) {
    ImGui::SetTooltip(e.show_panel ? "Hide the sidebar (N)" : "Show the sidebar (N)");
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 b(a.x + tab.x, a.y + tab.y);
  dl->AddRectFilled(a, b, hot ? IM_COL32(84, 84, 84, 255) : IM_COL32(48, 48, 48, 240), 6.f * k,
                    ImDrawFlags_RoundCornersLeft);
  dl->AddRect(a, b, IM_COL32(22, 22, 22, 255), 6.f * k, ImDrawFlags_RoundCornersLeft);
  // (pointing where it goes: right closes, left opens)
  const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  const float s = 5.f * k, dir = e.show_panel ? 1.f : -1.f;
  const ImU32 col = hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255);
  dl->AddLine(ImVec2(c.x - dir * s * 0.5f, c.y - s), ImVec2(c.x + dir * s * 0.5f, c.y), col, 2.f * k);
  dl->AddLine(ImVec2(c.x + dir * s * 0.5f, c.y), ImVec2(c.x - dir * s * 0.5f, c.y + s), col, 2.f * k);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (click) {
    e.show_panel = !e.show_panel;
    save_settings(e);
  }
}

// ---- the Textures tool: a click on the world, the textures of what's there (EnhancedTextures)
std::vector<LevelData*> g_levels;  // this frame's, from the post pass

struct TexTool {
  bool have = false;
  EnhancedTexPick pick;
  V3 at = {0, 0, 0};
  std::string msg;
  int sel = -1;
  // the selected one: its picture before the sliders (the user's version, else the game's)
  std::string key;
  std::vector<u8> base;
  int bw = 0, bh = 0;
  bool edited = false;
  fs::file_time_type mtime;  // its file's, when last read or written here (changed: read again)
  u64 checked = 0;
  float hue = 0.f, sat = 1.f, bright = 0.f, contrast = 1.f;
  float tint[3] = {1.f, 0.6f, 0.4f};
  float tint_amt = 0.f;
  std::future<std::string> picking;  // Replace's file dialog
  std::future<std::string> saving;   // the file being written: its error, if any
};
TexTool g_tex;

const EnhancedTexRef* tex_selected() {
  auto& t = g_tex;
  return t.have && t.sel >= 0 && t.sel < (int)t.pick.textures.size() ? &t.pick.textures[t.sel] : nullptr;
}

void tex_reset_sliders() {
  g_tex.hue = 0.f;
  g_tex.sat = 1.f;
  g_tex.bright = 0.f;
  g_tex.contrast = 1.f;
  g_tex.tint_amt = 0.f;
}

// the selected one's picture read: the user's version if there's one, else the game's
void tex_load_base(const EnhancedTexRef& r) {
  auto& t = g_tex;
  t.key = r.tpage + "/" + r.name;
  t.base.clear();
  const auto path = enhanced_texture_edit_path(r.tpage, r.name);
  std::error_code ec;
  t.edited = fs::exists(path, ec);
  if (t.edited) {
    int n = 0;
    if (u8* px = stbi_load(path.string().c_str(), &t.bw, &t.bh, &n, 4)) {
      t.base.assign(px, px + (size_t)t.bw * t.bh * 4);
      stbi_image_free(px);
      t.mtime = fs::last_write_time(path, ec);
      return;
    }
  }
  enhanced_world_texture_original(g_levels, r.tpage, r.name, &t.base, &t.bw, &t.bh);
}

bool tex_sliders_moved() {
  const auto& t = g_tex;
  return t.hue != 0.f || t.sat != 1.f || t.bright != 0.f || t.contrast != 1.f || t.tint_amt > 0.f;
}

// the base with the sliders: hue turned, saturation, brightness, contrast, a tint
std::vector<u8> tex_adjusted() {
  const auto& t = g_tex;
  std::vector<u8> out = t.base;
  if (!tex_sliders_moved()) {
    return out;
  }
  const float a = t.hue * kPi / 180.f, c = std::cos(a), s = std::sin(a);
  const float m[3][3] = {{0.299f + 0.701f * c + 0.168f * s, 0.587f - 0.587f * c + 0.330f * s, 0.114f - 0.114f * c - 0.497f * s},
                         {0.299f - 0.299f * c - 0.328f * s, 0.587f + 0.413f * c + 0.035f * s, 0.114f - 0.114f * c + 0.292f * s},
                         {0.299f - 0.300f * c + 1.250f * s, 0.587f - 0.588f * c - 1.050f * s, 0.114f + 0.886f * c - 0.203f * s}};
  for (size_t i = 0; i + 3 < out.size(); i += 4) {
    float v[3], r[3];
    for (int k = 0; k < 3; k++) {
      v[k] = t.base[i + k] / 255.f;
    }
    for (int k = 0; k < 3; k++) {
      r[k] = m[k][0] * v[0] + m[k][1] * v[1] + m[k][2] * v[2];
    }
    const float lum = 0.299f * r[0] + 0.587f * r[1] + 0.114f * r[2];
    for (int k = 0; k < 3; k++) {
      float x = lum + (r[k] - lum) * t.sat;
      x = x * (1.f - t.tint_amt) + lum * t.tint[k] * 1.6f * t.tint_amt;
      x = (x - 0.5f) * t.contrast + 0.5f + t.bright;
      out[i + k] = (u8)std::clamp(x * 255.f + 0.5f, 0.f, 255.f);
    }
  }
  return out;
}

// written as the user's version (in the background), in use at once
void tex_save(const EnhancedTexRef& r, std::vector<u8> px, int w, int h) {
  auto& t = g_tex;
  enhanced_world_texture_upload(g_levels, r.tpage, r.name, px.data(), w, h);
  enhanced_note_texture_edit(r.tpage, r.name, true);
  t.edited = true;
  const auto path = enhanced_texture_edit_path(r.tpage, r.name);
  t.saving = std::async(std::launch::async, [path, px = std::move(px), w, h] {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return stbi_write_png(path.string().c_str(), w, h, 4, px.data(), w * 4) ? std::string()
                                                                             : "Couldn't write " + path.string();
  });
}

void texture_pick(Editor& e) {
  auto& t = g_tex;
  if (!e.hover_ok) {
    return;
  }
  // (a little slack, more far away: the depth's point is a bit off the triangle)
  const float dist = length(e.hover - cam_pos(e));
  const float margin = std::max(0.15f * kMeter, dist * 0.006f);
  EnhancedTexPick pick;
  if (enhanced_pick_world_textures(g_levels, &e.hover[0], margin, &pick)) {
    t.pick = std::move(pick);
    t.have = true;
    t.at = e.hover;
    t.sel = 0;
    t.msg.clear();
    tex_reset_sliders();
    tex_load_base(t.pick.textures[0]);
  } else {
    t.have = false;
    t.sel = -1;
    t.msg = "Nothing with a texture of the level's there (the desert's ground: View > Terrain Textures; "
            "placed models and characters aren't listed).";
  }
}

void texture_panel(Editor& e, float w) {
  auto& t = g_tex;
  const float k = ui_k();
  // a file written, a picture picked, the user's paint program saving it
  if (t.saving.valid() && t.saving.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto err = t.saving.get();
    if (!err.empty()) {
      t.msg = err;
    }
    if (const auto* r = tex_selected()) {
      std::error_code ec;
      t.mtime = fs::last_write_time(enhanced_texture_edit_path(r->tpage, r->name), ec);
    }
  }
  if (t.picking.valid() && t.picking.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto src = t.picking.get();
    const auto* r = tex_selected();
    int pw = 0, ph = 0, n = 0;
    u8* px = src.empty() || !r ? nullptr : stbi_load(src.c_str(), &pw, &ph, &n, 4);
    if (px) {
      t.base.assign(px, px + (size_t)pw * ph * 4);
      t.bw = pw;
      t.bh = ph;
      stbi_image_free(px);
      tex_reset_sliders();
      tex_save(*r, t.base, t.bw, t.bh);
      t.msg = fmt::format("{} replaced ({}x{})", r->name, pw, ph);
    } else if (!src.empty()) {
      t.msg = "Couldn't read " + src;
    }
  }
  if (const auto* r = tex_selected(); r && t.edited && !t.saving.valid() && e.frames - t.checked > 30) {
    t.checked = e.frames;
    std::error_code ec;
    const auto path = enhanced_texture_edit_path(r->tpage, r->name);
    const auto now = fs::last_write_time(path, ec);
    if (!ec && now != t.mtime) {
      // (saved in the paint program: read again, a moment after, once it's done writing)
      tex_load_base(*r);
      tex_reset_sliders();
      if (!t.base.empty()) {
        enhanced_world_texture_upload(g_levels, r->tpage, r->name, t.base.data(), t.bw, t.bh);
        t.msg = r->name + ": the paint program's changes are in";
      }
    }
  }

  ImGui::TextWrapped("Click something in the world: the textures it's made of. Replace them, edit them "
                     "in your paint program (saving there shows it here), or recolor them.");
  if (!t.have) {
    if (!t.msg.empty()) {
      ImGui::TextDisabled("%s", t.msg.c_str());
    }
    if (ImGui::Button("Terrain Textures...")) {
      e.show_terrain = true;
    }
    return;
  }
  ImGui::TextColored(kBlueHi, "%s", t.pick.what.c_str());
  // the thumbnails
  const float th = 58.f * k;
  const int per_row = std::max(1, (int)((w + 4.f * k) / (th + 8.f * k)));
  for (int i = 0; i < (int)t.pick.textures.size(); i++) {
    const auto& r = t.pick.textures[i];
    ImGui::PushID(i);
    if (i % per_row) {
      ImGui::SameLine();
    }
    const bool on = i == t.sel;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? kBlue : grey(0.157f));
    if (ImGui::ImageButton("##t", (ImTextureID)(intptr_t)r.gl, ImVec2(th, th))) {
      if (i != t.sel) {
        t.sel = i;
        tex_reset_sliders();
        tex_load_base(r);
      }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\n%s  %dx%d%s", r.name.c_str(), r.tpage.c_str(), r.w, r.h,
                        enhanced_has_texture_edit(r.tpage, r.name) ? "\n(edited)" : "");
    }
    ImGui::PopID();
  }
  const auto* r = tex_selected();
  if (!r) {
    return;
  }
  ImGui::Separator();
  const float big = std::min(w, 256.f * k);
  ImGui::Image((ImTextureID)(intptr_t)r->gl, ImVec2(big, big * (float)std::max(t.bh, 1) / std::max(t.bw, 1)));
  ImGui::Text("%s", r->name.c_str());
  ImGui::TextDisabled("%s  %dx%d%s", r->tpage.c_str(), t.bw, t.bh, t.edited ? "  (edited)" : "");
  const bool busy = t.picking.valid() || t.saving.valid();
  ImGui::BeginDisabled(busy);
  if (ImGui::Button("Replace...")) {
    t.picking = std::async(std::launch::async, [] {
      const char* filters[] = {"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp"};
      const char* f = tinyfd_openFileDialog("A picture to use instead", "", 5, filters, "Pictures", 0);
      return std::string(f ? f : "");
    });
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Any size: it's stretched over the same places");
  }
  ImGui::SameLine();
  if (ImGui::Button("Edit in Paint Program")) {
    const auto path = enhanced_texture_edit_path(r->tpage, r->name);
    // (written first: the sliders' version, plain alpha)
    std::vector<u8> px = tex_adjusted();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (stbi_write_png(path.string().c_str(), t.bw, t.bh, 4, px.data(), t.bw * 4)) {
      enhanced_note_texture_edit(r->tpage, r->name, true);
      t.edited = true;
      t.base = std::move(px);
      tex_reset_sliders();
      t.mtime = fs::last_write_time(path, ec);
      enhanced_open_file(path);
      t.msg = "Opened " + path.filename().string() + ": save it there to see it here";
    } else {
      t.msg = "Couldn't write " + path.string();
    }
  }
  if (t.edited) {
    ImGui::SameLine();
    if (ImGui::Button("Restore")) {
      std::error_code ec;
      fs::remove(enhanced_texture_edit_path(r->tpage, r->name), ec);
      enhanced_note_texture_edit(r->tpage, r->name, false);
      enhanced_world_texture_upload(g_levels, r->tpage, r->name, nullptr, 0, 0);
      tex_reset_sliders();
      tex_load_base(*r);
      t.msg = r->name + ": the game's own back";
    }
  }
  ImGui::EndDisabled();
  ImGui::SeparatorText("Recolor");
  bool changed = false, done = false;
  auto track = [&](bool c) {
    changed |= c;
    done |= ImGui::IsItemDeactivatedAfterEdit();
  };
  prop("Hue");
  track(ImGui::SliderFloat("##hue", &t.hue, -180.f, 180.f, "%.0f deg"));
  prop("Saturation");
  track(ImGui::SliderFloat("##sat", &t.sat, 0.f, 2.f, "%.2f"));
  prop("Brightness");
  track(ImGui::SliderFloat("##bright", &t.bright, -0.5f, 0.5f, "%+.2f"));
  prop("Contrast");
  track(ImGui::SliderFloat("##contrast", &t.contrast, 0.f, 2.f, "%.2f"));
  prop("Tint");
  track(ImGui::ColorEdit3("##tint", t.tint, ImGuiColorEditFlags_NoInputs));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.f);
  track(ImGui::SliderFloat("##tintamt", &t.tint_amt, 0.f, 1.f, "%.2f"));
  if (changed && !t.base.empty()) {
    // (seen while dragging; kept when let go)
    const auto px = tex_adjusted();
    enhanced_world_texture_upload(g_levels, r->tpage, r->name, px.data(), t.bw, t.bh);
  }
  if (done && !t.base.empty() && !busy) {
    tex_save(*r, tex_adjusted(), t.bw, t.bh);
  }
  if (tex_sliders_moved() && ImGui::Button("Reset Sliders")) {
    tex_reset_sliders();
    if (t.edited) {
      tex_save(*r, t.base, t.bw, t.bh);
    } else {
      enhanced_world_texture_upload(g_levels, r->tpage, r->name, nullptr, 0, 0);
    }
  }
  if (!t.msg.empty()) {
    ImGui::TextDisabled("%s", t.msg.c_str());
  }
}

void side_panel(Editor& e) {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float width = panel_width(e);
  const float top = ImGui::GetFrameHeight();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * ui_k(), 8.f * ui_k()));
  ImGui::SetNextWindowPos(ImVec2(size.x - width, top), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, size.y - top - status_height()), ImGuiCond_Always);
  ImGui::Begin("##panel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  {
    // its left edge, dragged: the panel wider or narrower (an item that takes no room: the panel
    // isn't made taller than its window, so it doesn't scroll)
    const ImVec2 wpos = ImGui::GetWindowPos();
    const float grip = 6.f * ui_k();
    const ImRect bb(wpos, ImVec2(wpos.x + grip, wpos.y + ImGui::GetWindowHeight()));
    const ImGuiID id = ImGui::GetID("##resize");
    ImGui::ItemAdd(bb, id);
    bool hovered = false, held = false;
    ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const bool hot = hovered || held;
    if (hot) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      ImGui::GetWindowDrawList()->AddRectFilled(
          wpos, ImVec2(wpos.x + 2.f * ui_k(), wpos.y + ImGui::GetWindowHeight()), ImGui::GetColorU32(kBlue));
    }
    if (held) {
      e.panel_w = std::clamp((size.x - ImGui::GetIO().MousePos.x) / ui_k(), kPanelMin, kPanelMax);
      e.panel_resizing = true;
    } else if (e.panel_resizing) {
      e.panel_resizing = false;
      save_settings(e);
    }
  }
  const float w = ImGui::GetContentRegionAvail().x;
  bool grass_changed = false;
  const std::string tool_header = fmt::format("Tool: {}###tool", kToolNames[e.tool]);
  const int shown = ImGui::CollapsingHeader(tool_header.c_str(), ImGuiTreeNodeFlags_DefaultOpen) ? e.tool : -1;
  switch (shown) {
    case kGrass: {
      ImGui::TextWrapped("Paint grass anywhere (Ctrl: erase). The settings below change all the painted grass.");
      e.grass_mode = 0;
      prop("Brush");
      ImGui::SliderFloat("##Radius", &e.grass_brush, 0.5f, 40.f, "%.1f m", ImGuiSliderFlags_Logarithmic);
      ImGui::SeparatorText("Grass");
      {
        // (how it looks: all the painted grass, live)
        auto st = enhanced_grass_style();
        bool look = false;
        auto tip = [](const char* t) {
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", t);
          }
        };
        prop("Height");
        look |= ImGui::SliderFloat("##GHeight", &st.height, 0.2f, 3.f, "%.2fx");
        tip("How tall the blades grow (1x: knee high)");
        prop("Thickness");
        look |= ImGui::SliderFloat("##GWidth", &st.width, 0.3f, 4.f, "%.2fx");
        tip("How wide each blade is");
        prop("Density");
        float pct = st.density * 100.f;
        look |= ImGui::SliderFloat("##GDensity", &pct, 5.f, 100.f, "%.0f%%");
        st.density = pct / 100.f;
        tip("How many blades");
        prop("Thinning");
        ImGui::SliderFloat("##Falloff", &e.s.fade, 1.f, 60.f, "%.0f m");
        tip("How far past the strokes it thins out to nothing");
        // (applied when let go: each bake takes a moment)
        if (ImGui::IsItemActivated()) {
          remember(e);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          grass_changed = true;
        }
        prop("Wind");
        look |= ImGui::SliderFloat("##GWind", &st.wind, 0.f, 3.f, "%.2fx");
        tip("How much the gusts bend it (0: still)");
        prop("Color");
        look |= ImGui::ColorEdit3("##GColor", st.color, ImGuiColorEditFlags_Float);
        prop("Tips");
        look |= ImGui::ColorEdit3("##GDry", st.dry, ImGuiColorEditFlags_Float);
        tip("The drier color some patches' tips turn");
        if (look) {
          enhanced_set_grass_style(st);
          e.dirty = true;
        }
        if (ImGui::Button("Default Look")) {
          enhanced_set_grass_style(EnhancedGrassStyle());
          e.dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All Painted Grass")) {
          remember(e);
          e.s.lush.clear();
          e.s.edge.clear();
          grass_changed = true;
        }
      }
      if (e.advanced) {
        int zones = 0, mine = 0, fields = 0;
        enhanced_grass_counts(&zones, &mine, &fields);
        ImGui::TextDisabled("%d strokes; %d grass zones (%d made by the brush, %d of them baked)",
                            (int)(e.s.lush.size() + e.s.edge.size()), zones, mine, fields);
      }
      break;
    }
    case kPlace: {
      ImGui::RadioButton("Place", &e.place_mode, 0);
      ImGui::SameLine();
      ImGui::RadioButton("Tint", &e.place_mode, 1);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Paint a color over the objects the brush goes over (Ctrl: back to their own)");
      }
      if (e.place_mode == 1) {
        prop("Brush");
        ImGui::SliderFloat("##TintRadius", &e.place_brush, 0.5f, 40.f, "%.1f m", ImGuiSliderFlags_Logarithmic);
        prop("Color");
        ImGui::ColorEdit3("##TintColor", e.tint_color, ImGuiColorEditFlags_Float);
        prop("Strength");
        ImGui::SliderFloat("##TintStrength", &e.tint_strength, 0.02f, 1.f, "%.2f");
        break;
      }
      model_browser(e, w);
      if (ImGui::Button("Import Model...", ImVec2(-FLT_MIN, 0.f))) {
        e.imp.open = true;
      }
      if (e.mix.size() > 1) {
        ImGui::SeparatorText("Mix");
        // (each model: how often it comes)
        int drop = -1;
        for (size_t i = 0; i < e.mix.size(); i++) {
          ImGui::PushID((int)i);
          float wgt = mix_weight(e, e.mix[i]) * 100.f;
          std::string label = pretty(e.mix[i]);
          prop(label.c_str());
          ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                                  ImGui::GetStyle().ItemSpacing.x);
          if (ImGui::SliderFloat("##w", &wgt, 0.f, 100.f, "%.0f%%")) {
            e.mix_weight[e.mix[i]] = wgt / 100.f;
          }
          ImGui::SameLine();
          if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0.f))) {
            drop = (int)i;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Out of the mix");
          }
          ImGui::PopID();
        }
        if (drop >= 0) {
          e.mix.erase(e.mix.begin() + drop);
          if (e.mix.size() <= 1) {
            const auto it = e.mix.empty() ? e.models.end() : std::find(e.models.begin(), e.models.end(), e.mix[0]);
            e.model = it == e.models.end() ? -1 : (int)(it - e.models.begin());
            e.mix.clear();
          }
        }
        prop("Patches");
        ImGui::SliderFloat("##Patches", &e.patchiness, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("0: all mixed. 1: each patch of ground one model.");
        }
        prop("Patch Size");
        ImGui::SliderFloat("##Patch Size", &e.patch_size, 0.5f, 40.f, "%.1f m", ImGuiSliderFlags_Logarithmic);
      } else {
        ImGui::TextDisabled("Tip: Ctrl+click or Shift+click more models to paint them mixed");
      }
      ImGui::SeparatorText("Brush");
      prop("Radius");
      ImGui::SliderFloat("##Radius", &e.place_brush, 0.5f, 40.f, "%.1f m", ImGuiSliderFlags_Logarithmic);
      prop("Amount");
      ImGui::SliderInt("##Amount", &e.per_dab, 1, 20, "%d per dab");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many objects each dab of the brush lays down (fewer: hold the click "
                          "shorter, or a bigger Radius to spread them)");
      }
      if (e.advanced) {
        prop("Spread");
        ImGui::SliderFloat("##Spread", &e.spread, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("How far over the brush they're strewn: 0 all at the cursor, 1 the whole circle");
        }
        prop("Scale");
        ImGui::DragFloatRange2("##Scale", &e.scale_lo, &e.scale_hi, 0.01f, 0.05f, 20.f, "min %.2f",
                               "max %.2f");
        prop("Rotation");
        ImGui::BeginDisabled(!e.random_yaw);
        ImGui::SliderFloat("##Rotation", &e.yaw_jitter, 0.f, 180.f, "+/- %.0f deg");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("How far each one is turned at random, either way");
        }
        ImGui::Checkbox("Random Rotation", &e.random_yaw);
        ImGui::Checkbox("Ctrl Erases Any Model", &e.erase_any);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("On: Ctrl+drag erases every model under the brush.\n"
                            "Off: only the chosen one.");
        }
      }
      ImGui::SeparatorText("Gravity");
      if (ImGui::Checkbox("Fall to the Ground", &e.gravity)) {
        save_settings(e);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: objects start above and fall onto the game's collision.\n"
                          "Off: they stay where you click.");
      }
      if (e.advanced && e.gravity) {
        prop("Drop Height");
        ImGui::SliderFloat("##Drop Height", &e.drop_height, 0.f, 30.f, "%.1f m");
      }
      break;
    }
    case kLight:
      ImGui::TextWrapped("Click: a new light there. Click a light: select it.");
      ImGui::BeginChild("##lights", ImVec2(w, 160.f * ui_k()), true);
      for (size_t i = 0; i < e.s.lights.size(); i++) {
        const auto& l = e.s.lights[i];
        bool sel = std::find(e.sel.begin(), e.sel.end(), Pick{1, (int)i}) != e.sel.end();
        std::string name = fmt::format("{} {}##l{}", i + 1, l.note.empty() ? "light" : l.note, i);
        if (ImGui::Selectable(name.c_str(), sel)) {
          e.sel = {Pick{1, (int)i}};
        }
      }
      ImGui::EndChild();
      break;
    case kMeasure:
      ImGui::TextWrapped("Drag from one point to another: how far, how much higher, how far across.");
      if (e.have_measure || e.measuring) {
        const V3 d = (e.measure_b - e.measure_a) * (1.f / kMeter);
        ImGui::Text("Distance  %.2f m", length(d));
        ImGui::Text("Height    %+.2f m", d[1]);
        ImGui::Text("Across    %.2f m", std::hypot(d[0], d[2]));
      }
      break;
    case kTexture:
      texture_panel(e, w);
      break;
    case kNote:
      ImGui::TextWrapped("Draw notes on the ground (1-6 color, E eraser, Backspace undo, H hide).");
      break;
    default:
      break;
  }
  if (grass_changed) {
    push_grass(e);
    e.dirty = true;
  }

  // what's selected (Blender's Item panel)
  if (!e.sel.empty() && ImGui::CollapsingHeader("Item", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (e.sel.size() > 1) {
      ImGui::Text("%d selected", (int)e.sel.size());
      collision_combo(e);
      gameplay_section(e);
    } else if (e.sel[0].kind == 0) {
      auto& o = e.s.objects[e.sel[0].idx];
      ImGui::TextColored(kBlueHi, "%s", pretty(o.model).c_str());
      // (as Blender shows them: its axes, Z up; the turn as XYZ Euler angles in degrees)
      const V3 bp = to_blender(V3{o.pos[0], o.pos[1], o.pos[2]} * (1.f / kMeter));
      float pos[3] = {bp[0], bp[1], bp[2]};
      const V3 eu = quat_to_euler(rot_of(o));
      float rot[3] = {eu[0] * 180.f / kPi, eu[1] * 180.f / kPi, eu[2] * 180.f / kPi};
      float scl[3] = {o.scale[0], o.scale[2], o.scale[1]};
      bool changed = false, turned = false, done = false;
      if (e.advanced) {
        prop("Location");
        changed |= ImGui::DragFloat3("##Location", pos, 0.05f, 0.f, 0.f, "%.2f");
        if (ImGui::IsItemActivated()) {
          remember(e);
        }
        done |= ImGui::IsItemDeactivatedAfterEdit();
      }
      prop("Rotation");
      turned = ImGui::DragFloat3("##Rotation", rot, 1.f, 0.f, 0.f, "%.1f");
      changed |= turned;
      if (ImGui::IsItemActivated()) {
        remember(e);
      }
      done |= ImGui::IsItemDeactivatedAfterEdit();
      collision_combo(e);
      prop("Scale");
      changed |= ImGui::DragFloat3("##Scale", scl, 0.01f, 0.01f, 100.f, "%.2f");
      if (ImGui::IsItemActivated()) {
        remember(e);
      }
      done |= ImGui::IsItemDeactivatedAfterEdit();
      if (e.advanced) {
        prop("Tint");
        changed |= ImGui::ColorEdit3("##Tint", o.tint, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        if (ImGui::IsItemActivated()) {
          remember(e);
        }
        done |= ImGui::IsItemDeactivatedAfterEdit();
      }
      if (changed) {
        const V3 gp = from_blender(V3{pos[0], pos[1], pos[2]}) * kMeter;
        for (int k = 0; k < 3; k++) {
          o.pos[k] = gp[k];
        }
        if (turned) {
          set_rot(o, euler_to_quat(V3{rot[0], rot[1], rot[2]} * (kPi / 180.f)));
        }
        // (Blender's Y the game's z, its Z the game's up)
        o.scale[0] = std::clamp(scl[0], 0.01f, 100.f);
        o.scale[2] = std::clamp(scl[1], 0.01f, 100.f);
        o.scale[1] = std::clamp(scl[2], 0.01f, 100.f);
        push_objects(e);
      }
      if (done) {
        e.dirty = true;
      }
      gameplay_section(e);
    } else {
      auto& l = e.s.lights[e.sel[0].idx];
      bool changed = false, done = false;
      auto track = [&](bool c) {
        changed |= c;
        if (ImGui::IsItemActivated()) {
          remember(e);
        }
        done |= ImGui::IsItemDeactivatedAfterEdit();
      };
      if (e.advanced) {
        char note[64];
        std::snprintf(note, sizeof(note), "%s", l.note.c_str());
        prop("Name");
        track(ImGui::InputText("##Name", note, sizeof(note)));
        l.note = note;
      }
      prop("Color");
      track(ImGui::ColorEdit3("##Color", l.color, ImGuiColorEditFlags_Float));
      prop("Power");
      track(ImGui::SliderFloat("##Power", &l.intensity, 0.f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic));
      float radius = l.radius / kMeter;
      prop("Radius");
      track(ImGui::SliderFloat("##Radius", &radius, 0.3f, 60.f, "%.1f m", ImGuiSliderFlags_Logarithmic));
      l.radius = radius * kMeter;
      float y = l.pos[1] / kMeter;
      prop("Height");
      track(ImGui::DragFloat("##Height", &y, 0.05f, 0.f, 0.f, "%.2f m"));
      l.pos[1] = y * kMeter;
      track(ImGui::Checkbox("Flicker (fire)", &l.flicker));
      ImGui::SameLine();
      track(ImGui::Checkbox("Night Only", &l.night));
      if (e.advanced) {
        track(ImGui::Checkbox("Visible Bulb", &l.bulb));
      }
      if (changed) {
        enhanced_set_lights(e.s.lights, false);
      }
      if (done) {
        e.dirty = true;
      }
    }
  }
  if (e.advanced && ImGui::CollapsingHeader("Outliner")) {
    // (selected rows blue, as Blender's outliner)
    ImGui::PushStyleColor(ImGuiCol_Header, kBlue);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kBlueHi);
    ImGui::BeginChild("##scene", ImVec2(w, 300.f * ui_k()), true);
    // (each object: its eye, its lock, its name; the groups first, each with an eye and a lock for
    // all of it, then the rest)
    auto set_all = [&](const std::vector<size_t>& which, bool hide_it, bool flip_hidden) {
      remember(e);
      bool to = true;
      for (size_t i : which) {
        to &= flip_hidden ? e.s.objects[i].hidden : e.s.objects[i].locked;
      }
      for (size_t i : which) {
        (flip_hidden ? e.s.objects[i].hidden : e.s.objects[i].locked) = !to;
      }
      std::erase_if(e.sel, [&](const Pick& p) {
        return p.kind == 0 && (e.s.objects[p.idx].hidden || e.s.objects[p.idx].locked);
      });
      commit(e, flip_hidden, false, false);
      (void)hide_it;
    };
    auto row = [&](size_t i) {
      auto& o = e.s.objects[i];
      ImGui::PushID((int)i);
      if (icon_toggle("##eye", true, !o.hidden)) {
        set_all({i}, true, true);
      }
      ImGui::SameLine(0.f, 2.f);
      if (icon_toggle("##lock", false, o.locked)) {
        set_all({i}, false, false);
      }
      ImGui::SameLine();
      const bool sel = std::find(e.sel.begin(), e.sel.end(), Pick{0, (int)i}) != e.sel.end();
      std::string name = fmt::format("{}  {}", i + 1, pretty(o.model));
      ImGui::BeginDisabled(o.hidden || o.locked);
      if (ImGui::Selectable(name.c_str(), sel)) {
        if (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl) {
          if (sel) {
            std::erase(e.sel, Pick{0, (int)i});
          } else {
            e.sel.push_back({0, (int)i});
          }
        } else {
          e.sel = {Pick{0, (int)i}};
        }
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        e.sel = {Pick{0, (int)i}};
        frame_selection(e);
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    };
    std::vector<std::string> groups;
    for (const auto& o : e.s.objects) {
      if (!o.group.empty() && std::find(groups.begin(), groups.end(), o.group) == groups.end()) {
        groups.push_back(o.group);
      }
    }
    std::sort(groups.begin(), groups.end());
    for (const auto& g : groups) {
      std::vector<size_t> members;
      bool all_hidden = true, all_locked = true;
      for (size_t i = 0; i < e.s.objects.size(); i++) {
        if (e.s.objects[i].group == g) {
          members.push_back(i);
          all_hidden &= e.s.objects[i].hidden;
          all_locked &= e.s.objects[i].locked;
        }
      }
      ImGui::PushID(g.c_str());
      if (icon_toggle("##geye", true, !all_hidden)) {
        set_all(members, true, true);
      }
      ImGui::SameLine(0.f, 2.f);
      if (icon_toggle("##glock", false, all_locked)) {
        set_all(members, false, false);
      }
      ImGui::SameLine();
      const bool open = ImGui::TreeNodeEx("##group", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen,
                                          "%s  (%d)", g.c_str(), (int)members.size());
      // (a click on its name: all of it selected; right click: ungroup)
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
          e.sel.clear();
        }
        for (size_t i : members) {
          if (!e.s.objects[i].hidden && !e.s.objects[i].locked &&
              std::find(e.sel.begin(), e.sel.end(), Pick{0, (int)i}) == e.sel.end()) {
            e.sel.push_back({0, (int)i});
          }
        }
      }
      if (ImGui::BeginPopupContextItem("##gmenu")) {
        if (ImGui::MenuItem("Ungroup")) {
          remember(e);
          for (size_t i : members) {
            e.s.objects[i].group.clear();
          }
          commit(e, false, false, false);
        }
        ImGui::EndPopup();
      }
      if (open) {
        for (size_t i : members) {
          row(i);
        }
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
    for (size_t i = 0; i < e.s.objects.size(); i++) {
      if (e.s.objects[i].group.empty()) {
        row(i);
      }
    }
    for (size_t i = 0; i < e.s.lights.size(); i++) {
      const bool sel = std::find(e.sel.begin(), e.sel.end(), Pick{1, (int)i}) != e.sel.end();
      const auto& l = e.s.lights[i];
      std::string name = fmt::format("light {}  {}##sl{}", i + 1, l.note, i);
      if (ImGui::Selectable(name.c_str(), sel)) {
        e.sel = {Pick{1, (int)i}};
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::TextDisabled("Double click: frame it");
  }
  ImGui::End();
}

// the outliner's eye (shown or hidden) or lock (locked or not), drawn, as a button: true when clicked
bool icon_toggle(const char* id, bool eye, bool on) {
  const float s = ImGui::GetTextLineHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool click = ImGui::InvisibleButton(id, ImVec2(s, s));
  const bool hover = ImGui::IsItemHovered();
  auto* dl = ImGui::GetWindowDrawList();
  const ImU32 col = ImGui::GetColorU32(hover ? grey(1.f) : on ? grey(0.85f) : grey(0.4f));
  const float h = s * 0.5f, th = std::max(1.f, s * 0.08f);
  const ImVec2 c(p.x + h, p.y + h);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
  if (eye) {
    if (on) {
      // open: its outline, the pupil
      dl->AddBezierQuadratic(P(-0.85f, 0.f), P(0.f, -0.85f), P(0.85f, 0.f), col, th);
      dl->AddBezierQuadratic(P(-0.85f, 0.f), P(0.f, 0.85f), P(0.85f, 0.f), col, th);
      dl->AddCircleFilled(c, h * 0.28f, col);
    } else {
      // shut: the lid, its lashes
      dl->AddBezierQuadratic(P(-0.85f, -0.1f), P(0.f, 0.6f), P(0.85f, -0.1f), col, th);
      for (float x : {-0.45f, 0.f, 0.45f}) {
        dl->AddLine(P(x, 0.22f), P(x * 1.3f, 0.6f), col, th);
      }
    }
    if (hover) {
      ImGui::SetTooltip(on ? "Shown (click: hide while editing)" : "Hidden while editing (click: show)");
    }
  } else {
    // a padlock: its body, the shackle shut or open
    dl->AddRectFilled(P(-0.6f, -0.05f), P(0.6f, 0.8f), col, 1.5f);
    if (on) {
      dl->AddBezierCubic(P(-0.38f, -0.05f), P(-0.38f, -0.85f), P(0.38f, -0.85f), P(0.38f, -0.05f), col, th);
    } else {
      dl->AddBezierCubic(P(-0.38f, -0.05f), P(-0.38f, -0.85f), P(0.38f, -0.85f), P(0.38f, -0.35f), col, th);
    }
    if (hover) {
      ImGui::SetTooltip(on ? "Locked: can't be selected (click: unlock)" : "Click: lock (can't be selected)");
    }
  }
  return click;
}

// a tool's icon, drawn (no icon font): s the icon's size, c its middle
void tool_icon(ImDrawList* dl, int tool, ImVec2 c, float s, ImU32 col) {
  const float h = s * 0.5f, th = std::max(1.5f, s * 0.07f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
  switch (tool) {
    case kSelect: {
      // a box drawn with dashes, an arrow on its corner
      const ImVec2 a = P(-0.8f, -0.8f), b = P(0.45f, 0.45f);
      for (int i = 0; i < 4; i++) {
        const float u0 = i / 4.f + 0.03f, u1 = (i + 0.6f) / 4.f;
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * u0, a.y), ImVec2(a.x + (b.x - a.x) * u1, a.y), col, th);
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * u0, b.y), ImVec2(a.x + (b.x - a.x) * u1, b.y), col, th);
        dl->AddLine(ImVec2(a.x, a.y + (b.y - a.y) * u0), ImVec2(a.x, a.y + (b.y - a.y) * u1), col, th);
        dl->AddLine(ImVec2(b.x, a.y + (b.y - a.y) * u0), ImVec2(b.x, a.y + (b.y - a.y) * u1), col, th);
      }
      dl->AddTriangleFilled(P(0.05f, 0.05f), P(0.95f, 0.45f), P(0.45f, 0.95f), col);
      break;
    }
    case kGrass:
      // blades
      dl->AddBezierQuadratic(P(-0.5f, 0.9f), P(-0.6f, 0.1f), P(-0.95f, -0.45f), col, th);
      dl->AddBezierQuadratic(P(-0.15f, 0.9f), P(-0.2f, -0.2f), P(-0.25f, -0.9f), col, th);
      dl->AddBezierQuadratic(P(0.15f, 0.9f), P(0.25f, -0.1f), P(0.55f, -0.75f), col, th);
      dl->AddBezierQuadratic(P(0.45f, 0.9f), P(0.6f, 0.3f), P(0.95f, -0.15f), col, th);
      dl->AddLine(P(-0.9f, 0.9f), P(0.9f, 0.9f), col, th);
      break;
    case kPlace: {
      // a cube
      const ImVec2 top = P(0.f, -0.9f), l = P(-0.8f, -0.45f), r = P(0.8f, -0.45f), m = P(0.f, 0.f),
                   bl = P(-0.8f, 0.45f), br = P(0.8f, 0.45f), b = P(0.f, 0.9f);
      const ImVec2 rim[6] = {top, r, br, b, bl, l};
      dl->AddPolyline(rim, 6, col, ImDrawFlags_Closed, th);
      dl->AddLine(l, m, col, th);
      dl->AddLine(r, m, col, th);
      dl->AddLine(m, b, col, th);
      break;
    }
    case kLight:
      // a bulb and its rays
      dl->AddCircle(P(0.f, -0.1f), h * 0.42f, col, 0, th);
      dl->AddLine(P(-0.2f, 0.45f), P(0.2f, 0.45f), col, th);
      dl->AddLine(P(-0.15f, 0.62f), P(0.15f, 0.62f), col, th);
      for (int i = 0; i < 8; i++) {
        const float a = i * kPi / 4.f + kPi / 8.f;
        if (std::sin(a) > 0.6f) {
          continue;  // (none down, where the bulb's screw is)
        }
        dl->AddLine(P(std::cos(a) * 0.62f, -0.1f + std::sin(a) * 0.62f),
                    P(std::cos(a) * 0.9f, -0.1f + std::sin(a) * 0.9f), col, th);
      }
      break;
    case kMeasure: {
      // a ruler, slanted, its marks
      const ImVec2 q[4] = {P(-0.95f, 0.35f), P(0.35f, -0.95f), P(0.95f, -0.35f), P(-0.35f, 0.95f)};
      dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, th);
      for (int i = 1; i < 6; i++) {
        const float t = i / 6.f, len = i % 2 ? 0.22f : 0.38f;
        const ImVec2 s(q[0].x + (q[1].x - q[0].x) * t, q[0].y + (q[1].y - q[0].y) * t);
        dl->AddLine(s, ImVec2(s.x + len * h * 0.7071f, s.y + len * h * 0.7071f), col, th * 0.8f);
      }
      break;
    }
    case kTexture: {
      // a picture: its frame, a mountain, the sun
      dl->AddRect(P(-0.9f, -0.75f), P(0.9f, 0.75f), col, 1.5f, 0, th);
      const ImVec2 hill[3] = {P(-0.75f, 0.6f), P(-0.15f, -0.25f), P(0.45f, 0.6f)};
      dl->AddPolyline(hill, 3, col, 0, th);
      dl->AddLine(P(0.15f, 0.2f), P(0.4f, -0.05f), col, th);
      dl->AddLine(P(0.4f, -0.05f), P(0.75f, 0.6f), col, th);
      dl->AddCircleFilled(P(0.45f, -0.4f), h * 0.14f, col);
      break;
    }
    case kNote:
      // a pencil
      dl->AddLine(P(-0.6f, 0.6f), P(0.6f, -0.6f), col, th * 3.f);
      dl->AddTriangleFilled(P(-0.6f, 0.6f), P(-0.9f, 0.9f), P(-0.45f, 0.75f), col);
      break;
    default:
      break;
  }
}

// the toolbar on the left, as Blender's (T): one button a tool, the names shown in the simple mode
void toolbar(Editor& e) {
  const float k = ui_k();
  ImGui::SetNextWindowPos(ImVec2(8.f * k, ImGui::GetFrameHeight() + 8.f * k), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * k);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f * k, 4.f * k));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.f * k, 2.f * k));
  ImGui::Begin("##tools", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
  static const char* kToolTips[kToolCount] = {
      "Click things to select them, then move, rotate or scale them", "Paint where the tall grass grows",
      "Pick a model and put it in the world (drag to scatter many)",
      "Click to place a light, then change its color, power and size",
      "Drag from one point to another: how far",
      "Click something: the pictures it's made of, to replace, edit or recolor",
      "Write and draw notes on the ground"};
  const float icon = 44.f * k;
  const float bw = e.advanced ? icon : 200.f * k;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int t = 0; t < kToolCount; t++) {
    if (t == kNote && !e.advanced) {
      continue;
    }
    const bool on = e.tool == t;
    ImGui::PushID(t);
    ImGui::PushStyleColor(ImGuiCol_Button, on ? kBlue : grey(0.157f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? kBlueHi : grey(0.29f));
    if (ImGui::Button("##tool", ImVec2(bw, icon))) {
      e.tool = (Tool)t;
    }
    ImGui::PopStyleColor(2);
    const ImVec2 a = ImGui::GetItemRectMin();
    tool_icon(dl, t, ImVec2(a.x + icon * 0.5f, a.y + icon * 0.5f), icon * 0.55f, ImGui::GetColorU32(grey(0.92f)));
    if (!e.advanced) {
      dl->AddText(ImVec2(a.x + icon + 4.f * k, a.y + (icon - ImGui::GetFontSize()) * 0.5f),
                  ImGui::GetColorU32(grey(0.92f)), kToolNames[t]);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::BeginTooltip();
      ImGui::Text("%s", kToolNames[t]);
      ImGui::SameLine();
      ImGui::TextDisabled("  %d", t + 1);
      ImGui::TextColored(kKeyText, "%s", kToolTips[t]);
      ImGui::EndTooltip();
    }
    ImGui::PopID();
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

// the top bar, as Blender's: the menus on the left, the file's state, the mode and out on the right
void topbar(Editor& e) {
  if (!ImGui::BeginMainMenuBar()) {
    return;
  }
  const float k = ui_k();
  ImGui::TextColored(kBlueHi, "Editor");
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Save", "Ctrl S", false, e.dirty)) {
      save_all(e);
    }
    if (ImGui::MenuItem("Revert", nullptr, false, e.dirty)) {
      discard_all(e);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Import Model...")) {
      e.imp.open = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit Editor", "F4")) {
      g_enhanced_menu.editor_leave = true;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Undo", "Ctrl Z", false, !e.undo.empty())) {
      undo(e);
    }
    if (ImGui::MenuItem("Redo", "Ctrl Shift Z", false, !e.redo.empty())) {
      redo(e);
    }
    ImGui::Separator();
    const bool any = !e.sel.empty();
    if (ImGui::MenuItem("Move", "G", false, any)) {
      start_modal(e, 'G');
    }
    if (ImGui::MenuItem("Rotate", "R", false, any)) {
      start_modal(e, 'R');
    }
    if (ImGui::MenuItem("Scale", "S", false, any)) {
      start_modal(e, 'S');
    }
    if (ImGui::MenuItem("Duplicate", "Shift D", false, any)) {
      duplicate_selected(e);
    }
    if (ImGui::MenuItem("Drop to Ground", "End", false, any)) {
      drop_selected(e);
    }
    ImGui::MenuItem("Snap to Surface (moving)", "Shift Tab", &e.snap_surface);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Moving (G), the objects go on whatever is under the cursor");
    }
    ImGui::MenuItem("  and Align to It", nullptr, &e.snap_align, e.snap_surface);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Stood up along the surface's slope (rocks on a hillside, signs on a wall)");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Hide Selected", "H", false, any)) {
      hide_objects(e, 1);
    }
    if (ImGui::MenuItem("Hide Unselected", "Shift H", false, any)) {
      hide_objects(e, 2);
    }
    if (ImGui::MenuItem("Show Hidden", "Alt H")) {
      hide_objects(e, 0);
    }
    if (ImGui::MenuItem("Group Selected", "Ctrl G", false, any)) {
      group_selected(e);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "X", false, any)) {
      delete_selected(e);
    }
    if (ImGui::MenuItem("Clear Rotation", "Alt R", false, any)) {
      clear_transform(e, true);
    }
    if (ImGui::MenuItem("Clear Scale", "Alt S", false, any)) {
      clear_transform(e, false);
    }
    if (ImGui::BeginMenu("Pivot Point")) {
      for (int i = 0; i < kPivotCount; i++) {
        if (ImGui::MenuItem(kPivotNames[i], nullptr, e.pivot_mode == i)) {
          e.pivot_mode = i;
        }
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Gravity for New Objects", nullptr, &e.gravity)) {
      save_settings(e);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Add")) {
    if (ImGui::MenuItem("Search and Add...", "Shift A")) {
      e.add_menu = true;
      e.add_at = e.center_ok ? e.center : cam_pos(e) + view_axis(e, 2) * (10.f * kMeter);
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Toolbar", "T", &e.show_tools);
    ImGui::MenuItem("Sidebar", "N", &e.show_panel);
    if (ImGui::MenuItem("Frame Selected", "Numpad .", false, !e.sel.empty())) {
      frame_selection(e);
    }
    ImGui::MenuItem("Terrain Textures...", nullptr, &e.show_terrain);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The ground's pictures (rock, gravel, sand): replace them, break up their repeats");
    }
    if (ImGui::MenuItem("Put Jak Here", "J")) {
      put_jak_here(e);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Where the cursor points (else the middle of the view): Jak goes there when you go "
                        "back to play");
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help")) {
    ImGui::MenuItem("Keymap", "?", &e.show_help);
    ImGui::EndMenu();
  }

  // (the right side, measured)
  const char* state = e.dirty ? "Unsaved changes" : "Saved";
  const ImGuiStyle& st = ImGui::GetStyle();
  const float seg = 120.f * k;
  const float right = ImGui::CalcTextSize(state).x + 2 * seg + ImGui::CalcTextSize("Quit  F4").x +
                      st.FramePadding.x * 2 + st.ItemSpacing.x * 4 + 30.f * k;
  ImGui::SameLine(ImGui::GetWindowWidth() - right);
  ImGui::TextColored(e.dirty ? ImVec4(1.f, 0.75f, 0.35f, 1.f) : grey(0.6f), "%s", state);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(e.dirty ? "The changes show already; Ctrl+S saves them to disk" : "All saved");
  }
  ImGui::SameLine(0.f, 18.f * k);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.f, st.ItemSpacing.y));
  bool simple = toggle_button("Simple", !e.advanced, ImVec2(seg, 0.f));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Just the essentials, tools with their names");
  }
  ImGui::SameLine();
  bool adv = toggle_button("Advanced", e.advanced, ImVec2(seg, 0.f));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Numbers, the outliner, every option, notes");
  }
  ImGui::PopStyleVar();
  if ((simple && e.advanced) || (adv && !e.advanced)) {
    e.advanced = adv;
    if (!e.advanced && e.tool == kNote) {
      e.tool = kSelect;
    }
    save_settings(e);
  }
  ImGui::SameLine(0.f, 18.f * k);
  if (ImGui::Button("Quit  F4")) {
    g_enhanced_menu.editor_leave = true;
  }
  ImGui::EndMainMenuBar();
}

// buttons by what's selected: what can be done with it, for who doesn't know the keys
void selection_buttons(Editor& e) {
  if (e.sel.empty() || e.modal || e.pad || e.advanced) {
    return;
  }
  bool objects = false;
  for (const auto& p : e.sel) {
    objects |= p.kind == 0;
  }
  V3 c = sel_center(e);
  if (e.sel.size() == 1 && e.sel[0].kind == 0) {
    const auto& o = e.s.objects[e.sel[0].idx];
    const auto* b = model_bounds(e, o.model);
    c[1] += b ? box_y(e, o, true) : 1.f * kMeter;
  }
  ImVec2 sp;
  if (!to_screen(e, c, &sp)) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  sp.x = std::max(300.f * ui_k(), std::min(sp.x, size.x - 700.f * ui_k()));
  sp.y = std::max(60.f, std::min(sp.y - 16.f, size.y - 120.f));
  ImGui::SetNextWindowPos(sp, ImGuiCond_Always, ImVec2(0.5f, 1.f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * ui_k());
  ImGui::Begin("##selbuttons", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
  auto button = [&](const char* label, const char* tip) {
    ImGui::SameLine();
    bool b = ImGui::Button(label);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tip);
    }
    return b;
  };
  ImGui::Dummy(ImVec2(0, 0));
  if (button("Move  G", "Follows the mouse, click to drop it (X Y Z: along an axis)")) {
    start_modal(e, 'G');
  }
  if (button("Rotate  R", "Turns with the mouse around the middle, click to confirm")) {
    start_modal(e, 'R');
  }
  if (button("Scale  S", "Move the mouse away from or toward the middle, click to confirm")) {
    start_modal(e, 'S');
  }
  if (button("Duplicate", "A copy that follows the mouse (Shift+D)")) {
    duplicate_selected(e);
  }
  if (objects && button("Drop", "Lets it fall to the ground with the game's gravity (End)")) {
    drop_selected(e);
  }
  ImGui::PushStyleColor(ImGuiCol_Button, kAlert);
  if (button("Delete", "Remove it (X or Del)")) {
    delete_selected(e);
  }
  ImGui::PopStyleColor();
  ImGui::End();
  ImGui::PopStyleVar();
}

// the keys, all of them (? or the top bar's button)
void help_window(Editor& e) {
  if (!e.show_help) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f * ui_k());
  ImGui::Begin("Keymap", &e.show_help, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  auto section = [](const char* title, std::initializer_list<std::pair<const char*, const char*>> rows) {
    ImGui::SeparatorText(title);
    for (const auto& r : rows) {
      key_hint(r.first, r.second);
    }
    ImGui::Spacing();
  };
  // (two columns: each section's line stays in its own)
  ImGui::BeginTable("##keymap", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX);
  ImGui::TableNextColumn();
  section("View (as Blender)", {{"Middle mouse / pen lower button", "orbit around the point"},
                               {"Shift + middle, right drag", "pan"},
                               {"Ctrl + middle, wheel", "zoom"},
                               {"Numpad .", "frame selected"}});
  section("Objects", {{"Click / pen tip", "select (Shift: extend)"},
                      {"Drag on empty space", "box select"},
                      {"A / Alt+A", "select all / none"},
                      {"G  R  S", "move, rotate, scale"},
                      {"then X Y Z", "that axis (again: local, again: free)"},
                      {"then Shift X Y Z", "the other two (that axis' plane)"},
                      {"R R", "trackball rotation"},
                      {"G R S on the way", "switch to that, keeping what's done"},
                      {"Alt+R / Alt+S", "clear rotation / scale"},
                      {".", "pivot point"},
                      {"Shift + right click", "place the 3D cursor"},
                      {"then 0-9", "the exact value (m, degrees, times)"},
                      {"then Ctrl", "snap (0.5 m, 5 degrees, 0.1)"},
                      {"Shift+D", "duplicate"},
                      {"Ctrl+C / Ctrl+V", "copy / paste where you point"},
                      {"X / Del", "delete"},
                      {"End", "drop to the ground"},
                      {"Shift+Tab", "snap to the surface while moving"},
                      {"H / Shift+H / Alt+H", "hide / hide others / show all"},
                      {"Ctrl+G", "group (the outliner)"},
                      {"J", "put Jak here"},
                      {"Shift+A", "add here"}});
  ImGui::TableNextColumn();
  section("Brushes", {{"1-5 (6 notes, advanced)", "tools"},
                      {"F or [ ]", "brush radius"},
                      {"Ctrl held", "erase with the brush"}});
  section("File", {{"Ctrl+Z / Ctrl+Shift+Z", "undo / redo"},
                   {"Ctrl+S", "save"},
                   {"F4 / Start", "quit (asks to save)"},
                   {"N / T", "sidebar / toolbar"}});
  section("Controller", {{"Sticks, L1/R1, R2", "fly, look, down/up, slow"},
                         {"Cross (A)", "use the tool (held: paint)"},
                         {"Square", "erase / delete"},
                         {"Triangle", "next tool"},
                         {"D-pad", "model, radius, rotate, scale"},
                         {"L2 held", "move the selection"},
                         {"L3", "gravity / drop"},
                         {"Select / R3", "undo / redo"}});
  ImGui::EndTable();
  ImGui::End();
}

// leaving with changes not saved: save them, drop them or stay
void leave_popup(Editor& e) {
  auto leave = [&]() {
    g_enhanced_menu.editor = false;
    g_enhanced_menu.photo = false;
    e.ask_leave = false;
  };
  if (g_enhanced_menu.editor_leave.exchange(false)) {
    if (e.modal) {
      end_modal(e, false);
    }
    if (!e.dirty) {
      leave();
      return;
    }
    e.ask_leave = true;
    ImGui::OpenPopup("Save changes?");
  }
  if (!e.ask_leave) {
    return;
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal("Save changes?", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    e.ask_leave = false;
    return;
  }
  ImGui::Text("You have unsaved changes.");
  ImGui::Spacing();
  const bool pad_save = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
  const bool pad_drop = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false);
  const bool pad_stay = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
  ImGui::PushStyleColor(ImGuiCol_Button, kBlue);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBlueHi);
  if (ImGui::Button("Save and Quit  (Enter)", ImVec2(220.f * ui_k(), 36.f * ui_k())) || pad_save ||
      ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
    save_all(e);
    leave();
    ImGui::CloseCurrentPopup();
  }
  ImGui::PopStyleColor(2);
  ImGui::SameLine();
  if (ImGui::Button("Quit Without Saving  (Square)", ImVec2(240.f * ui_k(), 36.f * ui_k())) || pad_drop) {
    discard_all(e);
    leave();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Stay  (Esc / Circle)", ImVec2(180.f * ui_k(), 36.f * ui_k())) || pad_stay ||
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    e.ask_leave = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

float status_height() {
  return ImGui::GetFrameHeight() + 6.f * ui_k();
}

// the status bar at the bottom, as Blender's: what the keys do now, as keycaps; counts on the right
void hint_bar(const Editor& e) {
  using Hints = std::vector<std::pair<const char*, const char*>>;
  Hints hints;
  std::string mode;
  if (e.modal) {
    static const char* kAxes[3] = {"X", "Z", "Y"};
    mode = e.modal == 'G' ? "Move" : e.modal == 'R' ? "Rotate" : "Scale";
    if (e.trackball) {
      mode += "  trackball";
    } else if (e.axis >= 0) {
      mode += std::string(e.axis_plane ? "  across " : e.modal == 'R' ? "  about " : "  along ") +
              (e.axis_local ? "local " : "global ") + kAxes[e.axis];
    }
    mode += std::string("  (") + kPivotNames[e.pivot_mode] + ")";
    if (!e.typed.empty()) {
      mode += "  " + e.typed + (e.modal == 'G' ? " m" : e.modal == 'R' ? " degrees" : "x");
    }
    hints = {{"Click", "confirm"}, {"Right click", "cancel"}, {"0-9", "value"}, {"Ctrl", "snap"}};
    if (e.modal == 'G') {
      hints.insert(hints.begin() + 2, {"X Y Z", "axis"});
    }
  } else if (e.resizing) {
    mode = "Radius";
    hints = {{"Mouse", "grow / shrink"}, {"Click", "confirm"}, {"Right click", "cancel"}};
  } else if (e.pad) {
    switch (e.tool) {
      case kSelect:
      case kLight:
        if (e.sel.empty()) {
          hints = {{"Cross", e.tool == kLight ? "place / select" : "select"}, {"Triangle", "tool"}};
        } else if (e.sel[0].kind == 1) {
          hints = {{"Cross", "select"},   {"L2 held", "move"},     {"D-pad left/right", "radius"},
                   {"Up/down", "power"}, {"Square", "delete"}, {"Circle", "deselect"}};
        } else {
          hints = {{"Cross", "select"},   {"L2 held", "move"}, {"D-pad left/right", "rotate"}, {"Up/down", "scale"},
                   {"Square", "delete"}, {"L3", "drop"},      {"Circle", "deselect"}};
        }
        break;
      case kGrass:
        hints = {{"Cross held", "paint"},
                 {"Square held", "erase"},
                 {"Up/down", "radius"},
                 {"Triangle", "tool"}};
        break;
      case kPlace:
        hints = {{"Cross", "place (held: scatter)"},
                 {"Square held", "remove"},
                 {"D-pad left/right", "model"},
                 {"Up/down", "radius"},
                 {"L3", "gravity"},
                 {"Triangle", "tool"}};
        break;
      default:
        break;
    }
    hints.push_back({"Sticks, L1/R1", "fly"});
    hints.push_back({"Select", "undo"});
    hints.push_back({"Start", "quit"});
  } else {
    switch (e.tool) {
      case kSelect:
        hints = {{"Click", "select"}, {"Drag", "box select"}, {"G", "move"},  {"R", "rotate"},
                 {"S", "scale"},     {"X", "delete"},       {"Shift A", "add"}};
        break;
      case kGrass:
        hints = {{"Click", "paint"}, {"Ctrl", "erase"}, {"F", "radius"}};
        break;
      case kPlace:
        hints = {{"Click", "place"}, {"Drag", "scatter"}, {"Ctrl", "remove"}, {"F", "radius"}};
        break;
      case kLight:
        hints = {{"Click", "new light / select"}, {"G", "move"}, {"S", "radius"}, {"X", "delete"}};
        break;
      case kMeasure:
        hints = {{"Drag", "measure"}};
        break;
      case kTexture:
        hints = {{"Click", "its textures"}};
        break;
      default:
        hints = {{"Click", "draw"}};
        break;
    }
    hints.push_back({"Middle", "orbit"});
    hints.push_back({"Right drag", "pan"});
    hints.push_back({"Wheel", "zoom"});
  }
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float h = status_height();
  ImGui::SetNextWindowPos(ImVec2(0.f, size.y - h), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(size.x, h), ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, grey(0.137f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * ui_k(), 3.f * ui_k()));
  ImGui::Begin("##hint", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
  const float y = ImGui::GetCursorPosY();
  if (!mode.empty()) {
    // (what's going on, as Blender's header text during a move)
    ImGui::TextColored(kBlueHi, "%s", mode.c_str());
    ImGui::SameLine(0.f, 24.f * ui_k());
  }
  for (const auto& [key, what] : hints) {
    ImGui::SetCursorPosY(y);
    key_hint(key, what);
    ImGui::SameLine(0.f, 20.f * ui_k());
  }
  const std::string counts = fmt::format("{} objects   {} lights", e.s.objects.size(), e.s.lights.size());
  ImGui::SetCursorPos(ImVec2(size.x - ImGui::CalcTextSize(counts.c_str()).x - 14.f * ui_k(), y + ui_k()));
  ImGui::TextDisabled("%s", counts.c_str());
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

// Shift+A: add a model or a light where the cursor is
void add_menu(Editor& e) {
  if (e.add_menu) {
    ImGui::OpenPopup("Add##add");
    e.add_menu = false;
  }
  if (!ImGui::BeginPopup("Add##add")) {
    return;
  }
  static char find[64] = {};
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
    find[0] = 0;
  }
  ImGui::InputTextWithHint("##find", "Search...", find, sizeof(find));
  const V3 at = e.add_at;
  if (ImGui::Selectable("Light")) {
    remember(e);
    EnhancedLight l;
    l.night = false;
    l.pos[0] = at[0];
    l.pos[1] = at[1] + 1.2f * kMeter;
    l.pos[2] = at[2];
    e.s.lights.push_back(l);
    e.sel = {Pick{1, (int)e.s.lights.size() - 1}};
    commit(e, false, true, false);
  }
  ImGui::Separator();
  ImGui::BeginChild("##addlist", ImVec2(320.f * ui_k(), 300.f * ui_k()));
  for (size_t k = 0; k < kKinds.size(); k++) {
    bool header = false;
    for (size_t i = 0; i < e.models.size(); i++) {
      const auto& m = e.models[i];
      if (kind_of(m) != (int)k || !matches(m, find)) {
        continue;
      }
      if (!header) {
        ImGui::TextDisabled("%s / %s", kCats[kKinds[k].cat], kKinds[k].name);
        header = true;
      }
      std::string label = "  " + pretty(m) + "##" + m;
      if (ImGui::Selectable(label.c_str())) {
        remember(e);
        e.model = (int)i;
        EnhancedPlaced p;
        p.model = m;
        for (int c = 0; c < 3; c++) {
          p.pos[c] = at[c];
        }
        if (e.gravity) {
          p.pos[1] += e.drop_height * kMeter;
        }
        e.s.objects.push_back(p);
        e.sel = {Pick{0, (int)e.s.objects.size() - 1}};
        if (e.gravity) {
          drop(e, e.s.objects.size() - 1);
        }
        commit(e, true, false, false);
        ImGui::CloseCurrentPopup();
      }
    }
  }
  ImGui::EndChild();
  ImGui::EndPopup();
}

void frame_selection(Editor& e) {
  if (e.sel.empty()) {
    return;
  }
  V3 c = sel_center(e);
  float r = 2.f * kMeter;
  for (const auto& p : e.sel) {
    if (p.kind == 0) {
      V3 oc;
      r = std::max(r, object_radius(e, e.s.objects[p.idx], &oc));
      c = p == e.sel[0] && e.sel.size() == 1 ? oc : c;
    }
  }
  const V3 fwd = view_axis(e, 2);
  V3 want = c - fwd * (r * 3.f);
  add_move(want - cam_pos(e));
  e.pivot = c;
}

// ---- the pad (on the free camera: the sticks move and look, L1 / R1 down and up, R2 slower)
bool pad_pressed(ImGuiKey k, bool repeat = false) {
  return ImGui::IsKeyPressed(k, repeat);
}

// true: the frame's press was used (a move confirmed or dropped)
bool pad_input(Editor& e) {
  auto& io = ImGui::GetIO();
  // (the pad's in use from its first touch, the mouse's from its first move)
  for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; k++) {
    if (ImGui::IsKeyDown((ImGuiKey)k) && k != ImGuiKey_GamepadStart) {
      e.pad = true;
    }
  }
  if (std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y) > 2.f || io.MouseWheel != 0.f) {
    e.pad = false;
  }
  if (!e.pad) {
    return false;
  }
  // (the reticle points: the tools see it as the mouse; the buttons as the left button)
  const bool a_press = pad_pressed(ImGuiKey_GamepadFaceDown);
  const bool a_down = ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
  const bool sq_down = ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft);
  if (e.modal) {
    if (e.pad_grab && !ImGui::IsKeyDown(ImGuiKey_GamepadL2)) {
      e.pad_grab = false;
      end_modal(e, true);
      return true;
    } else if (pad_pressed(ImGuiKey_GamepadFaceRight)) {
      e.pad_grab = false;
      end_modal(e, false);
      return true;
    } else if (a_press) {
      e.pad_grab = false;
      end_modal(e, true);
      return true;
    }
    return false;
  }
  if (pad_pressed(ImGuiKey_GamepadFaceUp)) {
    // (the tools in turn: the notes are the keyboard's)
    e.tool = (Tool)((e.tool + 1) % kNote);
  }
  if (pad_pressed(ImGuiKey_GamepadBack)) {
    undo(e);
  }
  if (pad_pressed(ImGuiKey_GamepadR3)) {
    redo(e);
  }
  const bool left = pad_pressed(ImGuiKey_GamepadDpadLeft, true), right = pad_pressed(ImGuiKey_GamepadDpadRight, true);
  const bool up = pad_pressed(ImGuiKey_GamepadDpadUp, true), down = pad_pressed(ImGuiKey_GamepadDpadDown, true);
  float* brush = e.tool == kGrass ? &e.grass_brush : e.tool == kPlace ? &e.place_brush : nullptr;
  if (brush && (up || down)) {
    *brush = std::clamp(*brush * (up ? 1.15f : 1.f / 1.15f), 0.3f, 60.f);
  }
  switch (e.tool) {
    case kGrass:
      if (left || right) {
        e.grass_mode = 1 - e.grass_mode;
      }
      break;
    case kPlace:
      if ((left || right) && !e.models.empty()) {
        // (the next model of the shown kind, or any while searching)
        const int n = (int)e.models.size();
        int m = e.model < 0 ? 0 : e.model;
        for (int k = 0; k < n; k++) {
          m = (m + (right ? 1 : n - 1)) % n;
          if (in_browser(e, e.models[m])) {
            break;
          }
        }
        e.model = m;
      }
      if (pad_pressed(ImGuiKey_GamepadL3)) {
        e.gravity = !e.gravity;
        save_settings(e);
      }
      break;
    case kSelect:
    case kLight:
      if (!e.sel.empty()) {
        if (left || right || up || down) {
          remember(e);
          for (const auto& p : e.sel) {
            if (p.kind == 0) {
              auto& o = e.s.objects[p.idx];
              set_rot(o, qmul(qaxis({0, 1, 0}, (left ? 1.f : right ? -1.f : 0.f) * kPi / 12.f), rot_of(o)));
              for (auto& s : o.scale) {
                s = std::clamp(s * (up ? 1.1f : down ? 1.f / 1.1f : 1.f), 0.02f, 100.f);
              }
            } else {
              auto& l = e.s.lights[p.idx];
              l.radius = std::clamp(l.radius * (right ? 1.15f : left ? 1.f / 1.15f : 1.f), 0.3f * kMeter,
                                    60.f * kMeter);
              l.intensity = std::clamp(l.intensity * (up ? 1.15f : down ? 1.f / 1.15f : 1.f), 0.f, 10.f);
            }
          }
          commit(e, true, true, false);
        }
        if (pad_pressed(ImGuiKey_GamepadL2)) {
          start_modal(e, 'G');
          e.pad_grab = e.modal != 0;
        }
        if (pad_pressed(ImGuiKey_GamepadFaceLeft)) {
          delete_selected(e);
        }
        if (pad_pressed(ImGuiKey_GamepadL3)) {
          drop_selected(e);
        }
        if (pad_pressed(ImGuiKey_GamepadFaceRight)) {
          e.sel.clear();
        }
      }
      break;
    default:
      break;
  }
  (void)a_down;
  (void)sq_down;
  return false;
}

// the pad's buttons as the mouse's left one (with Ctrl: Square erases)
bool pad_tool_press(const Editor& e) {
  return e.pad && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                   (e.tool != kSelect && e.tool != kLight && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)));
}
bool pad_tool_down(const Editor& e) {
  return e.pad && (ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) ||
                   (e.tool != kSelect && e.tool != kLight && ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft)));
}

// ---- the frame's input
void handle_input(Editor& e) {
  auto& io = ImGui::GetIO();
  if (pad_input(e)) {
    return;
  }
  const bool over_ui = io.WantCaptureMouse && !e.pad;
  const bool typing = io.WantTextInput;
  const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;

  // the view: the middle button (the pen's lower one) and the wheel, whatever the tool
  // (the right button dragged: the view slides, as Shift + the middle one; a right click still
  // cancels a move)
  const bool right = !e.modal && !e.resizing && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
  if (!over_ui && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || right)) {
    e.nav = right || shift ? 2 : ctrl ? 3 : 1;
    e.nav_button = right ? ImGuiMouseButton_Right : ImGuiMouseButton_Middle;
    // (about the point under the cursor, else what's in the middle, else ahead)
    e.pivot = e.hover_ok ? e.hover
              : e.center_ok ? e.center
                            : cam_pos(e) + view_axis(e, 2) * (20.f * kMeter);
  }
  if (right) {
    e.right_from = io.MousePos;
  }
  if (e.nav && !ImGui::IsMouseDown(e.nav_button)) {
    // (Shift + a right click, not a drag: Blender's 3D cursor there)
    if (e.nav_button == ImGuiMouseButton_Right && shift && e.hover_ok &&
        std::hypot(io.MousePos.x - e.right_from.x, io.MousePos.y - e.right_from.y) < 4.f) {
      e.cursor3d = e.hover;
      e.have_cursor = true;
    }
    e.nav = 0;
  }
  if (e.nav && (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f)) {
    navigate(e, io.MouseDelta);
  }
  if (!over_ui && io.MouseWheel != 0.f && !e.resizing) {
    zoom_wheel(e, io.MouseWheel);
  }

  // a modal move going on: only its keys
  if (e.modal) {
    update_modal(e);
    // X / Y / Z: along that axis (again: the object's own; again: free), Shift: across it (its
    // plane); Blender's Z the game's up. G / R / S: another move from here, R again a trackball
    {
      const ImGuiKey keys[3] = {ImGuiKey_X, ImGuiKey_Z, ImGuiKey_Y};
      for (int a = 0; a < 3; a++) {
        if (ImGui::IsKeyPressed(keys[a], false)) {
          if (e.axis == a && e.axis_plane == shift) {
            if (!e.axis_local) {
              e.axis_local = true;
            } else {
              e.axis = -1;
              e.axis_local = false;
            }
          } else {
            e.axis = a;
            e.axis_plane = shift;
            e.axis_local = false;
          }
          e.trackball = false;
        }
      }
      for (int kind : {'G', 'R', 'S'}) {
        if (ImGui::IsKeyPressed(kind == 'G' ? ImGuiKey_G : kind == 'R' ? ImGuiKey_R : ImGuiKey_S, false)) {
          if (kind == 'R' && e.modal == 'R') {
            e.trackball = !e.trackball;
            e.axis = -1;
            e.axis_local = e.axis_plane = false;
          } else if (kind != e.modal) {
            switch_modal(e, kind);
          }
        }
      }
    }
    for (int d = 0; d < 10; d++) {
      if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + d), false) ||
          ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + d), false)) {
        e.typed += (char)('0' + d);
      }
    }
    if ((ImGui::IsKeyPressed(ImGuiKey_Period, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false)) &&
        e.typed.find('.') == std::string::npos) {
      e.typed += '.';
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) {
      e.typed = e.typed.rfind('-', 0) == 0 ? e.typed.substr(1) : "-" + e.typed;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !e.typed.empty()) {
      e.typed.pop_back();
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      end_modal(e, true);
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      end_modal(e, false);
    }
    return;
  }

  // F: the brush's size follows the mouse (as Blender's)
  float* brush = e.tool == kGrass ? &e.grass_brush : e.tool == kPlace ? &e.place_brush : nullptr;
  if (e.resizing) {
    float d = std::hypot(io.MousePos.x - e.resize_mouse.x, io.MousePos.y - e.resize_mouse.y);
    float depth = e.hover_ok ? std::max(dot(e.hover - cam_pos(e), view_axis(e, 2)), 1.f * kMeter)
                             : 20.f * kMeter;
    if (brush) {
      *brush = std::clamp(d * per_pixel(e, depth) / kMeter, 0.3f, 60.f);
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
      e.resizing = false;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      if (brush) {
        *brush = e.resize_from;
      }
      e.resizing = false;
    }
    return;
  }

  if (!typing) {
    // the tools
    for (int t = 0; t < kToolCount; t++) {
      if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + t), false) && e.tool != kNote &&
          (t != kNote || e.advanced)) {
        e.tool = (Tool)t;
      }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
      if (shift) {
        redo(e);
      } else {
        undo(e);
      }
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
      redo(e);
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
      save_all(e);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
      drop_selected(e);
    }
    if (e.tool != kNote) {
      if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        e.show_panel = !e.show_panel;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        e.show_tools = !e.show_tools;
      }
      if (brush && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        e.resizing = true;
        e.resize_from = *brush;
        float depth = e.hover_ok ? std::max(dot(e.hover - cam_pos(e), view_axis(e, 2)), 1.f * kMeter)
                                 : 20.f * kMeter;
        float px = *brush * kMeter / per_pixel(e, depth);
        e.resize_mouse = ImVec2(io.MousePos.x - px, io.MousePos.y);
      }
      if (brush && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
        *brush = std::max(0.3f, *brush / 1.15f);
      }
      if (brush && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
        *brush = std::min(60.f, *brush * 1.15f);
      }
      if (shift && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        e.add_menu = true;
        e.add_at = e.hover_ok    ? e.hover
                   : e.center_ok ? e.center
                                 : cam_pos(e) + view_axis(e, 2) * (10.f * kMeter);
      } else if (alt && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        e.sel.clear();
      } else if (ImGui::IsKeyPressed(ImGuiKey_A, false) && !ctrl) {
        e.sel.clear();
        for (size_t i = 0; i < e.s.objects.size(); i++) {
          if (!e.s.objects[i].hidden && !e.s.objects[i].locked) {
            e.sel.push_back({0, (int)i});
          }
        }
        for (size_t i = 0; i < e.s.lights.size(); i++) {
          e.sel.push_back({1, (int)i});
        }
      }
      // (as Blender's: the keys act where the mouse is, not over a panel)
      if (!ctrl && !alt && !over_ui && (e.tool == kSelect || e.tool == kLight)) {
        if (shift && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
          duplicate_selected(e);
        } else if (!shift && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
          start_modal(e, 'G');
        } else if (!shift && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
          start_modal(e, 'R');
        } else if (!shift && !ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
          start_modal(e, 'S');
        }
      }
      if (!over_ui && !ctrl && (ImGui::IsKeyPressed(ImGuiKey_X, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
        delete_selected(e);
      }
      // H: hide the selected, Shift H: the others, Alt H: show them all (Blender's)
      if (!over_ui && !ctrl && ImGui::IsKeyPressed(ImGuiKey_H, false)) {
        hide_objects(e, alt ? 0 : shift ? 2 : 1);
      }
      if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        group_selected(e);
      }
      if (shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        e.snap_surface = !e.snap_surface;
        save_settings(e);
      }
      if (!over_ui && !ctrl && !alt && !shift && ImGui::IsKeyPressed(ImGuiKey_J, false)) {
        put_jak_here(e);
      }
      if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        // (the objects around their middle; on the system's clipboard too, for another session)
        e.clip.clear();
        V3 c = {0, 0, 0};
        int n = 0;
        for (const auto& p : e.sel) {
          if (p.kind == 0) {
            e.clip.push_back(e.s.objects[p.idx]);
            c = c + V3{e.s.objects[p.idx].pos[0], e.s.objects[p.idx].pos[1], e.s.objects[p.idx].pos[2]};
            n++;
          }
        }
        for (auto& o : e.clip) {
          for (int k = 0; k < 3; k++) {
            o.pos[k] -= c[k] / std::max(n, 1);
          }
        }
        if (!e.clip.empty()) {
          nlohmann::json j = {{"jak_enhanced_objects", nlohmann::json::array()}};
          for (const auto& o : e.clip) {
            j["jak_enhanced_objects"].push_back(placed_json(o));
          }
          ImGui::SetClipboardText(j.dump().c_str());
        }
      }
      if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        // (copied in another session: from the system's clipboard)
        if (const char* text = ImGui::GetClipboardText(); text && std::strstr(text, "jak_enhanced_objects")) {
          try {
            const auto j = nlohmann::json::parse(text);
            std::vector<EnhancedPlaced> got;
            for (const auto& o : j.at("jak_enhanced_objects")) {
              got.push_back(placed_from_json(o));
            }
            if (!got.empty()) {
              e.clip = got;
            }
          } catch (const std::exception& ex) {
            lg::warn("[enhanced] editor: the clipboard's objects: {}", ex.what());
          }
        }
      }
      if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && !e.clip.empty() && e.hover_ok) {
        // (at the cursor; with gravity they fall)
        remember(e);
        e.sel.clear();
        for (auto o : e.clip) {
          for (int k = 0; k < 3; k++) {
            o.pos[k] += e.hover[k];
          }
          if (e.gravity) {
            o.pos[1] += e.drop_height * kMeter;
          }
          e.s.objects.push_back(o);
          e.sel.push_back({0, (int)e.s.objects.size() - 1});
          if (e.gravity) {
            drop(e, e.s.objects.size() - 1);
          }
        }
        commit(e, true, false, false);
      }
      // ?: the keys (the key by its place: the US layout's /, the Italian's ' where the US has -)
      if (shift && (ImGui::IsKeyPressed(ImGuiKey_Slash, false) || ImGui::IsKeyPressed(ImGuiKey_Minus, false))) {
        e.show_help = !e.show_help;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false)) {
        frame_selection(e);
      }
      if (!over_ui && !ctrl && !alt && ImGui::IsKeyPressed(ImGuiKey_Period, false)) {
        e.pivot_popup = true;
      }
      if (alt && !over_ui && (ImGui::IsKeyPressed(ImGuiKey_R, false) || ImGui::IsKeyPressed(ImGuiKey_S, false))) {
        clear_transform(e, ImGui::IsKeyPressed(ImGuiKey_R, false));
      }
    }
  }
  if (e.modal) {
    return;
  }

  // a box being drawn: let go, what's in it is selected (Shift: added)
  if (e.boxing) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !(e.pad && ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown))) {
      e.boxing = false;
      const ImVec2 b = pointer(e);
      const ImVec2 lo(std::min(b.x, e.box_from.x), std::min(b.y, e.box_from.y));
      const ImVec2 hi(std::max(b.x, e.box_from.x), std::max(b.y, e.box_from.y));
      if (!shift) {
        e.sel.clear();
      }
      if (hi.x - lo.x > 6.f || hi.y - lo.y > 6.f) {
        auto inside = [&](const V3& p) {
          ImVec2 sp;
          return to_screen(e, p, &sp) && sp.x >= lo.x && sp.x <= hi.x && sp.y >= lo.y && sp.y <= hi.y;
        };
        for (size_t i = 0; i < e.s.objects.size(); i++) {
          if (e.s.objects[i].hidden || e.s.objects[i].locked) {
            continue;
          }
          V3 c;
          object_radius(e, e.s.objects[i], &c);
          if (inside(c) && std::find(e.sel.begin(), e.sel.end(), Pick{0, (int)i}) == e.sel.end()) {
            e.sel.push_back({0, (int)i});
          }
        }
        for (size_t i = 0; i < e.s.lights.size(); i++) {
          const auto& l = e.s.lights[i];
          if (inside(V3{l.pos[0], l.pos[1], l.pos[2]}) &&
              std::find(e.sel.begin(), e.sel.end(), Pick{1, (int)i}) == e.sel.end()) {
            e.sel.push_back({1, (int)i});
          }
        }
      }
    }
    return;
  }

  // the left button (the pen's tip): the tool
  const bool press = (!over_ui && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || pad_tool_press(e);
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left) || pad_tool_down(e);
  const ImVec2 at_px = pointer(e);
  // (with the pad, Square is the brushes' Ctrl: it erases)
  const bool erase = ctrl || (e.pad && ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
  switch (e.tool) {
    case kSelect:
    case kLight:
      if (press) {
        Pick p;
        bool hit = pick_at(e, at_px, &p, e.tool == kLight);
        if (hit) {
          auto it = std::find(e.sel.begin(), e.sel.end(), p);
          if (shift) {
            if (it != e.sel.end()) {
              e.sel.erase(it);
            } else {
              e.sel.push_back(p);
            }
          } else {
            e.sel = {p};
          }
        } else if (e.tool == kSelect) {
          // (empty ground: a click deselects, a drag draws a box)
          e.boxing = true;
          e.box_from = at_px;
        } else if (e.tool == kLight && e.hover_ok) {
          remember(e);
          EnhancedLight l;
          l.night = false;
          l.pos[0] = e.hover[0];
          l.pos[1] = e.hover[1] + 1.2f * kMeter;
          l.pos[2] = e.hover[2];
          e.s.lights.push_back(l);
          e.sel = {Pick{1, (int)e.s.lights.size() - 1}};
          commit(e, false, true, false);
        } else if (!shift) {
          e.sel.clear();
        }
      }
      break;
    case kTexture:
      if (press) {
        texture_pick(e);
      }
      break;
    case kMeasure:
      // (from where it's pressed to where it's let go)
      if (press && e.hover_ok) {
        e.measuring = true;
        e.have_measure = false;
        e.measure_a = e.measure_b = e.hover;
      }
      if (e.measuring && down && e.hover_ok) {
        e.measure_b = e.hover;
      }
      if (e.measuring && !down) {
        e.measuring = false;
        e.have_measure = length(e.measure_b - e.measure_a) > 0.01f * kMeter;
      }
      break;
    case kGrass:
    case kPlace: {
      if (press && e.hover_ok && (e.tool == kGrass || e.model >= 0 || erase || e.place_mode == 1)) {
        remember(e);
        e.painting = true;
        e.paint_erase = erase;
        e.have_dab = false;
      }
      if (e.painting && down && e.hover_ok) {
        const float r = (e.tool == kGrass ? e.grass_brush : e.place_brush) * kMeter;
        const float spacing = e.tool == kGrass || e.place_mode == 1 ? r * 0.35f : r * 0.9f;
        if (!e.have_dab || length(e.hover - e.last_dab) > spacing) {
          if (e.tool == kGrass) {
            // (no field here yet: one made around it, baked in a moment)
            if (!e.paint_erase &&
                !enhanced_grass_field_at(e.hover[0] / kMeter, e.hover[2] / kMeter, e.grass_brush + 2.f)) {
              enhanced_grass_add_zone(e.hover[0] / kMeter, e.hover[1] / kMeter, e.hover[2] / kMeter);
            }
            grass_dab(e, e.hover);
          } else {
            place_dab(e, e.hover, !e.have_dab);
            push_objects(e);
          }
          e.last_dab = e.hover;
          e.have_dab = true;
        }
      }
      if (e.painting && !down) {
        e.painting = false;
        commit(e, e.tool == kPlace, false, e.tool == kGrass);
      }
      break;
    }
    default:
      break;
  }
}

// the period's menu: the pivot point (Blender's pie, as a list)
void pivot_menu(Editor& e) {
  if (e.pivot_popup) {
    ImGui::OpenPopup("##pivot_menu");
    e.pivot_popup = false;
  }
  if (ImGui::BeginPopup("##pivot_menu")) {
    ImGui::TextDisabled("Pivot Point");
    for (int i = 0; i < kPivotCount; i++) {
      if (ImGui::Selectable(kPivotNames[i], e.pivot_mode == i)) {
        e.pivot_mode = i;
      }
    }
    ImGui::EndPopup();
  }
}

// the marks over the picture: the brush, what's selected, the lights, the painted grass
void draw_marks(Editor& e) {
  auto* dl = ImGui::GetBackgroundDrawList();
  const auto& io = ImGui::GetIO();
  // what the Textures tool picked
  if (e.tool == kTexture && g_tex.have) {
    ImVec2 p;
    if (to_screen(e, g_tex.at, &p)) {
      dl->AddCircle(p, 9.f, IM_COL32(0, 0, 0, 180), 0, 4.f);
      dl->AddCircle(p, 9.f, IM_COL32(120, 200, 255, 255), 0, 2.f);
    }
  }
  // the measuring tape: the line, its ends, how long
  if ((e.measuring || e.have_measure) && e.tool == kMeasure) {
    ImVec2 a, b;
    if (to_screen(e, e.measure_a, &a) && to_screen(e, e.measure_b, &b)) {
      const ImU32 col = IM_COL32(255, 210, 60, 240);
      dl->AddLine(a, b, IM_COL32(0, 0, 0, 160), 4.f);
      dl->AddLine(a, b, col, 2.f);
      dl->AddCircleFilled(a, 4.f, col);
      dl->AddCircleFilled(b, 4.f, col);
      // (and straight up from the lower end: the rise)
      const V3 lo = e.measure_a[1] < e.measure_b[1] ? e.measure_a : e.measure_b;
      const V3 hi = e.measure_a[1] < e.measure_b[1] ? e.measure_b : e.measure_a;
      ImVec2 c;
      if (std::abs(hi[1] - lo[1]) > 0.1f * kMeter && to_screen(e, V3{lo[0], hi[1], lo[2]}, &c)) {
        ImVec2 l;
        if (to_screen(e, lo, &l)) {
          dl->AddLine(l, c, IM_COL32(255, 210, 60, 120), 1.f);
          dl->AddLine(c, hi == e.measure_a ? a : b, IM_COL32(255, 210, 60, 120), 1.f);
        }
      }
      const V3 d = (e.measure_b - e.measure_a) * (1.f / kMeter);
      const std::string label = fmt::format("{:.2f} m  ({:+.2f} m high)", length(d), d[1]);
      const ImVec2 mid((a.x + b.x) * 0.5f + 10.f, (a.y + b.y) * 0.5f - 22.f);
      const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
      dl->AddRectFilled(ImVec2(mid.x - 5.f, mid.y - 3.f), ImVec2(mid.x + ts.x + 5.f, mid.y + ts.y + 3.f),
                        IM_COL32(20, 20, 20, 200), 4.f);
      dl->AddText(mid, IM_COL32(255, 225, 120, 255), label.c_str());
    }
  }
  // where Jak goes
  if (e.have_jak_at) {
    ImVec2 p;
    if (to_screen(e, e.jak_at, &p)) {
      dl->AddCircle(p, 9.f, IM_COL32(255, 150, 40, 230), 0, 2.f);
      dl->AddText(ImVec2(p.x + 12.f, p.y - 8.f), IM_COL32(255, 190, 90, 255), "Jak goes here (F4)");
    }
  }
  // the axis a move is held to, through the pivot, in its color (Blender's: X red, Y green, Z blue)
  if (e.modal && e.axis >= 0 && !e.trackball) {
    static const ImU32 kAxisCol[3] = {IM_COL32(240, 70, 80, 220), IM_COL32(80, 140, 250, 220),
                                      IM_COL32(130, 210, 60, 220)};
    const V3 c = e.modal_center, a = constraint_axis(e);
    auto line = [&](const V3& k) {
      ImVec2 p0, p1;
      if (to_screen(e, c - k * (500.f * kMeter), &p0) && to_screen(e, c + k * (500.f * kMeter), &p1)) {
        dl->AddLine(p0, p1, kAxisCol[e.axis], 1.5f);
      } else if (to_screen(e, c, &p0) && to_screen(e, c + k * (20.f * kMeter), &p1)) {
        dl->AddLine(p0, p1, kAxisCol[e.axis], 1.5f);
      }
    };
    if (e.axis_plane) {
      // (across it: the other two)
      const V3 o1 = std::abs(a[1]) < 0.9f ? normalize(cross(a, V3{0, 1, 0})) : normalize(cross(a, V3{1, 0, 0}));
      line(o1);
      line(cross(a, o1));
    } else {
      line(a);
    }
  }
  // Blender's 3D cursor: a red and white ring, its cross
  ImVec2 cp;
  if (e.have_cursor && to_screen(e, e.cursor3d, &cp)) {
    const float r = 10.f * ui_k();
    for (int i = 0; i < 8; i++) {
      dl->PathArcTo(cp, r, i * kPi / 4.f, (i + 1) * kPi / 4.f, 4);
      dl->PathStroke(i % 2 ? IM_COL32(255, 255, 255, 230) : IM_COL32(230, 40, 40, 230), 0, 2.f);
    }
    dl->AddLine(ImVec2(cp.x - 1.8f * r, cp.y), ImVec2(cp.x - 0.5f * r, cp.y), IM_COL32(0, 0, 0, 200), 1.5f);
    dl->AddLine(ImVec2(cp.x + 0.5f * r, cp.y), ImVec2(cp.x + 1.8f * r, cp.y), IM_COL32(0, 0, 0, 200), 1.5f);
    dl->AddLine(ImVec2(cp.x, cp.y - 1.8f * r), ImVec2(cp.x, cp.y - 0.5f * r), IM_COL32(0, 0, 0, 200), 1.5f);
    dl->AddLine(ImVec2(cp.x, cp.y + 0.5f * r), ImVec2(cp.x, cp.y + 1.8f * r), IM_COL32(0, 0, 0, 200), 1.5f);
  }
  if (e.pad) {
    const ImVec2 c = pointer(e);
    dl->AddCircle(c, 9.f, IM_COL32(0, 0, 0, 160), 20, 3.f);
    dl->AddCircle(c, 9.f, IM_COL32(255, 255, 255, 230), 20, 1.5f);
    dl->AddCircleFilled(c, 2.f, IM_COL32(255, 255, 255, 230));
  }
  // the painted grass (its tool only, while not painting): faint rings where it was painted
  if (e.tool == kGrass && !e.painting) {
    auto disc = [&](const V3& p, ImU32 col) {
      V3 w = {p[0] * kMeter, e.hover_ok ? e.hover[1] : e.cam.trans[1] - 2.f * kMeter, p[1] * kMeter};
      ImVec2 sp;
      float depth;
      // (at the cursor's level: the painting has no height of its own)
      if (to_screen(e, w, &sp, &depth) && depth < 400.f * kMeter) {
        dl->AddCircle(sp, std::max(2.f, p[2] * kMeter / per_pixel(e, depth)), col, 24, 1.f);
      }
    };
    for (const auto& p : e.s.edge) {
      disc(p, IM_COL32(255, 255, 255, 40));
    }
    for (const auto& p : e.s.lush) {
      disc(p, IM_COL32(255, 255, 255, 40));
    }
  }
  // the brush on the ground
  const bool over_ui = io.WantCaptureMouse && !e.pad;
  if ((e.tool == kGrass || e.tool == kPlace) && e.hover_ok && !over_ui && !e.modal) {
    float r = (e.tool == kGrass ? e.grass_brush : e.place_brush) * kMeter;
    const bool erasing = io.KeyCtrl || (e.pad && ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft));
    ImU32 col = erasing ? IM_COL32(255, 255, 255, 230)
                : e.tool == kGrass ? IM_COL32(120, 230, 90, 230)
                                   : IM_COL32(255, 170, 50, 230);
    draw_ground_circle(e, dl, e.hover, r, col, 2.f);
    ImVec2 c;
    if (to_screen(e, e.hover, &c)) {
      dl->AddCircleFilled(c, 2.5f, col);
    }
  }
  // the lights: a dot of their color, a ring when selected, their reach on the ground
  for (size_t i = 0; i < e.s.lights.size(); i++) {
    const auto& l = e.s.lights[i];
    ImVec2 sp;
    if (!to_screen(e, V3{l.pos[0], l.pos[1], l.pos[2]}, &sp)) {
      continue;
    }
    ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(l.color[0], 1.f), std::min(l.color[1], 1.f),
                                                      std::min(l.color[2], 1.f), 1.f));
    bool sel = std::find(e.sel.begin(), e.sel.end(), Pick{1, (int)i}) != e.sel.end();
    dl->AddCircleFilled(sp, 7.f, col);
    dl->AddCircle(sp, sel ? 12.f : 9.f, sel ? IM_COL32(255, 160, 40, 255) : IM_COL32(0, 0, 0, 200), 20,
                  sel ? 2.5f : 1.5f);
    if (sel) {
      draw_ground_circle(e, dl, V3{l.pos[0], l.pos[1], l.pos[2]}, l.radius, IM_COL32(255, 160, 40, 120), 1.f);
    }
  }
  if (e.boxing) {
    const ImVec2 b = pointer(e);
    dl->AddRectFilled(e.box_from, b, IM_COL32(255, 160, 40, 30));
    dl->AddRect(e.box_from, b, IM_COL32(255, 160, 40, 220), 0.f, 0, 1.5f);
  }
  // the falling ones
  for (const auto& f : e.falls) {
    if (f.idx < e.s.objects.size()) {
      draw_box(e, dl, e.s.objects[f.idx], IM_COL32(120, 200, 255, 200));
    }
  }
  // the selected objects' boxes
  for (const auto& p : e.sel) {
    if (p.kind == 0 && p.idx < (int)e.s.objects.size()) {
      draw_box(e, dl, e.s.objects[p.idx], IM_COL32(255, 160, 40, 255));
    }
  }
}

// test switch: JAK_ENHANCED_EDITOR_PADTEST=<file> presses the pad's buttons from it (no real pad
// needed): each time the file changes its lines play in turn, "<button> <held s> <then wait s>",
// buttons FaceDown FaceLeft FaceUp FaceRight DpadLeft DpadRight DpadUp DpadDown L2 L3 R3 Back Start
void pad_test() {
  static const char* file = std::getenv("JAK_ENHANCED_EDITOR_PADTEST");
  if (!file) {
    return;
  }
  struct Step {
    ImGuiKey key;
    double down, up;
  };
  static std::vector<Step> steps;
  static std::string last;
  static double checked = 0;
  const double now = ImGui::GetTime();
  if (now - checked > 0.25) {
    checked = now;
    std::string text = fs::exists(file) ? file_util::read_text_file(fs::path(file)) : "";
    if (text != last) {
      last = text;
      static const std::unordered_map<std::string, ImGuiKey> kNames = {
          {"FaceDown", ImGuiKey_GamepadFaceDown}, {"FaceLeft", ImGuiKey_GamepadFaceLeft},
          {"FaceUp", ImGuiKey_GamepadFaceUp},     {"FaceRight", ImGuiKey_GamepadFaceRight},
          {"DpadLeft", ImGuiKey_GamepadDpadLeft}, {"DpadRight", ImGuiKey_GamepadDpadRight},
          {"DpadUp", ImGuiKey_GamepadDpadUp},     {"DpadDown", ImGuiKey_GamepadDpadDown},
          {"L2", ImGuiKey_GamepadL2},             {"L3", ImGuiKey_GamepadL3},
          {"R3", ImGuiKey_GamepadR3},             {"Back", ImGuiKey_GamepadBack},
          {"Start", ImGuiKey_GamepadStart}};
      double t = now + 0.1;
      std::istringstream in(text);
      std::string name;
      double held, wait;
      while (in >> name >> held >> wait) {
        auto it = kNames.find(name);
        if (it != kNames.end()) {
          steps.push_back({it->second, t, t + held});
        }
        t += held + wait;
      }
    }
  }
  auto& io = ImGui::GetIO();
  for (size_t i = 0; i < steps.size();) {
    auto& s = steps[i];
    if (s.down >= 0 && now >= s.down) {
      io.AddKeyEvent(s.key, true);
      s.down = -1;
    }
    if (s.down < 0 && now >= s.up) {
      io.AddKeyEvent(s.key, false);
      steps.erase(steps.begin() + i);
      continue;
    }
    i++;
  }
}

}  // namespace

void enhanced_editor_toggle() {
  if (!g_enhanced_menu.editor) {
    g_enhanced_menu.editor = true;
    g_enhanced_menu.photo = true;
    g_enhanced_menu.open = false;
  } else {
    g_enhanced_menu.editor_leave = true;
  }
}

int enhanced_editor_probe(int i, float* out, int* id) {
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  if (i < 0 || i >= (int)g_probes.size()) {
    return 0;
  }
  const auto& p = g_probes[i];
  if (p.answered) {
    return 2;
  }
  out[0] = p.from[0];
  out[1] = p.from[1];
  out[2] = p.from[2];
  out[3] = p.len;
  *id = p.id;
  return 1;
}

void enhanced_editor_probe_hit(int id, float u) {
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  for (auto& p : g_probes) {
    if (p.id == id) {
      p.answered = true;
      p.u = u;
    }
  }
}

void enhanced_editor_camera(float* trans, float* rot) {
  CamMoves m;
  {
    std::lock_guard<std::mutex> lock(g_cam_mutex);
    if (!g_cam_moves.any) {
      return;
    }
    m = g_cam_moves;
    g_cam_moves = CamMoves();
  }
  if (!g_enhanced_menu.editing()) {
    return;
  }
  auto turn = [&](V3 v) {
    if (m.pitch != 0.f) {
      v = turn_axis(v, m.axis, m.pitch);
    }
    return m.yaw != 0.f ? turn_y(v, m.yaw) : v;
  };
  if (m.pitch != 0.f || m.yaw != 0.f) {
    V3 rel = turn(V3{trans[0], trans[1], trans[2]} - m.pivot);
    for (int k = 0; k < 3; k++) {
      trans[k] = m.pivot[k] + rel[k];
    }
    for (int r = 0; r < 3; r++) {
      V3 row = turn(V3{rot[r * 4], rot[r * 4 + 1], rot[r * 4 + 2]});
      for (int k = 0; k < 3; k++) {
        rot[r * 4 + k] = row[k];
      }
    }
  }
  for (int k = 0; k < 3; k++) {
    trans[k] += m.move[k];
  }
}

void enhanced_editor_frame(int w, int h, GLuint depth_fbo, const EnhancedCamera& cam, const float proj[4]) {
  auto& e = g_ed;
  enhanced_notes_frame(w, h, depth_fbo, cam, proj, g_enhanced_menu.editing() && e.tool == kNote);
  std::lock_guard<std::mutex> lock(e.mutex);
  e.cam = cam;
  std::copy(proj, proj + 4, e.proj);
  e.have_cam = true;
  if (!g_enhanced_menu.editing()) {
    return;
  }
  if (!e.pbo[0]) {
    glGenBuffers(2, e.pbo);
    for (int i = 0; i < 2; i++) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, e.pbo[i]);
      glBufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(float), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
  // last frame's depths: under the cursor, in the middle, beside the cursor (its surface's normal)
  const int prev = 1 - e.cur;
  if (e.pending[prev]) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, e.pbo[prev]);
    if (auto* d = (const float*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4 * sizeof(float), GL_MAP_READ_BIT)) {
      float depth[4] = {d[0], d[1], d[2], d[3]};
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      const auto& pc = e.pending_cam[prev];
      const float* pp = e.pending_proj[prev];
      V3 beside[2];
      bool beside_ok[2] = {false, false};
      for (int k = 0; k < 4; k++) {
        bool ok = false;
        V3 world = {0, 0, 0};
        if (depth[k] > 1e-7f && depth[k] < 0.999999f) {
          // (as enh_composite.frag's view_pos, then to the world)
          float z = pp[1] / (depth[k] * 2.f - 1.f - pp[0]);
          float v[3] = {(e.pending_uv[prev][k][0] * 2.f - 1.f) * z / pp[2],
                        (e.pending_uv[prev][k][1] * 2.f - 1.f) * z / pp[3], z};
          if (z > 0.3f * kMeter && z < 3000.f * kMeter) {
            for (int r = 0; r < 3; r++) {
              world[r] = pc.rot[r][0] * v[0] + pc.rot[r][1] * v[1] + pc.rot[r][2] * v[2] + pc.trans[r];
            }
            ok = true;
          }
        }
        if (k == 0) {
          e.hover_ok = ok;
          e.hover = world;
        } else if (k == 1) {
          e.center_ok = ok;
          e.center = world;
        } else {
          beside[k - 2] = world;
          beside_ok[k - 2] = ok;
        }
      }
      // (the normal: across the two beside it, turned toward the camera)
      e.normal_ok = false;
      if (e.hover_ok && beside_ok[0] && beside_ok[1]) {
        V3 n = cross(beside[0] - e.hover, beside[1] - e.hover);
        const float len = length(n);
        if (len > 1e-3f) {
          n = n * (1.f / len);
          const V3 to_cam = V3{pc.trans[0], pc.trans[1], pc.trans[2]} - e.hover;
          if (dot(n, to_cam) < 0.f) {
            n = n * -1.f;
          }
          e.hover_normal = n;
          e.normal_ok = true;
        }
      }
      e.hover_frame = e.frames;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    e.pending[prev] = false;
  }
  int px = std::clamp((int)(e.cursor[0] * w), 0, w - 1);
  int py = std::clamp((int)(e.cursor[1] * h), 0, h - 1);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, e.pbo[e.cur]);
  glReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glReadPixels(w / 2, h / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void*)sizeof(float));
  const int bx = std::min(px + 6, w - 1), by = std::min(py + 6, h - 1);
  glReadPixels(bx, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void*)(2 * sizeof(float)));
  glReadPixels(px, by, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, (void*)(3 * sizeof(float)));
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  e.pending[e.cur] = true;
  e.pending_cam[e.cur] = cam;
  std::copy(proj, proj + 4, e.pending_proj[e.cur]);
  e.pending_uv[e.cur][0][0] = (px + 0.5f) / w;
  e.pending_uv[e.cur][0][1] = (py + 0.5f) / h;
  e.pending_uv[e.cur][1][0] = (w / 2 + 0.5f) / w;
  e.pending_uv[e.cur][1][1] = (h / 2 + 0.5f) / h;
  e.pending_uv[e.cur][2][0] = (bx + 0.5f) / w;
  e.pending_uv[e.cur][2][1] = (py + 0.5f) / h;
  e.pending_uv[e.cur][3][0] = (px + 0.5f) / w;
  e.pending_uv[e.cur][3][1] = (by + 0.5f) / h;
  e.cur = prev;
}

void enhanced_editor_ui() {
  auto& e = g_ed;
  if (!e.loaded) {
    e.loaded = true;
    load_objects(e);
    load_settings(e);
    enhanced_set_placed(e.s.objects);
  }
  // (photo mode left some other way: the editor too, its changes kept in memory, unsaved)
  if (g_enhanced_menu.editor && !g_enhanced_menu.photo) {
    g_enhanced_menu.editor = false;
  }
  const bool on = g_enhanced_menu.editing();
  enhanced_notes_ui(on && e.tool == kNote);
  if (!on) {
    if (e.was_on) {
      e.was_on = false;
      e.have_jak_at = false;
      if (e.modal) {
        end_modal(e, false);
      }
      e.painting = false;
      e.nav = 0;
      e.resizing = false;
      e.pad_grab = false;
      e.ask_leave = false;
      // (what was falling lands at once)
      for (const auto& f : e.falls) {
        if (f.have_ground && f.idx < e.s.objects.size()) {
          e.s.objects[f.idx].pos[1] = f.ground;
        }
      }
      e.falls.clear();
      push_objects(e);
    }
    g_enhanced_menu.editor_leave = false;
    return;
  }
  std::lock_guard<std::mutex> lock(e.mutex);
  if (!e.was_on) {
    e.was_on = true;
    e.models = enhanced_scenery_models();
    e.s.lights = enhanced_lights();
    std::vector<std::array<float, 3>> lush, edge;
    if (!e.dirty) {
      if (enhanced_grass_painted(&lush, &edge, &e.s.fade)) {
        e.s.lush.assign(lush.begin(), lush.end());
        e.s.edge.assign(edge.begin(), edge.end());
      }
      e.saved = e.s;
    }
  }
  pad_test();
  e.frames++;
  auto& io = ImGui::GetIO();
  const ImVec2 at = pointer(e);
  e.cursor[0] = at.x / std::max(io.DisplaySize.x, 1.f);
  e.cursor[1] = 1.f - at.y / std::max(io.DisplaySize.y, 1.f);
  if (!e.have_cam) {
    return;
  }
  // (the selection kept valid)
  std::erase_if(e.sel, [&](const Pick& p) {
    return p.idx < 0 || p.idx >= (int)(p.kind == 0 ? e.s.objects.size() : e.s.lights.size());
  });
  if (e.ask_leave) {
    // (the question's up: nothing else)
  } else if (e.tool != kNote) {
    handle_input(e);
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    e.tool = kSelect;
  }
  update_falls(e);
  keep_above_ground(e);
  // (Blender's look, its own font at its size: the game's style back after)
  const ImGuiStyle game_style = ImGui::GetStyle();
  blender_style(ImGui::GetStyle(), ui_k());
  ImGui::PushFont(ImGui::g_ui_font, 16.f * ui_k());
  draw_marks(e);
  pivot_menu(e);
  topbar(e);
  if (e.show_tools) {
    toolbar(e);
  }
  if (e.show_panel) {
    side_panel(e);
  }
  panel_tab(e);
  selection_buttons(e);
  add_menu(e);
  hint_bar(e);
  help_window(e);
  import_window(e);
  terrain_window(e);
  leave_popup(e);
  ImGui::PopFont();
  ImGui::GetStyle() = game_style;
}

void enhanced_editor_levels(const std::vector<LevelData*>& levels) {
  g_levels = levels;
}
