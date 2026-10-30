#pragma once

#include <array>
#include <unordered_map>

#include "common/common_types.h"

struct SharedRenderState;
struct EnhancedCamera;

/*!
 * What the sprite renderer adds to the game's smoke and flames (fx_detail): a 3D noise the puffs
 * drift through, so they evolve instead of turning like a still picture. Sprites are told apart by
 * their texture's name and blend mode; all the others draw as they always did.
 */
class EnhancedSpriteFx {
 public:
  // once per flush_sprites: the shader's samplers (always: a sampler2D and a sampler3D left on the
  // same unit fail every draw) and the frame's uniforms. on = false: every bucket stays untouched
  // (camera: the sprites' world -> screen matrix, 16 floats)
  void begin_flush(SharedRenderState* render_state, bool on, const float* camera);
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

  void copy_depth(SharedRenderState* render_state);

  // the scene's depth as it stands when the sprites draw: their soft edges read it, and the depth
  // buffer they test against can't be read at the same time (the post pass's copy comes later)
  u32 m_depth_fbo = 0;
  u32 m_depth_tex = 0;
  int m_depth_w = 0, m_depth_h = 0;
  u64 m_depth_frame = 0;

  bool m_on = false;
  int m_class_loc = -1;
  std::unordered_map<u64, int> m_classes;  // by GL texture (stable, the VRAM address isn't) + blend
};

/*!
 * Where the ground is under a point, from the depth buffer: a line of points below it is read back
 * into a pixel buffer, answered a frame or so later (no stall). An explosion needs it once: its dust
 * ring, what it throws bounces there, its scorch mark goes there.
 */
class EnhancedGroundProbe {
 public:
  // a ticket for poll (the oldest question is dropped if too many are open)
  int ask(const float* pos,
          const EnhancedCamera& cam,
          const std::array<float, 4>& proj,
          u32 depth_fbo,
          int w,
          int h);
  // false: not yet. true: answered (found: seen, y: its height, world)
  bool poll(int ticket, bool* found, float* y);

 private:
  static constexpr int kSlots = 4;
  static constexpr int kPoints = 12;
  struct Slot {
    u32 pbo = 0;
    void* fence = nullptr;
    int ticket = -1;
    float top[3] = {};
    float view[kPoints][3] = {};
    bool on_screen[kPoints] = {};
    float cam_rot_y[3] = {};  // the world y of each view axis
    float cam_y = 0.f;
    float depth_a = 0.f, depth_b = 0.f;
  };
  Slot m_slots[kSlots];
  int m_next_ticket = 0;
};

// Draws what Jak's gun shots leave behind (g_enhanced_shots), placed like the sprites: leaves the
// sprite shader active. Test switch: JAK_ENHANCED_SHOT_TEST=1: shots of each gun in turn fly ahead of
// Jak every half second and hit the ground.
void enhanced_draw_shots(SharedRenderState* render_state,
                         const float* camera,
                         const float* hvdf_offset,
                         float pfog0);
