#include "EnhancedTerrain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>

#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/graphics/enhanced_state.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/pipelines/opengl.h"
#include "game/runtime.h"

#include "fmt/core.h"
#include "third-party/stb_image/stb_image.h"

namespace {
constexpr float kMeter = 4096.f;
constexpr int kChunk = 32;  // cells per side of a draw
constexpr int kChunks = (kTerrainSize - 1) / kChunk;

struct GpuVertex {
  float pos[3];  // world, game units (first: the shadow pass reads only this)
  float normal[3];
  u8 mask[4];
};

struct Terrain {
  std::mutex mutex;  // (sculpted on the render thread, read by the game's collision)
  bool on = false;
  float x0 = 0, z0 = 0;
  std::vector<float> h;
  std::vector<u32> mask;
  // the part changed since it was last sent, in points (lo > hi: none)
  int dirty_lo[2] = {1, 1}, dirty_hi[2] = {0, 0};
  std::vector<float> chunk_lo, chunk_hi;
  float y0 = 0;  // the land past the grid, and the grid's rim
  std::vector<float> grab_base;
  std::vector<std::array<float, 3>> lakes;
  bool water_dirty = false;
  int water_indices = 0;
  GLuint water_vao = 0, water_vbo = 0, water_ibo = 0;
  GLuint ring_vao = 0, ring_vbo = 0;

  GLuint vao = 0, vbo = 0, ibo = 0;
  GLuint tex[kMatCount] = {};
  float tex_luma[kMatCount] = {};
  bool textures_tried = false;
  bool ring_ok = false;
  u64 drawn_frame = 0;
  const tfrag3::ShrubTree* lit_tree = nullptr;
  int tod = 0;
  float base = 0.5f;
} g_t;

float& height_at(int i, int j) {
  return g_t.h[j * kTerrainSize + i];
}

void mark_dirty(int i0, int j0, int i1, int j1) {
  if (g_t.dirty_lo[0] > g_t.dirty_hi[0]) {
    g_t.dirty_lo[0] = i0;
    g_t.dirty_lo[1] = j0;
    g_t.dirty_hi[0] = i1;
    g_t.dirty_hi[1] = j1;
    return;
  }
  g_t.dirty_lo[0] = std::min(g_t.dirty_lo[0], i0);
  g_t.dirty_lo[1] = std::min(g_t.dirty_lo[1], j0);
  g_t.dirty_hi[0] = std::max(g_t.dirty_hi[0], i1);
  g_t.dirty_hi[1] = std::max(g_t.dirty_hi[1], j1);
}

void make_vertex(int i, int j, GpuVertex* v) {
  const int il = std::max(i - 1, 0), ir = std::min(i + 1, kTerrainSize - 1);
  const int jl = std::max(j - 1, 0), jr = std::min(j + 1, kTerrainSize - 1);
  const float nx = (height_at(il, j) - height_at(ir, j)) / ((ir - il) * kTerrainCell);
  const float nz = (height_at(i, jl) - height_at(i, jr)) / ((jr - jl) * kTerrainCell);
  const float len = std::sqrt(nx * nx + nz * nz + 1.f);
  v->pos[0] = (g_t.x0 + i * kTerrainCell) * kMeter;
  v->pos[1] = height_at(i, j) * kMeter;
  v->pos[2] = (g_t.z0 + j * kTerrainCell) * kMeter;
  v->normal[0] = nx / len;
  v->normal[1] = 1.f / len;
  v->normal[2] = nz / len;
  const u32 m = g_t.mask[j * kTerrainSize + i];
  v->mask[0] = m & 255;
  v->mask[1] = (m >> 8) & 255;
  v->mask[2] = (m >> 16) & 255;
  v->mask[3] = 255;
}

void load_textures() {
  g_t.textures_tried = true;
  // (the ground's close-up grain, tinted in the shader; without the file a plain color)
  static const char* files[kMatCount] = {"sand_fine.png", "ground_detail.png", "rock_detail.png",
                                         "sand_coarse.png"};
  const auto dir =
      file_util::get_jak_project_dir() / "custom_assets" / game_version_names[g_game_version] / "scenery";
  for (int m = 0; m < kMatCount; m++) {
    int w = 0, h = 0;
    u8* px = stbi_load((dir / files[m]).string().c_str(), &w, &h, nullptr, 4);
    u8 white[4] = {255, 255, 255, 255};
    float sum = 0;
    if (px) {
      for (int k = 0; k < w * h; k++) {
        sum += 0.3f * px[k * 4] + 0.59f * px[k * 4 + 1] + 0.11f * px[k * 4 + 2];
      }
    }
    g_t.tex_luma[m] = px && w * h ? std::max(sum / (w * h) / 255.f, 0.05f) : 1.f;
    glGenTextures(1, &g_t.tex[m]);
    glBindTexture(GL_TEXTURE_2D, g_t.tex[m]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, px ? w : 1, px ? h : 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px ? px : white);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (px) {
      stbi_image_free(px);
    }
  }
}

void make_gl() {
  glGenVertexArrays(1, &g_t.vao);
  glGenBuffers(1, &g_t.vbo);
  glGenBuffers(1, &g_t.ibo);
  glBindVertexArray(g_t.vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_t.vbo);
  glBufferData(GL_ARRAY_BUFFER, (size_t)kTerrainSize * kTerrainSize * sizeof(GpuVertex), nullptr,
               GL_DYNAMIC_DRAW);
  // each chunk's indices one after the other, so a chunk is a range
  std::vector<u32> idx;
  idx.reserve((size_t)kChunks * kChunks * kChunk * kChunk * 6);
  for (int cj = 0; cj < kChunks; cj++) {
    for (int ci = 0; ci < kChunks; ci++) {
      for (int j = cj * kChunk; j < (cj + 1) * kChunk; j++) {
        for (int i = ci * kChunk; i < (ci + 1) * kChunk; i++) {
          const u32 a = j * kTerrainSize + i, b = a + 1, c = a + kTerrainSize, d = c + 1;
          idx.insert(idx.end(), {a, c, b, b, c, d});
        }
      }
    }
  }
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_t.ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(u32), idx.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, pos));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                        (void*)offsetof(GpuVertex, normal));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                        (void*)offsetof(GpuVertex, mask));
  glBindVertexArray(0);
}

