#include "EnhancedNotes.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/enhanced_state.h"

#include "third-party/imgui/imgui.h"
#include "third-party/json.hpp"

namespace {

constexpr float kMeter = 4096.f;

struct Brush {
  const char* name;
  ImU32 color;
};
// (keys 1-6)
constexpr Brush kBrushes[] = {
    {"red", IM_COL32(235, 60, 50, 255)},     {"green", IM_COL32(60, 210, 70, 255)},
    {"blue", IM_COL32(60, 120, 245, 255)},   {"yellow", IM_COL32(245, 215, 40, 255)},
    {"white", IM_COL32(245, 245, 245, 255)}, {"purple", IM_COL32(175, 80, 230, 255)},
};
constexpr int kNumBrushes = sizeof(kBrushes) / sizeof(kBrushes[0]);

struct Stroke {
  int brush = 0;
  float size = 2.f;                // m, the brush's radius
  std::vector<std::array<float, 3>> points;  // world, meters
};

struct Notes {
  std::mutex mutex;
  bool loaded = false;
  std::vector<Stroke> strokes;
  int brush = 0;
  float size = 2.f;
  bool erase = false;
  bool hidden = false;
  // the cursor, asked by the ImGui pass, read by the post pass a frame later
  bool painting = false;     // the button held over the picture
  bool new_stroke = true;    // the next point starts a stroke
  float cursor[2] = {0, 0};  // 0..1, from the bottom left
  // the depth readback in flight: the pixel buffer, and the camera it was read with
  GLuint pbo[2] = {0, 0};
  bool pending[2] = {false, false};
  EnhancedCamera pending_cam[2];
  float pending_proj[2][4] = {};
  float pending_uv[2][2] = {};
  int cur = 0;
  // the camera of the last frame (to draw the strokes over it)
  EnhancedCamera cam;
  float proj[4] = {0, 0, 1, 1};
};
Notes g_notes;

fs::path notes_path() {
  return file_util::get_jak_project_dir() / "local" / "notes" / "notes.json";
}

void save(const Notes& n) {
  nlohmann::json j;
  j["comment"] =
      "Notes painted in photo mode (EnhancedNotes): brush color, radius (m), points (x, y, z, m: the "
      "game's world, y up)";
  auto& arr = j["strokes"] = nlohmann::json::array();
  for (const auto& s : n.strokes) {
    nlohmann::json e;
    e["color"] = kBrushes[s.brush].name;
    e["radius"] = s.size;
    auto& pts = e["points"] = nlohmann::json::array();
    for (const auto& p : s.points) {
      pts.push_back({std::round(p[0] * 10) / 10, std::round(p[1] * 10) / 10, std::round(p[2] * 10) / 10});
    }
    arr.push_back(e);
  }
  try {
    file_util::create_dir_if_needed(notes_path().parent_path());
    file_util::write_text_file(notes_path(), j.dump(1));
  } catch (const std::exception& e) {
    lg::error("[enhanced] notes: {}", e.what());
  }
}

void load(Notes& n) {
  n.loaded = true;
  if (!fs::exists(notes_path())) {
    return;
  }
  try {
    auto j = nlohmann::json::parse(file_util::read_text_file(notes_path()));
    for (const auto& e : j.at("strokes")) {
      Stroke s;
      const std::string color = e.value("color", "red");
      for (int b = 0; b < kNumBrushes; b++) {
        if (color == kBrushes[b].name) {
          s.brush = b;
        }
      }
      s.size = e.value("radius", 2.f);
      for (const auto& p : e.at("points")) {
        s.points.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
      }
      n.strokes.push_back(std::move(s));
    }
  } catch (const std::exception& e) {
    lg::error("[enhanced] notes: {}", e.what());
  }
}

// a world point (m) to the screen (pixels): false behind the camera
bool project(const EnhancedCamera& cam, const float proj[4], const std::array<float, 3>& p, ImVec2 size,
             ImVec2* out, float* z) {
  float d[3] = {p[0] * kMeter - cam.trans[0], p[1] * kMeter - cam.trans[1], p[2] * kMeter - cam.trans[2]};
  float v[3];
  for (int c = 0; c < 3; c++) {
    v[c] = d[0] * cam.rot[0][c] + d[1] * cam.rot[1][c] + d[2] * cam.rot[2][c];
  }
  // (the view looks down +z: enh_composite.frag's view_pos)
  if (v[2] < 0.3f * kMeter) {
    return false;
  }
  float nx = v[0] * proj[2] / v[2], ny = v[1] * proj[3] / v[2];
  *out = ImVec2((nx * 0.5f + 0.5f) * size.x, (1.f - (ny * 0.5f + 0.5f)) * size.y);
  *z = v[2];
  return true;
}

void add_point(Notes& n, const std::array<float, 3>& w) {
  if (n.erase) {
    // the eraser: the points within its reach go, strokes split where they did
    std::vector<Stroke> kept;
    for (auto& s : n.strokes) {
      Stroke part = s;
      part.points.clear();
      for (const auto& p : s.points) {
        float d = std::hypot(p[0] - w[0], p[2] - w[2]);
        if (d < n.size && std::abs(p[1] - w[1]) < n.size + 3.f) {
          if (!part.points.empty()) {
            kept.push_back(part);
          }
          part.points.clear();
        } else {
          part.points.push_back(p);
        }
      }
      if (!part.points.empty()) {
        kept.push_back(part);
      }
    }
    n.strokes.swap(kept);
    return;
  }
  if (n.new_stroke || n.strokes.empty()) {
    Stroke s;
    s.brush = n.brush;
    s.size = n.size;
    n.strokes.push_back(s);
    n.new_stroke = false;
  }
  auto& pts = n.strokes.back().points;
  // (a point every third of the brush)
  if (!pts.empty()) {
    const auto& l = pts.back();
    if (std::hypot(l[0] - w[0], l[1] - w[1], l[2] - w[2]) < n.size * 0.3f) {
      return;
    }
  }
  pts.push_back(w);
}
}  // namespace

