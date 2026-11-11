#include "EnhancedFrameProbe.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/EnhancedTune.h"
#include "game/graphics/pipelines/opengl.h"

namespace enh_probe {
namespace {

constexpr int kN = 8;       // a stage is shrunk to kN x kN
constexpr int kLag = 3;     // buffers per stage: read back kLag - 1 swaps later, never waited for
constexpr int kBefore = 3;  // rows logged before a frame that stands out
constexpr int kAfter = 4;   // and after it

struct Row {
  uint64_t frame = 0;
  bool have[kStages] = {};
  float mean[kStages] = {};   // mean luma, 0-1
  float black[kStages] = {};  // share of the samples under 3/255
  std::array<uint8_t, kN * kN * 3> window = {};  // the window's samples
  std::string notes;          // events: the row is logged
  std::string states;         // conditions: shown with the row
  bool blackout = false;      // the game blacked the screen out itself
  bool logged = false;
};

struct State {
  bool made = false;
  GLuint fbo = 0;
  GLuint tex = 0;
  GLuint pbo[kStages][kLag] = {};
  uint64_t pending[kStages][kLag] = {};  // the frame in the buffer, 0: none
  uint64_t frame = 1;                    // the frame being drawn (one per swap)
  Row cur;                               // its notes so far
  std::deque<Row> rows;                  // the last frames, oldest first
  uint64_t log_until = 0;                // rows up to this frame are logged as they are read back
  int dump_left = 0;                     // (black_probe 2) frames still to save
  int dumped = 0;
};

State& st() {
  static State s;
  return s;
}

const char* kStageNames[kStages] = {"scene", "post", "final", "window"};

// The GL state a blit and a read touch, put back when it goes out of scope: the probe runs between
// the game's renderers, which keep their own state.
struct SavedState {
  GLint read = 0, draw = 0, pbo = 0, align = 4;
  GLboolean scissor = GL_FALSE;
  SavedState() {
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) {
      glDisable(GL_SCISSOR_TEST);
    }
  }
  ~SavedState() {
    if (scissor) {
      glEnable(GL_SCISSOR_TEST);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, align);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw);
  }
};

void make() {
  auto& s = st();
  if (s.made) {
    return;
  }
  s.made = true;
  SavedState saved;
  GLint old_tex = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  glGenFramebuffers(1, &s.fbo);
  glGenTextures(1, &s.tex);
  glBindTexture(GL_TEXTURE_2D, s.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kN, kN, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
  for (auto& stage : s.pbo) {
    glGenBuffers(kLag, stage);
    for (int i = 0; i < kLag; i++) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, stage[i]);
      glBufferData(GL_PIXEL_PACK_BUFFER, kN * kN * 4, nullptr, GL_STREAM_READ);
    }
  }
}

void log_row(Row& r) {
  if (r.logged) {
    return;
  }
  r.logged = true;
  std::string line = fmt::format("[enhanced] probe f={}", r.frame);
  for (int i = 0; i < kStages; i++) {
    if (r.have[i]) {
      line += fmt::format(" {}={:.3f}/{:.0f}%", kStageNames[i], r.mean[i], r.black[i] * 100.f);
    } else {
      line += fmt::format(" {}=-", kStageNames[i]);
    }
  }
  if (!r.notes.empty() || !r.states.empty()) {
    line += " |" + r.states + r.notes;
  }
  lg::info("{}", line);
}

// (black_probe 2) the window as it is about to be shown, 480 px wide
void dump_window(int window_w, int window_h, uint64_t frame, const std::string& notes) {
  const char* dir = std::getenv("JAK_ENHANCED_DUMP_DIR");
  if (!dir || window_w <= 0 || window_h <= 0) {
    return;
  }
  const int w = 480;
  const int h = std::max(1, window_h * w / window_w);
  GLuint fbo = 0, tex = 0;
  GLint old_tex = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
  {
    SavedState saved;
    GLint old_read_buffer = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBlitFramebuffer(0, 0, window_w, window_h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glReadBuffer((GLenum)old_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<uint32_t> px((size_t)w * h), out((size_t)w * h);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        out[(size_t)y * w + x] = px[(size_t)(h - 1 - y) * w + x] | 0xff000000u;
      }
    }
    std::string tag;
    for (char c : notes) {
      tag += (std::isalnum((unsigned char)c) || c == '.') ? c : '_';
    }
    const std::string path = fmt::format("{}/probe_{:06d}{}.png", dir, frame, tag.substr(0, 40));
    file_util::write_rgba_png(path, out.data(), w, h);
  }
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &tex);
}

}  // namespace

bool on() {
  static const EnhancedTune t_probe("debug.black_probe", 0.f);
  return (float)t_probe > 0.5f;
}

void note(const std::string& what) {
  if (on()) {
    st().cur.notes += " " + what;
  }
}

void state(const std::string& what) {
  if (on()) {
    st().cur.states += " " + what;
  }
}

void blackout(float alpha) {
  if (on()) {
    auto& s = st();
    s.cur.blackout = true;
    s.cur.states += fmt::format(" alp-{:.2f}", alpha);
  }
}