// the land past the grid, out to where the fog has it
void make_ring() {
  const float far_m = 20000.f;
  const float xa = g_t.x0, xb = g_t.x0 + (kTerrainSize - 1) * kTerrainCell;
  const float za = g_t.z0, zb = g_t.z0 + (kTerrainSize - 1) * kTerrainCell;
  const float cx = (xa + xb) * 0.5f, cz = (za + zb) * 0.5f;
  const float xs[4] = {cx - far_m, xa, xb, cx + far_m}, zs[4] = {cz - far_m, za, zb, cz + far_m};
  std::vector<GpuVertex> v;
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 3; i++) {
      if (i == 1 && j == 1) {
        continue;
      }
      const int c[6][2] = {{i, j}, {i, j + 1}, {i + 1, j}, {i + 1, j}, {i, j + 1}, {i + 1, j + 1}};
      for (const auto& p : c) {
        GpuVertex g = {};
        g.pos[0] = xs[p[0]] * kMeter;
        g.pos[1] = g_t.y0 * kMeter;
        g.pos[2] = zs[p[1]] * kMeter;
        g.normal[1] = 1.f;
        g.mask[3] = 255;
        v.push_back(g);
      }
    }
  }
  if (!g_t.ring_vao) {
    glGenVertexArrays(1, &g_t.ring_vao);
    glGenBuffers(1, &g_t.ring_vbo);
  }
  glBindVertexArray(g_t.ring_vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_t.ring_vbo);
  glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(GpuVertex), v.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, pos));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                        (void*)offsetof(GpuVertex, normal));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                        (void*)offsetof(GpuVertex, mask));
  glBindVertexArray(0);
}

