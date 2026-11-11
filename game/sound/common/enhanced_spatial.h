#pragma once

// Enhanced: 3D audio and the area's reverb, on top of 989snd's stereo pan.
// - 3D audio: the game pans each sound effect around the listener (989snd's pan, 0 = ahead, 90 = right,
//   180 = behind, 270 = left), but 989snd's pan table only knows left and right: a sound behind Jak
//   sounds just like one in front. Each sfx voice gets a head shadow instead (Voice::Run): the ear
//   turned away from the sound hears it duller, and sounds from behind are duller in both ears, a
//   cheap stand-in for an HRTF that needs no delay lines.
// - Reverb: the voices of the sound effects (not the music) are sent to one stereo reverb (comb +
//   allpass, Freeverb-like), mixed after the synth (Player::Tick). The renderer picks its preset from
//   the zone (world.json "reverb", EnhancedPost::update_scene): the open desert almost dry, Haven's
//   streets a short slap off the walls, a roof over Jak a small room, sewers, caves, factories and
//   temples a long tail. Presets fade into each other (~1.5 s).
// Switches: the menu's "3D audio" and "Area reverb" (Sound section), JAK_ENHANCED_AUDIO3D=0|1 and
// JAK_ENHANCED_REVERB=0|1 (these win over the saved menu).

#include "game/sound/common/sound_types.h"

namespace snd::spatial {

enum Preset { Dry, Street, Room, Cave, kPresets };

// any thread
void set_audio3d(bool on);
void set_reverb(bool on);
bool audio3d();
bool reverb();
// render thread: the area's preset and how wet (times the preset's own, 0..2)
void set_area(int preset, float wet);
// 989snd pan (degrees, 0 ahead, 90 right) -> head-shadow amounts per ear, Q15 (0 = clear)
void shadow_for_pan(int pan, s32& left, s32& right);

// audio thread: add the reverb of this sample's send to out
void reverb_sample(s16Output& out, s32 send_l, s32 send_r);

// a log line now and then: the preset in use and how many voices send (Player::Tick)
const char* preset_name(int preset);
int current_preset();

}  // namespace snd::spatial