void sample(Stage stage, unsigned read_fbo, int x, int y, int w, int h) {
  if (!on() || w <= 0 || h <= 0) {
    return;
  }
  make();
  auto& s = st();
  SavedState saved;
  if (read_fbo != 0) {
    // (a multisampled frame can't be shrunk by a blit: skipped)
    GLint multisampled = 0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, read_fbo);
    glGetIntegerv(GL_SAMPLE_BUFFERS, &multisampled);
    if (multisampled) {
      return;
    }
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
  GLint old_read_buffer = 0;
  if (read_fbo == 0) {
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glReadBuffer(GL_BACK);
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
  glBlitFramebuffer(x, y, x + w, y + h, 0, 0, kN, kN, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  if (read_fbo == 0) {
    glReadBuffer((GLenum)old_read_buffer);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, s.fbo);
  const int slot = (int)(s.frame % kLag);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo[stage][slot]);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, kN, kN, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  s.pending[stage][slot] = s.frame;
}

void end_frame(int window_w, int window_h) {
  static const EnhancedTune t_probe("debug.black_probe", 0.f);
  auto& s = st();
  if (!on()) {
    s.cur = Row();
    return;
  }
  sample(kWindow, 0, 0, 0, window_w, window_h);
  if ((float)t_probe > 1.5f) {
    if (!s.cur.notes.empty()) {
      s.dump_left = 4;
    }
    if (s.dump_left > 0 && s.dumped < 400) {
      dump_window(window_w, window_h, s.frame, s.cur.notes);
      s.dump_left--;
      s.dumped++;
    }
  }

  // this frame's row (its numbers come kLag - 1 swaps later)
  s.cur.frame = s.frame;
  s.rows.push_back(s.cur);
  s.cur = Row();
  while (s.rows.size() > 64) {
    s.rows.pop_front();
  }

  // read back the frame whose buffers come up for reuse next
  const uint64_t ready = s.frame >= (uint64_t)(kLag - 1) ? s.frame - (kLag - 1) : 0;
  Row* r = nullptr;
  Row* prev[3] = {};  // the frames before it: 1, 2, 3 swaps back
  for (auto& row : s.rows) {
    if (row.frame == ready) {
      r = &row;
    } else if (row.frame < ready && row.frame + 3 >= ready) {
      prev[ready - row.frame - 1] = &row;
    }
  }
  if (r) {
    SavedState saved;
    const int slot = (int)(ready % kLag);
    for (int stage = 0; stage < kStages; stage++) {
      if (s.pending[stage][slot] != ready) {
        continue;
      }
      s.pending[stage][slot] = 0;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo[stage][slot]);
      if (auto* d = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, kN * kN * 4,
                                                     GL_MAP_READ_BIT)) {
        float sum = 0.f;
        int black = 0;
        for (int i = 0; i < kN * kN; i++) {
          const float l = (0.3f * d[i * 4] + 0.59f * d[i * 4 + 1] + 0.11f * d[i * 4 + 2]) / 255.f;
          sum += l;
          black += l < 3.f / 255.f;
          if (stage == kWindow) {
            for (int k = 0; k < 3; k++) {
              r->window[i * 3 + k] = d[i * 4 + k];
            }
          }
        }
        r->have[stage] = true;
        r->mean[stage] = sum / (kN * kN);
        r->black[stage] = (float)black / (kN * kN);
      }
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }

    // a frame the game sent nothing for: which earlier frame the window shows (the samples'
    // summed difference to the windows 1, 2 and 3 swaps back: 0 = that one)
    if (r->have[kWindow] && r->notes.find("no-chain") != std::string::npos) {
      r->notes += " window-vs-f-1/2/3:";
      for (int k = 0; k < 3; k++) {
        int diff = -1;
        if (prev[k] && prev[k]->have[kWindow]) {
          diff = 0;
          for (size_t i = 0; i < r->window.size(); i++) {
            diff += std::abs((int)r->window[i] - (int)prev[k]->window[i]);
          }
        }
        r->notes += fmt::format("{}{}", k ? "/" : "", diff);
      }
    }
  }

  // the frame before the one just read, now that the frame after it is known: a window much
  // darker than both its neighbours is a flash (DARK-FRAME, BLACK-FRAME when nearly all of it is
  // black); darker than the two frames before it and still dark after is a cut or a fade
  // (DARK-CUT, for a look). Not the frames the game blacked out itself.
  Row* c = prev[0];
  const Row* b1 = prev[1];
  const Row* b2 = prev[2];
  if (r && c && b1 && c->have[kWindow] && b1->have[kWindow] && r->have[kWindow] && !c->blackout) {
    const float l = c->mean[kWindow];
    const float around = std::min(b1->mean[kWindow], r->mean[kWindow]);
    const bool dip = l < 0.5f * around && around - l > 0.03f;
    const bool black_dip = c->black[kWindow] > 0.9f && b1->black[kWindow] < 0.6f &&
                           r->black[kWindow] < 0.6f;
    if (dip || black_dip) {
      c->notes += c->black[kWindow] > 0.9f ? " BLACK-FRAME" : " DARK-FRAME";
    } else if (b2 && b2->have[kWindow]) {
      const float before = std::min(b1->mean[kWindow], b2->mean[kWindow]);
      if (l < 0.5f * before && before - l > 0.03f) {
        c->notes += " DARK-CUT";
      }
    }
  }
  // the rows go to the log one frame late (c: its notes are final now), in order, each once: a row
  // with a note, the kBefore rows before it and the kAfter after it
  if (c) {
    if (!c->notes.empty()) {
      s.log_until = std::max(s.log_until, c->frame + kAfter);
      for (auto& row : s.rows) {
        if (row.frame + kBefore >= c->frame && row.frame < c->frame) {
          log_row(row);
        }
      }
    }
    if (c->frame <= s.log_until) {
      log_row(*c);
    }
  }
  s.frame++;
}

}  // namespace enh_probe
