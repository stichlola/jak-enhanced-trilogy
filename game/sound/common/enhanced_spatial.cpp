// Enhanced: 3D audio (head shadow) and the area's reverb. See enhanced_spatial.h.
#include "enhanced_spatial.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace snd::spatial {
namespace {

bool env_flag(const char* name, bool def) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    return def;
  }
  return std::atoi(v) != 0;
}

std::atomic<bool> g_audio3d = env_flag("JAK_ENHANCED_AUDIO3D", true);
std::atomic<bool> g_reverb = env_flag("JAK_ENHANCED_REVERB", true);
const bool g_audio3d_env = std::getenv("JAK_ENHANCED_AUDIO3D") != nullptr;
const bool g_reverb_env = std::getenv("JAK_ENHANCED_REVERB") != nullptr;
std::atomic<int> g_preset = Dry;
std::atomic<float> g_wet = 1.f;

constexpr float kFs = 48000.f;

// wet: send level into the tail; t60: seconds for the tail to fall 60 dB; damp: how fast the
// highs die in the tail (0..1); pre: ms before the tail; early/early_ms: one discrete echo (the
// slap off a wall across the street)
struct Params {
  float wet, t60, damp, pre, early, early_ms;
};
constexpr Params kParams[kPresets] = {
    {0.05f, 0.35f, 0.70f, 8.f, 0.00f, 30.f},   // Dry: the open desert, the sea
    {0.10f, 0.75f, 0.45f, 12.f, 0.30f, 70.f},  // Street: Haven, Spargus' alleys
    {0.16f, 0.90f, 0.50f, 6.f, 0.18f, 16.f},   // Room: a roof over Jak, small halls
    {0.28f, 2.60f, 0.25f, 25.f, 0.14f, 45.f},  // Cave: sewers, mines, factory, temples, the nest
};

constexpr int kCombs = 6;
constexpr int kCombLen[kCombs] = {1215, 1293, 1390, 1476, 1548, 1623};
constexpr int kAllLen[2] = {605, 480};
constexpr int kSpread = 23;
constexpr int kPreMax = 4800;  // 100 ms

struct Line {
  std::vector<float> buf;
  int pos = 0;
  void init(int n) { buf.assign(n, 0.f); }
};

struct Reverb {
  Line comb[2][kCombs];
  float comb_lp[2][kCombs] = {};
  Line all[2][2];
  Line pre[2];
  Params cur = kParams[Dry];
  float cur_wet_scale = 1.f;
  float g[kCombs] = {};
  int tick = 0;
  bool ready = false;
  float level = 0.f;  // fades in and out with the switch

  void init() {
    for (int c = 0; c < 2; c++) {
      for (int i = 0; i < kCombs; i++) {
        comb[c][i].init(kCombLen[i] + c * kSpread);
      }
      for (int i = 0; i < 2; i++) {
        all[c][i].init(kAllLen[i] + c * kSpread);
      }
      pre[c].init(kPreMax);
    }
    update_gains();
    ready = true;
  }
  void update_gains() {
    for (int i = 0; i < kCombs; i++) {
      g[i] = std::pow(10.f, -3.f * kCombLen[i] / (std::max(cur.t60, 0.05f) * kFs));
    }
  }
  // the parameters ease toward the area's preset (every 64 samples, ~1.5 s to get there)
  void ease() {
    const Params& t = kParams[std::clamp(g_preset.load(), 0, (int)kPresets - 1)];
    const float k = 64.f / (1.5f * kFs) * 3.f;
    auto e = [k](float& a, float b) { a += (b - a) * std::min(1.f, k); };
    e(cur.wet, t.wet);
    e(cur.t60, t.t60);
    e(cur.damp, t.damp);
    e(cur.pre, t.pre);
    e(cur.early, t.early);
    e(cur.early_ms, t.early_ms);
    e(cur_wet_scale, std::clamp(g_wet.load(), 0.f, 2.f));
    e(level, g_reverb ? 1.f : 0.f);
    update_gains();
  }
  static float read_back(const Line& l, int samples) {
    const int n = (int)l.buf.size();
    int p = l.pos - samples;
    while (p < 0) {
      p += n;
    }
    return l.buf[p % n];
  }
};