void enhanced_notes_frame(int w,
                          int h,
                          GLuint depth_fbo,
                          const EnhancedCamera& cam,
                          const float proj[4],
                          bool active) {
  auto& n = g_notes;
  std::lock_guard<std::mutex> lock(n.mutex);
  n.cam = cam;
  std::copy(proj, proj + 4, n.proj);
  if (!active) {
    return;
  }
  if (!n.pbo[0]) {
    glGenBuffers(2, n.pbo);
    for (int i = 0; i < 2; i++) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, n.pbo[i]);
      glBufferData(GL_PIXEL_PACK_BUFFER, sizeof(float), nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
  // last frame's depth under the cursor: the world point there
  const int prev = 1 - n.cur;
  if (n.pending[prev]) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, n.pbo[prev]);
    if (auto* d = (const float*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sizeof(float), GL_MAP_READ_BIT)) {
      const float depth = d[0];
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
      const auto& pc = n.pending_cam[prev];
      const float* pp = n.pending_proj[prev];
      if (depth > 1e-7f && n.painting) {
        // (as enh_composite.frag's view_pos, then to the world)
        float z = pp[1] / (depth * 2.f - 1.f - pp[0]);
        float v[3] = {(n.pending_uv[prev][0] * 2.f - 1.f) * z / pp[2],
                      (n.pending_uv[prev][1] * 2.f - 1.f) * z / pp[3], z};
        if (z > 0.3f * kMeter && z < 2000.f * kMeter) {
          std::array<float, 3> world;
          for (int r = 0; r < 3; r++) {
            world[r] = (pc.rot[r][0] * v[0] + pc.rot[r][1] * v[1] + pc.rot[r][2] * v[2] + pc.trans[r]) / kMeter;
          }
          add_point(n, world);
        }
      }
    }
    n.pending[prev] = false;
  }
  if (n.painting) {
    int px = std::clamp((int)(n.cursor[0] * w), 0, w - 1);
    int py = std::clamp((int)(n.cursor[1] * h), 0, h - 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, depth_fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, n.pbo[n.cur]);
    glReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    n.pending[n.cur] = true;
    n.pending_cam[n.cur] = cam;
    std::copy(proj, proj + 4, n.pending_proj[n.cur]);
    n.pending_uv[n.cur][0] = (px + 0.5f) / w;
    n.pending_uv[n.cur][1] = (py + 0.5f) / h;
    n.cur = prev;
  }
}