// each lake: from its point, every cell with some of it under its water
void make_water() {
  g_t.water_dirty = false;
  std::vector<GpuVertex> verts;
  std::vector<u32> idx;
  const int cells = kTerrainSize - 1;
  std::vector<u8> seen((size_t)cells * cells), wet((size_t)cells * cells);
  std::vector<int> open;
  auto quad = [&](float x0, float z0, float x1, float z1, float y) {
    const u32 first = (u32)verts.size();
    for (int k = 0; k < 4; k++) {
      GpuVertex g = {};
      g.pos[0] = (k & 1 ? x1 : x0) * kMeter;
      g.pos[1] = y * kMeter;
      g.pos[2] = (k >> 1 ? z1 : z0) * kMeter;
      g.normal[1] = 1.f;
      verts.push_back(g);
    }
    idx.insert(idx.end(), {first, first + 2, first + 1, first + 1, first + 2, first + 3});
  };
  for (const auto& lake : g_t.lakes) {
    std::fill(seen.begin(), seen.end(), 0);
    std::fill(wet.begin(), wet.end(), 0);
    bool sea = false;
    open.clear();
    const int si = (int)((lake[0] - g_t.x0) / kTerrainCell), sj = (int)((lake[1] - g_t.z0) / kTerrainCell);
    if (si < 0 || sj < 0 || si >= cells || sj >= cells) {
      continue;
    }
    open.push_back(sj * cells + si);
    seen[open[0]] = 1;
    while (!open.empty()) {
      const int c = open.back();
      open.pop_back();
      const int i = c % cells, j = c / cells;
      const float low = std::min({height_at(i, j), height_at(i + 1, j), height_at(i, j + 1), height_at(i + 1, j + 1)});
      if (low >= lake[2]) {
        continue;
      }
      wet[c] = 1;
      sea = sea || i == 0 || j == 0 || i == cells - 1 || j == cells - 1;
      const int next[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
      for (const auto& n : next) {
        if (n[0] >= 0 && n[1] >= 0 && n[0] < cells && n[1] < cells && !seen[n[1] * cells + n[0]]) {
          seen[n[1] * cells + n[0]] = 1;
          open.push_back(n[1] * cells + n[0]);
        }
      }
    }
    // (a flat surface: each row's runs as one long piece, flooding a plain stays cheap)
    for (int j = 0; j < cells; j++) {
      for (int i = 0; i < cells; i++) {
        if (!wet[j * cells + i]) {
          continue;
        }
        int end = i;
        while (end + 1 < cells && wet[j * cells + end + 1]) {
          end++;
        }
        quad(g_t.x0 + i * kTerrainCell, g_t.z0 + j * kTerrainCell, g_t.x0 + (end + 1) * kTerrainCell,
             g_t.z0 + (j + 1) * kTerrainCell, lake[2]);
        i = end;
      }
    }
    // it got to the rim of the grid: it's a sea, out to the horizon
    if (sea) {
      const float far_m = 20000.f;
      const float xa = g_t.x0, xb = g_t.x0 + cells * kTerrainCell, za = g_t.z0, zb = g_t.z0 + cells * kTerrainCell;
      const float xs[4] = {xa - far_m, xa, xb, xb + far_m}, zs[4] = {za - far_m, za, zb, zb + far_m};
      for (int b = 0; b < 3; b++) {
        for (int a = 0; a < 3; a++) {
          if (a != 1 || b != 1) {
            quad(xs[a], zs[b], xs[a + 1], zs[b + 1], lake[2]);
          }
        }
      }
    }
  }
  if (!g_t.water_vao) {
    glGenVertexArrays(1, &g_t.water_vao);
    glGenBuffers(1, &g_t.water_vbo);
    glGenBuffers(1, &g_t.water_ibo);
  }
  glBindVertexArray(g_t.water_vao);
  glBindBuffer(GL_ARRAY_BUFFER, g_t.water_vbo);
  glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GpuVertex), verts.data(), GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_t.water_ibo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(u32), idx.data(), GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)offsetof(GpuVertex, pos));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                        (void*)offsetof(GpuVertex, normal));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                        (void*)offsetof(GpuVertex, mask));
  glBindVertexArray(0);
  g_t.water_indices = (int)idx.size();
}