Reverb g_rev;

}  // namespace

void set_audio3d(bool on) {
  if (!g_audio3d_env) {
    g_audio3d = on;
  }
}
void set_reverb(bool on) {
  if (!g_reverb_env) {
    g_reverb = on;
  }
}
bool audio3d() {
  return g_audio3d;
}
bool reverb() {
  return g_reverb;
}
void set_area(int preset, float wet) {
  g_preset = std::clamp(preset, 0, (int)kPresets - 1);
  g_wet = wet;
}

int current_preset() {
  return g_preset;
}

const char* preset_name(int preset) {
  static const char* names[kPresets] = {"dry", "street", "room", "cave"};
  return names[std::clamp(preset, 0, (int)kPresets - 1)];
}

void shadow_for_pan(int pan, s32& left, s32& right) {
  if (pan < 0 || !g_audio3d) {
    left = right = 0;
    return;
  }
  const float a = (float)(pan % 360) * 3.14159265f / 180.f;
  const float rear = std::max(0.f, -std::cos(a));  // 1 straight behind
  const float side = std::sin(a);                   // +1 right, -1 left
  // the far ear in the head's shadow, both ears duller for what's behind
  const float kl = std::min(0.85f, 0.45f * rear + 0.40f * std::max(0.f, side));
  const float kr = std::min(0.85f, 0.45f * rear + 0.40f * std::max(0.f, -side));
  left = (s32)(kl * 32767.f);
  right = (s32)(kr * 32767.f);
}

void reverb_sample(s16Output& out, s32 send_l, s32 send_r) {
  Reverb& r = g_rev;
  if (!r.ready) {
    r.init();
  }
  if ((r.tick++ & 63) == 0) {
    r.ease();
  }
  if (r.level < 0.001f) {
    return;
  }
  const float in[2] = {send_l / 32768.f, send_r / 32768.f};
  float o[2];
  const int pre_n = std::clamp((int)(r.cur.pre * kFs / 1000.f), 1, kPreMax - 1);
  const int early_n = std::clamp((int)(r.cur.early_ms * kFs / 1000.f), 1, kPreMax - 1);
  for (int c = 0; c < 2; c++) {
    Line& pl = r.pre[c];
    pl.buf[pl.pos] = in[c];
    const float x = Reverb::read_back(pl, pre_n) * 0.5f;
    const float slap = Reverb::read_back(pl, early_n);
    pl.pos = (pl.pos + 1) % kPreMax;
    float acc = 0.f;
    for (int i = 0; i < kCombs; i++) {
      Line& l = r.comb[c][i];
      const float y = l.buf[l.pos];
      r.comb_lp[c][i] = y * (1.f - r.cur.damp) + r.comb_lp[c][i] * r.cur.damp;
      l.buf[l.pos] = x + r.comb_lp[c][i] * r.g[i];
      l.pos = (l.pos + 1) % (int)l.buf.size();
      acc += y;
    }
    acc *= 1.f / kCombs;
    for (int i = 0; i < 2; i++) {
      Line& l = r.all[c][i];
      const float b = l.buf[l.pos];
      const float y = b - acc;
      l.buf[l.pos] = acc + b * 0.5f;
      l.pos = (l.pos + 1) % (int)l.buf.size();
      acc = y;
    }
    o[c] = (acc * r.cur.wet * 2.f + slap * r.cur.early * r.cur.wet) * r.cur_wet_scale * r.level;
  }
  auto add = [](s16 a, float b) {
    return (s16)std::clamp<s32>((s32)a + (s32)(b * 32767.f), INT16_MIN, INT16_MAX);
  };
  out.left = add(out.left, o[0]);
  out.right = add(out.right, o[1]);
}

}  // namespace snd::spatial
