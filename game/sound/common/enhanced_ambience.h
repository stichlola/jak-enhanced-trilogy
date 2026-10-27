#pragma once

// Enhanced: ambient sound loops (rain, dripping, wind, crickets, birds, frogs, waves, a city) mixed
// on top of the game's own sound. The renderer (EnhancedPost::update_scene) says how loud each
// layer should be from the weather, the zone and the hour; the audio thread fades each one there
// and mixes it in (Player::Tick). The loops are game/assets/enhanced/sounds/<name>.wav, 48 kHz
// stereo 16-bit, made by scripts/enhanced/ambient_sounds.py.

#include "game/sound/common/sound_types.h"

namespace snd::ambience {

enum Layer {
  RainLight,
  RainHeavy,
  Drips,
  WindNight,
  WindStorm,
  Crickets,
  Birds,
  Frogs,
  Waves,
  City,
  kLayers
};

// render thread: how loud each layer should be (0..1, before the volume). The first call loads the
// loops (those missing stay silent)
void set_targets(const float (&gain)[kLayers]);
// the ambience's own volume (the menu's slider, 0..1)
void set_volume(float volume);

// audio thread (Player): the game's sfx volume (0..0x400) and pause (the sfx group paused: the
// pause menu), then add the layers to the game's mix
void set_sfx_volume(int volume);
void set_paused(bool paused);
void mix(s16Output* out, int frames);

}  // namespace snd::ambience