// how soft a brush is at this distance over its radius; the rim of the grid stays put, so it meets the
// land past it
float brush_weight(int i, int j, float x, float z, float radius) {
  const float dx = g_t.x0 + i * kTerrainCell - x, dz = g_t.z0 + j * kTerrainCell - z;
  const float d2 = (dx * dx + dz * dz) / (radius * radius);
  if (d2 >= 1.f) {
    return 0.f;
  }
  const float s = 1.f - d2;
  const int border = std::min({i, j, kTerrainSize - 1 - i, kTerrainSize - 1 - j});
  const float rim = std::clamp((border - 1) / 12.f, 0.f, 1.f);
  return s * s * rim * rim * (3.f - 2.f * rim);
}

// what changed: to the GPU, and each chunk's height range
void send_dirty() {
  if (g_t.dirty_lo[0] > g_t.dirty_hi[0]) {
    return;
  }
  const int i0 = std::max(g_t.dirty_lo[0] - 1, 0), i1 = std::min(g_t.dirty_hi[0] + 1, kTerrainSize - 1);
  const int j0 = std::max(g_t.dirty_lo[1] - 1, 0), j1 = std::min(g_t.dirty_hi[1] + 1, kTerrainSize - 1);
  g_t.dirty_lo[0] = 1;
  g_t.dirty_hi[0] = 0;
  std::vector<GpuVertex> row(i1 - i0 + 1);
  glBindBuffer(GL_ARRAY_BUFFER, g_t.vbo);
  for (int j = j0; j <= j1; j++) {
    for (int i = i0; i <= i1; i++) {
      make_vertex(i, j, &row[i - i0]);
    }
    glBufferSubData(GL_ARRAY_BUFFER, ((size_t)j * kTerrainSize + i0) * sizeof(GpuVertex),
                    row.size() * sizeof(GpuVertex), row.data());
  }
  g_t.chunk_lo.resize((size_t)kChunks * kChunks);
  g_t.chunk_hi.resize((size_t)kChunks * kChunks);
  for (int cj = j0 / kChunk; cj <= std::min(j1 / kChunk, kChunks - 1); cj++) {
    for (int ci = i0 / kChunk; ci <= std::min(i1 / kChunk, kChunks - 1); ci++) {
      float lo = 1e30f, hi = -1e30f;
      for (int j = cj * kChunk; j <= (cj + 1) * kChunk; j++) {
        for (int i = ci * kChunk; i <= (ci + 1) * kChunk; i++) {
          lo = std::min(lo, height_at(i, j));
          hi = std::max(hi, height_at(i, j));
        }
      }
      g_t.chunk_lo[cj * kChunks + ci] = lo;
      g_t.chunk_hi[cj * kChunks + ci] = hi;
    }
  }
}
}  // namespace

float enhanced_terrain_height(float x, float z) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on) {
    return 0.f;
  }
  const float fx = (x - g_t.x0) / kTerrainCell, fz = (z - g_t.z0) / kTerrainCell;
  if (fx < 0 || fz < 0 || fx >= kTerrainSize - 1 || fz >= kTerrainSize - 1) {
    return g_t.y0;
  }
  const int i = (int)fx, j = (int)fz;
  const float a = fx - i, b = fz - j;
  return (height_at(i, j) * (1 - a) + height_at(i + 1, j) * a) * (1 - b) +
         (height_at(i, j + 1) * (1 - a) + height_at(i + 1, j + 1) * a) * b;
}