void enhanced_notes_ui(bool active) {
  auto& n = g_notes;
  std::lock_guard<std::mutex> lock(n.mutex);
  if (!n.loaded) {
    load(n);
  }
  if (!active) {
    if (n.painting) {
      n.painting = false;
      save(n);
    }
    return;
  }
  auto& io = ImGui::GetIO();
  const ImVec2 size = io.DisplaySize;
  // the keys
  for (int b = 0; b < kNumBrushes; b++) {
    if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + b), false)) {
      n.brush = b;
      n.erase = false;
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
    n.erase = !n.erase;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_H, false)) {
    n.hidden = !n.hidden;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
    n.size = std::max(0.25f, n.size / 1.25f);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
    n.size = std::min(40.f, n.size * 1.25f);
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !n.strokes.empty()) {
    n.strokes.pop_back();
    save(n);
  }
  // painting: the button (the pen) held over the picture, not over a window
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.WantCaptureMouse;
  if (down) {
    n.painting = true;
    n.cursor[0] = io.MousePos.x / std::max(size.x, 1.f);
    n.cursor[1] = 1.f - io.MousePos.y / std::max(size.y, 1.f);
  } else if (n.painting) {
    n.painting = false;
    n.new_stroke = true;
    save(n);
  }

  // the strokes, over the picture: wide as the brush is at that distance, see-through
  auto* dl = ImGui::GetForegroundDrawList();
  const float px_per_unit = 0.5f * size.x * n.proj[2];
  if (!n.hidden) {
    for (const auto& s : n.strokes) {
      const ImU32 col = (kBrushes[s.brush].color & 0x00ffffffu) | (150u << 24);
      ImVec2 prev_p;
      float prev_z = 0.f;
      bool have_prev = false;
      for (const auto& p : s.points) {
        ImVec2 sp;
        float z;
        if (!project(n.cam, n.proj, p, size, &sp, &z)) {
          have_prev = false;
          continue;
        }
        const float r = std::max(2.f, s.size * kMeter * px_per_unit / z);
        if (have_prev) {
          dl->AddLine(prev_p, sp, col, std::max(2.f, (r + s.size * kMeter * px_per_unit / prev_z)));
        }
        dl->AddCircleFilled(sp, r, col, 20);
        prev_p = sp;
        prev_z = z;
        have_prev = true;
      }
    }
  }
  // the cursor: the brush's color, a ring (the eraser: white, dashed look)
  dl->AddCircle(io.MousePos, 10.f, n.erase ? IM_COL32(255, 255, 255, 255) : kBrushes[n.brush].color, 24,
                2.5f);

  // the panel
  ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, 20.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::Begin("##notes", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::Text("Notes   brush: %s   size: %.1f m   strokes: %d%s", n.erase ? "eraser" : kBrushes[n.brush].name,
              n.size, (int)n.strokes.size(), n.hidden ? "   (hidden)" : "");
  ImGui::TextUnformatted(
      "pen / left button: paint   1-6: color   [ ]: size   E: eraser   Backspace: undo   H: hide   Esc: back");
  for (int b = 0; b < kNumBrushes; b++) {
    if (b > 0) {
      ImGui::SameLine();
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kBrushes[b].color), "%d %s", b + 1, kBrushes[b].name);
  }
  ImGui::End();
}
