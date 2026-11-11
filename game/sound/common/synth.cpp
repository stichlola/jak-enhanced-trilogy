// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "synth.h"

#include <stdexcept>

namespace snd {

static s16 ApplyVolume(s16 sample, s32 volume) {
  return (sample * volume) >> 15;
}

s16Output Synth::Tick() {
  s16Output out{};

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  s32 send_l = 0, send_r = 0;
  int sends = 0;
  for (auto& v : mVoices) {
    const s16Output o = v->Run();
    out += o;
    // Enhanced: the reverb's send (enhanced_spatial.h)
    if (v->Send()) {
      send_l += o.left;
      send_r += o.right;
      sends++;
    }
  }
  mSends = sends;

  out.left = ApplyVolume(out.left, mVolume.left.Get());
  out.right = ApplyVolume(out.right, mVolume.right.Get());
  mSendL = (send_l * mVolume.left.Get()) >> 15;
  mSendR = (send_r * mVolume.right.Get()) >> 15;

  mVolume.Run();

  return out;
}

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
  mVoices.emplace_front(voice);
}

void Synth::SetMasterVol(u32 volume) {
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}
}  // namespace snd