void enhanced_terrain_brush(TerrainBrush brush, float x, float z, float radius, float amount, float target, int mat) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on) {
    return;
  }
  const int i0 = std::max((int)std::floor((x - radius - g_t.x0) / kTerrainCell), 0);
  const int i1 = std::min((int)std::ceil((x + radius - g_t.x0) / kTerrainCell), kTerrainSize - 1);
  const int j0 = std::max((int)std::floor((z - radius - g_t.z0) / kTerrainCell), 0);
  const int j1 = std::min((int)std::ceil((z + radius - g_t.z0) / kTerrainCell), kTerrainSize - 1);
  if (i0 > i1 || j0 > j1) {
    return;
  }
  // (the smoothing reads what the dab found, not what it's just changed: it'd smear one way)
  std::vector<float> before;
  if (brush == kBrushSmooth) {
    before = g_t.h;
  }
  for (int j = j0; j <= j1; j++) {
    for (int i = i0; i <= i1; i++) {
      const float w = brush_weight(i, j, x, z, radius);
      if (w <= 0.f) {
        continue;
      }
      float& h = height_at(i, j);
      switch (brush) {
        case kBrushRaise:
          h += amount * w;
          break;
        case kBrushFlatten:
          h += (target - h) * std::min(amount * w, 1.f);
          break;
        case kBrushSmooth: {
          auto at = [&](int a, int b) {
            a = std::clamp(a, 0, kTerrainSize - 1);
            b = std::clamp(b, 0, kTerrainSize - 1);
            return before[b * kTerrainSize + a];
          };
          float sum = 0.f;
          for (int b = -2; b <= 2; b++) {
            for (int a = -2; a <= 2; a++) {
              sum += at(i + a, j + b);
            }
          }
          h += (sum / 25.f - h) * std::min(amount * w, 1.f);
          break;
        }
        case kBrushPaint: {
          u32& m = g_t.mask[j * kTerrainSize + i];
          u32 out = 0;
          const float k = std::min(amount * w, 1.f);
          for (int c = 0; c < 3; c++) {
            const float was = ((m >> (c * 8)) & 255) / 255.f;
            const float want = mat == c + 1 ? 1.f : 0.f;
            out |= (u32)std::lround(std::clamp(was + (want - was) * k, 0.f, 1.f) * 255.f) << (c * 8);
          }
          m = out;
          break;
        }
      }
    }
  }
  mark_dirty(i0, j0, i1, j1);
  g_t.water_dirty = g_t.water_dirty || brush != kBrushPaint;
}

void enhanced_terrain_grab_begin() {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  g_t.grab_base = g_t.h;
}

void enhanced_terrain_grab(float x, float z, float radius, float dy) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on || g_t.grab_base.size() != g_t.h.size()) {
    return;
  }
  const int i0 = std::max((int)std::floor((x - radius - g_t.x0) / kTerrainCell), 0);
  const int i1 = std::min((int)std::ceil((x + radius - g_t.x0) / kTerrainCell), kTerrainSize - 1);
  const int j0 = std::max((int)std::floor((z - radius - g_t.z0) / kTerrainCell), 0);
  const int j1 = std::min((int)std::ceil((z + radius - g_t.z0) / kTerrainCell), kTerrainSize - 1);
  if (i0 > i1 || j0 > j1) {
    return;
  }
  for (int j = j0; j <= j1; j++) {
    for (int i = i0; i <= i1; i++) {
      height_at(i, j) = g_t.grab_base[j * kTerrainSize + i] + dy * brush_weight(i, j, x, z, radius);
    }
  }
  mark_dirty(i0, j0, i1, j1);
  g_t.water_dirty = true;
}

void enhanced_terrain_set_lakes(const std::vector<std::array<float, 3>>& lakes) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  g_t.lakes = lakes;
  g_t.water_dirty = true;
}

void enhanced_draw_terrain_water(const TfragRenderSettings& settings,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on) {
    return;
  }
  if (g_t.water_dirty) {
    make_water();
  }
  if (!g_t.water_indices) {
    return;
  }
  auto& sh = render_state->shaders[ShaderId::ENH_TERRAIN];
  sh.activate();
  using clock = std::chrono::steady_clock;
  static const auto start = clock::now();
  glUniform1f(glGetUniformLocation(sh.id(), "enh_water"), 1.f);
  glUniform1f(glGetUniformLocation(sh.id(), "enh_time"),
              std::chrono::duration<float>(clock::now() - start).count());
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glDepthMask(GL_FALSE);
  glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDisable(GL_PRIMITIVE_RESTART);
  glBindVertexArray(g_t.water_vao);
  glDrawElements(GL_TRIANGLES, g_t.water_indices, GL_UNSIGNED_INT, nullptr);
  prof.add_draw_call();
  glBindVertexArray(0);
  glEnable(GL_PRIMITIVE_RESTART);
  glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glUniform1f(glGetUniformLocation(sh.id(), "enh_water"), 0.f);
}

