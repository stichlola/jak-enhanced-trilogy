#pragma once

// Jak Enhanced: finds frames that come out black (or much darker) for a frame or two, from the log
// instead of from screenshots. Off unless tuning.json sets "debug": {"black_probe": 1}.
//
// Each frame a few stages of the picture are shrunk to 8x8 and read back through a pixel buffer two
// swaps later (no stall): the 3D scene before the post, the post's result, the finished frame
// before it goes to the window, and the window's back buffer right before the swap. A frame whose
// window is much darker than the two before it (and that the game didn't black out itself) is
// logged with the frames around it, one row a frame:
//   [enhanced] probe f=363 scene=0.386/0% post=0.137/12% final=0.163/12% window=0.160/12% | fbo-2176x1360 DARK-FRAME
// (mean brightness 0-1 / share of black samples; "-": that stage didn't run). So is every frame
// that carries a note: a render target made again ("fbo-WxH"), an auto quality step ("auto-N"), a
// frame the game sent no display list for ("no-chain"). The first stage that goes dark is where
// the picture was lost.
//
// "black_probe": 2 also saves the window as it is about to be shown (480 px wide PNGs in
// JAK_ENHANCED_DUMP_DIR) for each noted frame and the three after it: one stall each, tests only.

#include <string>

namespace enh_probe {

enum Stage {
  kScene = 0,  // the game's 3D frame, before the Enhanced post
  kPost,       // after the Enhanced post (composite, TAA)
  kFinal,      // the game's finished frame (HUD drawn), before it goes to the window
  kWindow,     // the window's back buffer, right before the swap
  kStages
};

// (all of it on the render thread)
bool on();
// A stage of this frame: the color of read_fbo's rectangle (0: the window's back buffer).
void sample(Stage stage, unsigned read_fbo, int x, int y, int w, int h);
// Something that happened this frame: its row and the rows around it are logged.
void note(const std::string& what);
// A condition of this frame, shown in its row when the row is logged ("no-camera", "no-scene").
void state(const std::string& what);
// The game blacks the screen out itself this frame (the display's alpha under 1: a load, a fade):
// such a frame isn't reported as a black one.
void blackout(float alpha);
// Right before the swap: reads the frame of two swaps ago back, logs what stands out.
void end_frame(int window_w, int window_h);

}  // namespace enh_probe
