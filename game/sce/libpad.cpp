#include "libpad.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/display.h"
#include "game/graphics/enhanced_state.h"
#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/EnhancedPost.h"
#include "game/graphics/opengl_renderer/EnhancedTune.h"
#include "game/kernel/common/kernel_types.h"
#include "game/system/hid/input_bindings.h"

/*!
 * @file libpad.cpp
 * Stub implementation of the EE pad (controller) library
 */

namespace ee {

int scePadPortOpen(int port, int slot, void*) {
  // we are expected to return a non-zero file descriptor.
  // we return the port + 1 and succeed always.
  // the game just opens this once at the beginning, we don't have to implement closing/reopening.
  ASSERT(slot == 0);
  return port + 1;
}

int scePadGetState(int /*port*/, int /*slot*/) {
  // pretend we always have a controller connected
  return scePadStateStable;
}

// controller mode array for DS2s
// the game will try very hard to force into dualshock mode so let's not even try
static const int libpad_DualShock2_ModeIDs[2] = {
    (int)PadMode::Controller,  // no vibration or pressure sensitive buttons
    (int)PadMode::DualShock2   // vibration + pressure sensitive buttons
};
int scePadInfoMode(int /*port*/, int /*slot*/, int term, int offs) {
  if (term == InfoModeCurExID) {
    // return vibration mode ID. that's just dualshock mode.
    return libpad_DualShock2_ModeIDs[1];

  } else if (term == InfoModeCurID) {
    // return mode ID. just dualshock.
    return libpad_DualShock2_ModeIDs[1];

  } else if (term == InfoModeCurExOffs) {
    // offset of current mode ID
    return 1;

  } else if (term == InfoModeIdTable) {
    if (offs == -1)
      return 2;

    return libpad_DualShock2_ModeIDs[offs];  // ?
  }

  // invalid controller or some other error
  return 0;
}

// Enhanced test switch: pad 0 driven from the tuning file, for automated runs that really travel
// (drive across the desert, walk into a district) instead of teleporting, so levels stream in as in
// play. Unset keys: the real pad, untouched.
//   debug.pad = [left x, left y, right x, right y, buttons]: sticks -1..1 (left y -1 = forward),
//     buttons a PadData::ButtonIndex bit mask (CROSS 16384, TRIANGLE 4096, SQUARE 32768...), their
//     pressure full (the cars' throttle is the X button's pressure)
//   debug.route = [x0, z0, x1, z1, ...] (meters): the left stick steers Jak toward each point in
//     turn, from the camera's view (on foot the stick is camera-relative; in a car the camera sits
//     behind it); debug.pad's buttons are held meanwhile (X = gas). Reached within
//     debug.route_reach m; after the last point the stick is let go. debug.route_mode 0 on foot
//     (stuck for 1.5 s: a jump), 1 vehicle (steering only). Each point reached and each stuck second
//     goes to the log ("[enhanced] route ...")
namespace {
struct TestPad {
  bool on = false;
  int lx = 127, ly = 127, rx = 127, ry = 127;
  u32 buttons = 0;
};

TestPad enhanced_test_pad() {
  static const EnhancedTune t_pad("debug.pad", {0.f, 0.f, 0.f, 0.f, 0.f});
  // (a key holds as many values as its built-in list: room for 24 points, unused ones -1e9)
  static const EnhancedTune t_route(
      "debug.route", {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f,
                      -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f,
                      -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f,
                      -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f,
                      -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f});
  static const EnhancedTune t_reach("debug.route_reach", 10.f);
  static const EnhancedTune t_mode("debug.route_mode", 0.f);
  TestPad out;
  const auto stick = [](float v) { return std::clamp((int)std::lround(127.5f + 127.5f * v), 0, 255); };
  if (t_pad.is_set() && t_pad.size() >= 4) {
    out.on = true;
    out.lx = stick(t_pad[0]);
    out.ly = stick(t_pad[1]);
    out.rx = stick(t_pad[2]);
    out.ry = stick(t_pad[3]);
    out.buttons = t_pad.size() >= 5 ? (u32)std::max(0.f, (float)t_pad[4]) : 0u;
  }
  using clock = std::chrono::steady_clock;
  static int next = 0;
  static std::vector<float> asked;
  static clock::time_point last_move = clock::now(), last_log = clock::now(), jump_until{};
  static float last_pos[2] = {0.f, 0.f};
  if (!t_route.is_set() || t_route.size() < 2) {
    asked.clear();
    return out;
  }
  std::vector<float> now_route;
  for (int i = 0; i + 1 < t_route.size() && t_route[i] > -1e8f && t_route[i + 1] > -1e8f; i += 2) {
    now_route.push_back(t_route[i]);
    now_route.push_back(t_route[i + 1]);
  }
  if (now_route.empty()) {
    asked.clear();
    return out;
  }
  if (now_route != asked) {
    asked = now_route;
    next = 0;
    last_move = clock::now();
    lg::info("[enhanced] route of {} points, mode {}", (int)asked.size() / 2, (int)t_mode);
  }
  out.on = true;
  const auto& pl = g_enhanced_player;
  const auto& cam = g_enhanced_camera;
  // (the camera as the renderer last saw it: its valid flag only lasts within a frame)
  const bool cam_seen = cam.trans[0] != 0.f || cam.trans[1] != 0.f || cam.trans[2] != 0.f;
  if (!pl.valid || !cam_seen || next >= (int)asked.size() / 2) {
    out.lx = out.ly = 127;
    return out;
  }
  const float m = 4096.f;
  const float px = pl.pos[0] / m, pz = pl.pos[2] / m;
  float dx = asked[next * 2] - px, dz = asked[next * 2 + 1] - pz;
  const auto t = clock::now();
  if (std::sqrt(dx * dx + dz * dz) < (float)t_reach) {
    lg::info("[enhanced] route point {} reached at {:.0f} {:.0f} m", next, px, pz);
    next++;
    if (next >= (int)asked.size() / 2) {
      lg::info("[enhanced] route done");
      out.lx = out.ly = 127;
      return out;
    }
    dx = asked[next * 2] - px;
    dz = asked[next * 2 + 1] - pz;
  }
  // the camera's forward on the ground: from it to Jak; its right: the view's x axis
  float fx = pl.pos[0] - cam.trans[0], fz = pl.pos[2] - cam.trans[2];
  const float fl = std::sqrt(fx * fx + fz * fz);
  if (fl < 1.f) {
    fx = cam.rot[0][2];
    fz = cam.rot[2][2];
  } else {
    fx /= fl;
    fz /= fl;
  }
  // (the view's x axis points to the screen's left in the game's handedness: a first run steered
  // the car away from its point; debug.route_flip 1: the other side)
  static const EnhancedTune t_flip("debug.route_flip", 0.f);
  const float rs = (float)t_flip > 0.5f ? 1.f : -1.f;
  const float rgx = rs * cam.rot[0][0], rgz = rs * cam.rot[2][0];
  const float dl = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
  const float ux = dx / dl, uz = dz / dl;
  float err = std::atan2(ux * rgx + uz * rgz, ux * fx + uz * fz);  // + = to the right
  // stuck (no meter in 1.5 s): a way round, 1.5 s at a time, sides taken in turn: on foot 70 deg
  // off (with a jump), a vehicle backs up (Square, the brake, reverses) steering the other way
  static clock::time_point detour_until{};
  static float detour_side = 1.f;
  const double still_s = std::chrono::duration<double>(t - last_move).count();
  if (still_s > 1.5 && t > detour_until + std::chrono::milliseconds(500)) {
    detour_until = t + std::chrono::milliseconds(1500);
    detour_side = -detour_side;
  }
  const bool detour = t < detour_until;
  if (detour && (int)t_mode == 1) {
    out.buttons = (out.buttons & ~(1u << PadData::CROSS)) | (1u << PadData::SQUARE);
    err = -detour_side * 1.f;
  } else if (detour) {
    err += detour_side * 1.22f;
  }
  if ((int)t_mode == 1) {
    out.lx = stick(std::clamp(err / 0.5f, -1.f, 1.f));
    out.ly = 127;
  } else {
    out.lx = stick(std::sin(err));
    out.ly = stick(-std::cos(err));
  }
  const float moved = std::hypot(px - last_pos[0], pz - last_pos[1]);
  if (moved > 1.f) {
    last_move = t;
    last_pos[0] = px;
    last_pos[1] = pz;
  }
  const double still = std::chrono::duration<double>(t - last_move).count();
  if ((int)t_mode == 0 && still > 1.5 && t > jump_until + std::chrono::milliseconds(700)) {
    jump_until = t + std::chrono::milliseconds(250);
  }
  if (t < jump_until) {
    out.buttons |= 1u << PadData::CROSS;
  }
  if (std::chrono::duration<double>(t - last_log).count() >= 1.0) {
    last_log = t;
    lg::info("[enhanced] route to {} at {:.0f} {:.0f} m, {:.0f} m off, turn {:.0f} deg{}", next, px,
             pz, dl, err * 57.2958f, still > 1.5 ? ", stuck" : "");
  }
  return out;
}
}  // namespace

// reads controller data and writes it to a buffer in rdata (must be at least 32 bytes large).
// returns buffer size (32) or 0 on error.
int scePadRead(int port, int /*slot*/, u8* rdata) {
  auto cpad = (CPadInfo*)(rdata);

  cpad->valid = 0;  // success

  cpad->status = 0x70 /* (dualshock2) */ | (20 / 2); /* (dualshock2 data size) */

  std::optional<std::shared_ptr<PadData>> pad_data = std::nullopt;
  if (Display::GetMainDisplay()) {
    if (Gfx::g_debug_settings.treat_pad0_as_pad1) {
      if (port == 0) {
        pad_data = Display::GetMainDisplay()->get_input_manager()->get_current_data(1);
      } else {
        pad_data = Display::GetMainDisplay()->get_input_manager()->get_current_data(0);
      }
    } else {
      pad_data = Display::GetMainDisplay()->get_input_manager()->get_current_data(port);
    }
  }

  if (pad_data) {
    std::tie(cpad->rightx, cpad->righty) = pad_data.value()->analog_right();
    std::tie(cpad->leftx, cpad->lefty) = pad_data.value()->analog_left();

    // pressure sensitivity, ignored for almost all controllers
    for (size_t i = 0; i < pad_data.value()->pressure_data.size(); i++) {
      cpad->abutton[i] = pad_data.value()->pressure_data.at(i);
    }

    cpad->button0 = 0;
    for (size_t i = 0; i < pad_data.value()->button_data.size(); i++) {
      cpad->button0 |= pad_data.value()->button_data.at(i) << i;
    }
  }

  if (port == 0) {
    const TestPad tp = enhanced_test_pad();
    if (tp.on) {
      cpad->leftx = (u8)tp.lx;
      cpad->lefty = (u8)tp.ly;
      cpad->rightx = (u8)tp.rx;
      cpad->righty = (u8)tp.ry;
      cpad->button0 = (u16)tp.buttons;
      // (PadData::PressureIndex order: right left up down triangle circle cross square l1 r1 l2 r2)
      static const int kPressureButton[12] = {
          PadData::DPAD_RIGHT, PadData::DPAD_LEFT, PadData::DPAD_UP, PadData::DPAD_DOWN,
          PadData::TRIANGLE,   PadData::CIRCLE,    PadData::CROSS,   PadData::SQUARE,
          PadData::L1,         PadData::R1,        PadData::L2,      PadData::R2};
      for (int i = 0; i < 12; i++) {
        cpad->abutton[i] = (tp.buttons >> kPressureButton[i]) & 1 ? 255 : 0;
      }
    }
  }

  return 32;
}

int scePadSetActDirect(int port, int /*slot*/, const u8* data) {
  // offsets are set by scePadSetActAlign, but we already know the game uses 0 for big motor and 1
  // for small motor
  // also, the "slow" motor corresponds to the "large" motor on the PS2
  if (Display::GetMainDisplay()) {
    if (!Display::GetMainDisplay()->get_input_manager()->controller_has_rumble(port)) {
      return 0;
    }
    Display::GetMainDisplay()->get_input_manager()->enqueue_update_rumble(port, data[1],
                                                                          data[0] * 255);
    return 1;  // TODO - assuming 1 means rumble is supported, and 0 is no?
  }
  return 0;
}

int scePadSetActAlign(int /*port*/, int /*slot*/, const u8* /*data*/) {
  return 1;
}

// we also don't care
int scePadSetMainMode(int /*port*/, int /*slot*/, int /*offs*/, int /*lock*/) {
  return 1;
}

// async pad functions are gonna be synchronous so this always succeeds
int scePadGetReqState(int /*port*/, int /*slot*/) {
  return scePadReqStateComplete;
}

int scePadInfoAct(int /*port*/, int /*slot*/, int actno, int term) {
  if (actno == -1)
    return 2;  // i think?
  if (actno < 2) {
    if (term == InfoActSub) {
      return 1;  // whatever
    } else if (term == InfoActFunc) {
      return 1;  // whatever
    } else if (term == InfoActSize) {
      return 0;  // whatever
    } else if (term == InfoActCurr) {
      return 1;  // whatever
    }
  }
  return 0;
}

int scePadInfoPressMode(int /*port*/, int /*slot*/) {
  return 0;  // we do NOT support pressure sensitive buttons right now
}

int scePadEnterPressMode(int /*port*/, int /*slot*/) {
  return 1;  // we dont support pressure button, but if we did this would work straight away
}

}  // namespace ee