void enhanced_terrain_set(float x0,
                          float z0,
                          float y0,
                          const std::vector<float>& heights,
                          const std::vector<u32>& mask) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  const size_t n = (size_t)kTerrainSize * kTerrainSize;
  g_t.x0 = x0;
  g_t.z0 = z0;
  g_t.y0 = y0;
  g_t.ring_ok = false;
  g_t.water_dirty = true;
  g_t.h = heights.size() == n ? heights : std::vector<float>(n, y0);
  g_t.mask = mask.size() == n ? mask : std::vector<u32>(n, 0u);
  g_t.on = true;
  g_t.lit_tree = nullptr;
  mark_dirty(0, 0, kTerrainSize - 1, kTerrainSize - 1);
}

void enhanced_terrain_clear() {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  g_t.on = false;
}

bool enhanced_terrain_on() {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  return g_t.on;
}

void enhanced_terrain_get(std::vector<float>* heights, std::vector<u32>* mask) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  *heights = g_t.h;
  *mask = g_t.mask;
}

bool enhanced_terrain_ray(const float* from, const float* dir, float* out) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on) {
    return false;
  }
  // marched in steps of a cell, then narrowed down
  auto above = [&](const float* p) {
    const float fx = (p[0] - g_t.x0) / kTerrainCell, fz = (p[2] - g_t.z0) / kTerrainCell;
    if (fx < 0 || fz < 0 || fx >= kTerrainSize - 1 || fz >= kTerrainSize - 1) {
      return true;
    }
    const int i = (int)fx, j = (int)fz;
    const float a = fx - i, b = fz - j;
    const float y = (height_at(i, j) * (1 - a) + height_at(i + 1, j) * a) * (1 - b) +
                    (height_at(i, j + 1) * (1 - a) + height_at(i + 1, j + 1) * a) * b;
    return p[1] > y;
  };
  float t0 = 0.f;
  for (float t = kTerrainCell; t < 3000.f; t += kTerrainCell) {
    const float p[3] = {from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t};
    if (!above(p)) {
      float a = t0, b = t;
      for (int k = 0; k < 10; k++) {
        const float m = (a + b) * 0.5f;
        const float q[3] = {from[0] + dir[0] * m, from[1] + dir[1] * m, from[2] + dir[2] * m};
        (above(q) ? a : b) = m;
      }
      for (int r = 0; r < 3; r++) {
        out[r] = from[r] + dir[r] * b;
      }
      return true;
    }
    t0 = t;
  }
  return false;
}

