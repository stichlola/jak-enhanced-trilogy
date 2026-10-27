#include "enhanced_ambience.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

namespace snd::ambience {
namespace {

const char* const kNames[kLayers] = {"rain_light", "rain_heavy", "drips",
                                     "wind_night", "wind_storm", "crickets"};
// seconds to fade a layer all the way in or out
constexpr float kFade = 2.5f;
// how loud the loops sit under the game's own sound
constexpr float kMix = 0.55f;

struct Loop {
  std::vector<s16> samples;  // interleaved stereo
  size_t pos = 0;            // in frames
  float gain = 0.f;          // where the fade is (audio thread)
};

Loop g_loops[kLayers];
std::atomic<bool> g_ready{false};
std::atomic<float> g_target[kLayers] = {};
std::atomic<float> g_volume{0.7f};
std::atomic<int> g_sfx_volume{0x400};
std::atomic<bool> g_paused{false};

// the samples of a 48 kHz stereo 16-bit WAV (empty when it isn't one)
std::vector<s16> read_wav(const fs::path& path) {
  std::vector<s16> out;
  if (!fs::exists(path)) {
    return out;
  }
  const auto data = file_util::read_binary_file(path);
  if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
      std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
    lg::error("[enhanced] {} is not a WAV file", path.string());
    return out;
  }
  bool format_ok = false;
  size_t at = 12;
  while (at + 8 <= data.size()) {
    u32 size;
    std::memcpy(&size, data.data() + at + 4, 4);
    const u8* chunk = data.data() + at + 8;
    if (std::memcmp(data.data() + at, "fmt ", 4) == 0 && size >= 16) {
      u16 format, channels, bits;
      u32 rate;
      std::memcpy(&format, chunk, 2);
      std::memcpy(&channels, chunk + 2, 2);
      std::memcpy(&rate, chunk + 4, 4);
      std::memcpy(&bits, chunk + 14, 2);
      format_ok = format == 1 && channels == 2 && rate == 48000 && bits == 16;
    } else if (std::memcmp(data.data() + at, "data", 4) == 0 && format_ok) {
      size = std::min<size_t>(size, data.size() - at - 8);
      out.resize(size / 2 / 2 * 2);
      std::memcpy(out.data(), chunk, out.size() * 2);
      return out;
    }
    at += 8 + size + (size & 1);
  }
  lg::error("[enhanced] {}: not 48 kHz stereo 16-bit", path.string());
  return out;
}

void load() {
  const auto dir = file_util::get_jak_project_dir() / "game" / "assets" / "enhanced" / "sounds";
  for (int i = 0; i < kLayers; i++) {
    g_loops[i].samples = read_wav(dir / (std::string(kNames[i]) + ".wav"));
    // the layers start apart, so two loops of the same length don't repeat together
    g_loops[i].pos = g_loops[i].samples.empty() ? 0 : (i * 48000 * 3) % (g_loops[i].samples.size() / 2);
  }
  int count = 0;
  for (auto& l : g_loops) {
    count += !l.samples.empty();
  }
  lg::info("[enhanced] ambient sounds: {} of {} loops", count, (int)kLayers);
  g_ready = true;
}

}  // namespace

void set_targets(const float (&gain)[kLayers]) {
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    load();
  }
  for (int i = 0; i < kLayers; i++) {
    g_target[i] = std::clamp(gain[i], 0.f, 1.f);
  }
}

void set_volume(float volume) {
  g_volume = std::clamp(volume, 0.f, 1.f);
}

void set_sfx_volume(int volume) {
  g_sfx_volume = volume;
}

void set_paused(bool paused) {
  g_paused = paused;
}

void mix(s16Output* out, int frames) {
  if (!g_ready || frames <= 0) {
    return;
  }
  const float master = g_paused ? 0.f
                                : kMix * g_volume.load() *
                                      std::clamp(g_sfx_volume.load() / 1024.f, 0.f, 1.f);
  const float step = frames / (48000.f * kFade);
  for (int i = 0; i < kLayers; i++) {
    Loop& l = g_loops[i];
    const float want = g_target[i].load() * master;
    const float g0 = l.gain;
    const float g1 = g0 + std::clamp(want - g0, -step, step);
    l.gain = g1;
    const size_t len = l.samples.size() / 2;
    if (len == 0 || (g0 <= 0.f && g1 <= 0.f)) {
      continue;
    }
    for (int f = 0; f < frames; f++) {
      const float g = g0 + (g1 - g0) * (float)f / (float)frames;
      const s16* s = &l.samples[l.pos * 2];
      out[f] += s16Output{(s16)(s[0] * g), (s16)(s[1] * g)};
      if (++l.pos >= len) {
        l.pos = 0;
      }
    }
  }
}

}  // namespace snd::ambience
