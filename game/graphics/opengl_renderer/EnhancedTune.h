#pragma once

// Enhanced dev loop: numbers that are tuned often, read from a JSON file that the renderer reads
// again when it is saved, so a look change needs no rebuild and no restart.
//
// Files, later ones winning (a missing file or key keeps the number written in the code, so with
// no file the game is exactly as built):
//   game/assets/enhanced/tuning.json: shared, committed tweaks (optional)
//   local/tuning.json: your own, not committed (with JAK_ENHANCED_INSTANCE=n, tuning_i<n>.json)
//   $JAK_ENHANCED_TUNE_FILE: any file (a test's variants)
//   (pc-enhanced-getenv "JAK_ENHANCED_DO_TUNE <key> <values>" "" 0) from the REPL: wins until
//   cleared ("JAK_ENHANCED_DO_TUNE <key>" for one, "JAK_ENHANCED_DO_TUNE *" for all); a goalc
//   that hasn't run (mi) needs (define-extern pc-enhanced-getenv (function string string int
//   symbol)) in the same form
// Keys are dotted paths ("shadow.far_range_m"), as nested objects or written flat; a key's value
// is a number or, for a table, an array. Comments (// and /* */) are allowed.
// JAK_ENHANCED_TUNE_DUMP=<file> writes every key the game has asked for, with its built-in value.
//
// In code: a static per place, read like a float (it costs a load, the file is polled once a
// second):
//   static const EnhancedTune k_far("shadow.far_range_m", 1200.f);
//   make_mvp(k_far * kMeter, ...);
//   static const EnhancedTune k_scale("auto.render_scale", {1.f, 1.f, 1.f, 1.f, 0.85f, 0.72f});
//   k_scale[level]
// In a shader: `uniform float tune_<name> = <value>;` (or vec2/3/4, int) takes
// "shaders.tune_<name>" from the file when it is there (Shader::apply_tuning), and its own value
// otherwise.

#include <initializer_list>
#include <vector>

class EnhancedTune {
 public:
  EnhancedTune(const char* key, float def);
  EnhancedTune(const char* key, std::initializer_list<float> defs);
  EnhancedTune(const EnhancedTune&) = delete;
  EnhancedTune& operator=(const EnhancedTune&) = delete;

  operator float() const { return m_value[0]; }
  float operator[](int i) const {
    return m_value[i < 0 ? 0 : (i >= (int)m_value.size() ? (int)m_value.size() - 1 : i)];
  }
  int size() const { return (int)m_value.size(); }
  const char* key() const { return m_key; }
  // whether the files set it (otherwise it holds the built-in value)
  bool is_set() const { return m_set; }

  // (internal: EnhancedTune.cpp sets them)
  const char* m_key;
  std::vector<float> m_def;
  std::vector<float> m_value;
  bool m_set = false;
};

// Once a frame (EnhancedPost::begin_frame): reads the files again when one was saved, applies the
// REPL's requests. Cheap: it looks at the files' times once a second.
void enhanced_tune_poll();
// Bumped each time the values may have changed (shaders re-send their tune_ uniforms then).
int enhanced_tune_generation();
// A value by key for code that has no static (shaders' tune_ uniforms): false when no file sets it.
bool enhanced_tune_lookup(const char* key, float* out, int n);

// Requests from any thread (the REPL through pc-enhanced-getenv, the F5 key); done on the render
// thread at the start of the next frame.
void enhanced_request_shader_reload();
bool enhanced_take_shader_reload_request();
void enhanced_request_tune_reload();
// "<key> <v0> [v1 ...]": sets a key over the files; "<key>" alone clears it, "*" clears all.
void enhanced_request_tune_set(const char* line);