void enhanced_draw_terrain(const tfrag3::ShrubTree& tree,
                           const TfragRenderSettings& settings,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on || g_t.drawn_frame == render_state->frame_idx || tree.unpacked.vertices.empty()) {
    return;
  }
  g_t.drawn_frame = render_state->frame_idx;
  if (!g_t.vao) {
    make_gl();
  }
  if (!g_t.textures_tried) {
    load_textures();
  }
  send_dirty();
  if (!g_t.ring_ok) {
    make_ring();
    g_t.ring_ok = true;
  }
  // (lit by the time of day color of the vertex nearest its middle: the area's)
  if (g_t.lit_tree != &tree) {
    g_t.lit_tree = &tree;
    const float cx = (g_t.x0 + kTerrainSize) * kMeter, cz = (g_t.z0 + kTerrainSize) * kMeter;
    float best = 1e30f;
    const auto& verts = tree.unpacked.vertices;
    for (size_t v = 0; v < verts.size(); v += 3) {
      const float ex = verts[v].x - cx, ez = verts[v].z - cz, d = ex * ex + ez * ez;
      if (d < best) {
        best = d;
        g_t.tod = verts[v].color_index;
        g_t.base = (0.3f * verts[v].rgba_base[0] + 0.59f * verts[v].rgba_base[1] +
                    0.11f * verts[v].rgba_base[2]) /
                   255.f;
      }
    }
  }
  DrawMode solid = tree.static_draws.empty() ? DrawMode() : tree.static_draws[0].mode;
  solid.disable_at();
  auto& sh = render_state->shaders[ShaderId::ENH_TERRAIN];
  first_tfrag_draw_setup(settings.camera, render_state, ShaderId::ENH_TERRAIN);
  glUniform1i(glGetUniformLocation(sh.id(), "tex_T10"), 10);
  glUniform1i(glGetUniformLocation(sh.id(), "enh_tod"), g_t.tod);
  glUniform1f(glGetUniformLocation(sh.id(), "enh_water"), 0.f);
  {
    // (toward the sun, up at night: only the slopes show it, the flat ground keeps the sky's light)
    const float* d = g_enhanced_sun.dir;
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const bool sun = g_enhanced_sun.valid && len > 1e-6f && d[1] > 0.f;
    glUniform3f(glGetUniformLocation(sh.id(), "enh_sun"), sun ? d[0] / len : 0.f, sun ? d[1] / len : 1.f,
                sun ? d[2] / len : 0.f);
  }
  glUniform1f(glGetUniformLocation(sh.id(), "enh_base"), g_t.base);
  glUniform4f(glGetUniformLocation(sh.id(), "enh_luma"), g_t.tex_luma[0], g_t.tex_luma[1],
              g_t.tex_luma[2], g_t.tex_luma[3]);
  setup_tfrag_shader(render_state, solid, ShaderId::ENH_TERRAIN);
  for (int m = 0; m < kMatCount; m++) {
    glUniform1i(glGetUniformLocation(sh.id(), fmt::format("tex_mat{}", m).c_str()), 11 + m);
    glActiveTexture(GL_TEXTURE11 + m);
    glBindTexture(GL_TEXTURE_2D, g_t.tex[m]);
  }
  glActiveTexture(GL_TEXTURE0);
  glDisable(GL_PRIMITIVE_RESTART);
  glBindVertexArray(g_t.vao);
  const int per_chunk = kChunk * kChunk * 6;
  const float half = kChunk * kTerrainCell * 0.5f;
  int tris = 0;
  for (int cj = 0; cj < kChunks; cj++) {
    for (int ci = 0; ci < kChunks; ci++) {
      const float lo = g_t.chunk_lo[cj * kChunks + ci], hi = g_t.chunk_hi[cj * kChunks + ci];
      const float hy = (hi - lo) * 0.5f;
      const float r = std::sqrt(half * half * 2.f + hy * hy) * kMeter;
      if (!sphere_in_view_ref(math::Vector4f((g_t.x0 + (ci * 2 + 1) * half) * kMeter, (lo + hy) * kMeter,
                                             (g_t.z0 + (cj * 2 + 1) * half) * kMeter, r),
                              settings.camera.planes)) {
        continue;
      }
      glDrawElements(GL_TRIANGLES, per_chunk, GL_UNSIGNED_INT,
                     (void*)((size_t)(cj * kChunks + ci) * per_chunk * sizeof(u32)));
      tris += per_chunk / 3;
    }
  }
  prof.add_draw_call();
  prof.add_tri(tris);
  glBindVertexArray(g_t.ring_vao);
  glDrawArrays(GL_TRIANGLES, 0, 8 * 6);
  glEnable(GL_PRIMITIVE_RESTART);
  glBindVertexArray(0);
}

void enhanced_terrain_shadow_draw() {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on || !g_t.vao || g_t.chunk_lo.empty()) {
    return;
  }
  glBindBuffer(GL_ARRAY_BUFFER, g_t.vbo);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), (void*)0);
  glDisableVertexAttribArray(1);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_t.ibo);
  // (only the sides turned away from the sun: the lit ground doesn't shadow itself in stripes)
  glEnable(GL_CULL_FACE);
  glDrawElements(GL_TRIANGLES, kChunks * kChunks * kChunk * kChunk * 6, GL_UNSIGNED_INT, nullptr);
  glDisable(GL_CULL_FACE);
}

