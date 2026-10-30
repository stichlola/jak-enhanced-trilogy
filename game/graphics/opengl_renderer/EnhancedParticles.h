#pragma once

#include <unordered_map>

#include "common/common_types.h"

struct SharedRenderState;

/*!
 * What the sprite renderer adds to the game's smoke and flames (fx_detail): a 3D noise the puffs
 * drift through, so they evolve instead of turning like a still picture. Sprites are told apart by
 * their texture's name and blend mode; all the others draw as they always did.
 */
class EnhancedSpriteFx {
 public:
  // once per flush_sprites: the shader's samplers (always: a sampler2D and a sampler3D left on the
  // same unit fail every draw) and the frame's uniforms. on = false: every bucket stays untouched
  void begin_flush(SharedRenderState* render_state, bool on);
  // once per bucket, before its draw
  void set_bucket(SharedRenderState* render_state,
                  u32 tbp,
                  u64 gl_texture,
                  bool alpha_blended,
                  bool additive);

 private:
  int classify(SharedRenderState* render_state,
               u32 tbp,
               u64 gl_texture,
               bool alpha_blended,
               bool additive);

  bool m_on = false;
  int m_class_loc = -1;
  std::unordered_map<u64, int> m_classes;  // by GL texture (stable, the VRAM address isn't) + blend
};