int enhanced_terrain_collide(const float* box, const float* seg, float* out, int stride, int max) {
  std::lock_guard<std::mutex> lock(g_t.mutex);
  if (!g_t.on) {
    return 0;
  }
  const float cell = kTerrainCell * kMeter, ox = g_t.x0 * kMeter, oz = g_t.z0 * kMeter;
  const int i0 = std::max((int)std::floor((box[0] - ox) / cell), 0);
  const int i1 = std::min((int)std::floor((box[4] - ox) / cell), kTerrainSize - 2);
  const int j0 = std::max((int)std::floor((box[2] - oz) / cell), 0);
  const int j1 = std::min((int)std::floor((box[6] - oz) / cell), kTerrainSize - 2);
  int n = 0;
  // the land past the grid: flat, what of the box is out there
  const float y = g_t.y0 * kMeter;
  if (y >= box[1] && y <= box[5]) {
    const float xa = ox, xb = ox + (kTerrainSize - 1) * cell, za = oz, zb = oz + (kTerrainSize - 1) * cell;
    auto flat = [&](float x0, float z0, float x1, float z1) {
      if (x0 >= x1 || z0 >= z1 || n + 2 > max) {
        return;
      }
      const float q[4][2] = {{x0, z0}, {x0, z1}, {x1, z0}, {x1, z1}};
      const int order[2][3] = {{0, 1, 2}, {2, 1, 3}};
      for (const auto& t : order) {
        float* dst = out + (size_t)n * stride;
        for (int v = 0; v < 3; v++) {
          dst[v * 4] = q[t[v]][0];
          dst[v * 4 + 1] = y;
          dst[v * 4 + 2] = q[t[v]][1];
          dst[v * 4 + 3] = 1.f;
        }
        n++;
      }
    };
    flat(box[0], box[2], std::min(box[4], xa), box[6]);
    flat(std::max(box[0], xb), box[2], box[4], box[6]);
    flat(std::max(box[0], xa), box[2], std::min(box[4], xb), std::min(box[6], za));
    flat(std::max(box[0], xa), std::max(box[2], zb), std::min(box[4], xb), box[6]);
  }
  if (i0 > i1 || j0 > j1) {
    return n;
  }
  // (a wide query, as the camera's, more coarsely: all of it has to fit)
  int step = 1;
  if (!seg) {
    while ((((i1 - i0) / step + 1) * ((j1 - j0) / step + 1)) * 2 > max - n) {
      step *= 2;
    }
  }
  float sx = 0, sz = 0, mx = 0, mz = 0, mm = 0, reach = 0;
  if (seg) {
    sx = seg[0];
    sz = seg[2];
    mx = seg[4];
    mz = seg[6];
    mm = mx * mx + mz * mz;
    reach = seg[3] + cell * 1.5f;
  }
  auto put = [&](int ia, int ja, int ib, int jb, int ic, int jc) {
    float* dst = out + (size_t)n * stride;
    const int p[3][2] = {{ia, ja}, {ib, jb}, {ic, jc}};
    for (int v = 0; v < 3; v++) {
      dst[v * 4] = ox + p[v][0] * cell;
      dst[v * 4 + 1] = height_at(p[v][0], p[v][1]) * kMeter;
      dst[v * 4 + 2] = oz + p[v][1] * cell;
      dst[v * 4 + 3] = 1.f;
    }
    n++;
  };
  // (a line: from its start on, so that what it meets first is what fits)
  const bool back_i = seg && mx < 0, back_j = seg && mz < 0;
  for (int jj = j0; jj <= j1; jj += step) {
    const int j = back_j ? j1 - (jj - j0) : jj;
    const int j2 = std::min(j + step, kTerrainSize - 1);
    for (int ii = i0; ii <= i1; ii += step) {
      const int i = back_i ? i1 - (ii - i0) : ii;
      const int i2 = std::min(i + step, kTerrainSize - 1);
      if (seg) {
        const float cx = ox + (i + 0.5f) * cell, cz = oz + (j + 0.5f) * cell;
        const float k = mm > 0 ? std::clamp(((cx - sx) * mx + (cz - sz) * mz) / mm, 0.f, 1.f) : 0.f;
        const float dx = cx - sx - mx * k, dz = cz - sz - mz * k;
        if (dx * dx + dz * dz > reach * reach) {
          continue;
        }
      }
      const float a = height_at(i, j) * kMeter, b = height_at(i2, j) * kMeter,
                  c = height_at(i, j2) * kMeter, d = height_at(i2, j2) * kMeter;
      if (std::max({a, b, c, d}) < box[1] || std::min({a, b, c, d}) > box[5]) {
        continue;
      }
      if (n + 2 > max) {
        return n;
      }
      put(i, j, i, j2, i2, j);
      put(i2, j, i, j2, i2, j2);
    }
  }
  return n;
}
