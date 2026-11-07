#include "EnhancedTune.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

namespace {

struct TuneState {
  std::mutex mutex;
  std::vector<EnhancedTune*> vars;
  // every leaf of the files, by dotted key, the later file winning
  std::map<std::string, std::vector<float>> file_values;
  // set from the REPL: win over the files until cleared
  std::map<std::string, std::vector<float>> repl_values;
  std::vector<std::string> pending_sets;
  bool loaded = false;
  bool reload_requested = false;
  int frames = 0;
  std::vector<fs::file_time_type> times;
  size_t dumped = 0;
};

TuneState& state() {
  static TuneState s;
  return s;
}

std::atomic<int> g_generation{0};
std::atomic<bool> g_shader_reload{false};

std::vector<fs::path> tune_files() {
  std::vector<fs::path> out;
  const auto root = file_util::get_jak_project_dir();
  out.push_back(root / "game" / "assets" / "enhanced" / "tuning.json");
  const char* inst = std::getenv("JAK_ENHANCED_INSTANCE");
  const int n = inst ? std::atoi(inst) : 0;
  out.push_back(root / "local" /
                (n > 0 ? "tuning_i" + std::to_string(n) + ".json" : "tuning.json"));
  if (const char* env = std::getenv("JAK_ENHANCED_TUNE_FILE"); env && *env) {
    out.push_back(fs::path(env));
  }
  return out;
}

bool to_floats(const nlohmann::json& v, std::vector<float>* out) {
  if (v.is_number()) {
    out->push_back(v.get<float>());
    return true;
  }
  if (v.is_boolean()) {
    out->push_back(v.get<bool>() ? 1.f : 0.f);
    return true;
  }
  if (v.is_array()) {
    for (const auto& e : v) {
      if (!e.is_number()) {
        return false;
      }
      out->push_back(e.get<float>());
    }
    return !out->empty();
  }
  return false;
}

// objects become dotted keys; numbers and arrays of numbers are the values
void flatten(const nlohmann::json& j,
             const std::string& prefix,
             std::map<std::string, std::vector<float>>* out,
             const std::string& file) {
  for (auto it = j.begin(); it != j.end(); ++it) {
    const std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
    if (it.value().is_object()) {
      flatten(it.value(), key, out, file);
      continue;
    }
    std::vector<float> v;
    if (to_floats(it.value(), &v)) {
      (*out)[key] = std::move(v);
    } else if (!it.value().is_string()) {  // (strings: notes)
      lg::warn("[enhanced] tuning {}: '{}' is not a number or a list of numbers", file, key);
    }
  }
}

const std::vector<float>* find_locked(TuneState& s, const std::string& key) {
  if (auto it = s.repl_values.find(key); it != s.repl_values.end()) {
    return &it->second;
  }
  if (auto it = s.file_values.find(key); it != s.file_values.end()) {
    return &it->second;
  }
  return nullptr;
}

std::string show(const std::vector<float>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); i++) {
    out += (i ? " " : "") + fmt::format("{:g}", v[i]);
  }
  return out;
}

// every variable from the files (built-in values for what they leave out); the lock is held
void apply_all_locked() {
  auto& s = state();
  for (auto* t : s.vars) {
    const auto* v = find_locked(s, t->m_key);
    std::vector<float> next = t->m_def;
    if (v) {
      for (size_t i = 0; i < next.size() && i < v->size(); i++) {
        next[i] = (*v)[i];
      }
    }
    if (next != t->m_value) {
      lg::info("[enhanced] tune {} = {}{}", t->m_key, show(next), v ? "" : " (built in)");
    }
    t->m_value = std::move(next);
    t->m_set = v != nullptr;
  }
  g_generation++;
}

void apply_locked(TuneState& s, EnhancedTune* t) {
  if (const auto* v = find_locked(s, t->m_key)) {
    for (size_t i = 0; i < t->m_value.size() && i < v->size(); i++) {
      t->m_value[i] = (*v)[i];
    }
    t->m_set = true;
    lg::info("[enhanced] tune {} = {}", t->m_key, show(t->m_value));
  }
}

void load_files_locked(TuneState& s) {
  std::map<std::string, std::vector<float>> values;
  std::vector<std::string> used;
  for (const auto& path : tune_files()) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      continue;
    }
    try {
      auto j = parse_commented_json(file_util::read_text_file(path), path.string());
      if (!j.is_object()) {
        throw std::runtime_error("not a JSON object");
      }
      flatten(j, "", &values, path.filename().string());
      used.push_back(path.filename().string());
    } catch (const std::exception& e) {
      // (half-saved or wrong: everything stays as it was until the next save)
      lg::error("[enhanced] tuning {} not loaded: {}", path.string(), e.what());
      return;
    }
  }
  s.file_values = std::move(values);
  if (!used.empty() || s.loaded) {
    std::string names;
    for (const auto& u : used) {
      names += (names.empty() ? "" : ", ") + u;
    }
    lg::info("[enhanced] tuning: {} keys from {}", s.file_values.size(),
             names.empty() ? "no file" : names);
  }
}

void dump_locked(TuneState& s) {
  static const char* path = std::getenv("JAK_ENHANCED_TUNE_DUMP");
  if (!path || !*path || s.vars.size() == s.dumped) {
    return;
  }
  s.dumped = s.vars.size();
  nlohmann::json j = nlohmann::json::object();
  for (const auto* t : s.vars) {
    // (nested by the dots, so the dump can be copied into a tuning file as it is)
    nlohmann::json* at = &j;
    std::stringstream ss(t->key());
    std::string part;
    std::vector<std::string> parts;
    while (std::getline(ss, part, '.')) {
      parts.push_back(part);
    }
    for (size_t i = 0; i + 1 < parts.size(); i++) {
      at = &(*at)[parts[i]];
    }
    // (the built-in value: what the code does with no file)
    // (as written in the code: 0.85, not 0.8500000238)
    std::vector<double> d;
    for (int i = 0; i < t->size(); i++) {
      d.push_back(std::stod(fmt::format("{:g}", t->m_def[i])));
    }
    if (d.size() == 1) {
      (*at)[parts.back()] = d[0];
    } else {
      (*at)[parts.back()] = d;
    }
  }
  file_util::write_text_file(fs::path(path), j.dump(2));
}
}  // namespace

EnhancedTune::EnhancedTune(const char* key, float def) : EnhancedTune(key, {def}) {}

EnhancedTune::EnhancedTune(const char* key, std::initializer_list<float> defs)
    : m_key(key), m_def(defs), m_value(defs) {
  if (m_def.empty()) {
    m_def.push_back(0.f);
    m_value.push_back(0.f);
  }
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.vars.push_back(this);
  if (s.loaded) {
    apply_locked(s, this);
  }
}

void enhanced_tune_poll() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  bool changed = false;
  for (const auto& line : s.pending_sets) {
    std::stringstream ss(line);
    std::string key;
    ss >> key;
    if (key.empty()) {
      continue;
    }
    if (key == "*") {
      s.repl_values.clear();
      lg::info("[enhanced] tune: REPL values cleared");
      changed = true;
      continue;
    }
    std::vector<float> v;
    float f;
    while (ss >> f) {
      v.push_back(f);
    }
    if (v.empty()) {
      s.repl_values.erase(key);
    } else {
      s.repl_values[key] = v;
    }
    changed = true;
  }
  s.pending_sets.clear();

  if (!s.loaded || s.reload_requested || ++s.frames >= 60) {
    s.frames = 0;
    const auto files = tune_files();
    std::vector<fs::file_time_type> times;
    for (const auto& f : files) {
      std::error_code ec;
      auto t = fs::last_write_time(f, ec);
      times.push_back(ec ? fs::file_time_type{} : t);
    }
    if (!s.loaded || s.reload_requested || times != s.times) {
      s.times = times;
      load_files_locked(s);
      s.loaded = true;
      s.reload_requested = false;
      changed = true;
    }
    dump_locked(s);
  }
  if (changed) {
    apply_all_locked();
  }
}

int enhanced_tune_generation() {
  return g_generation.load();
}

bool enhanced_tune_lookup(const char* key, float* out, int n) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  const auto* v = find_locked(s, key);
  if (!v) {
    return false;
  }
  for (int i = 0; i < n && i < (int)v->size(); i++) {
    out[i] = (*v)[i];
  }
  return true;
}

void enhanced_request_shader_reload() {
  g_shader_reload = true;
}

bool enhanced_take_shader_reload_request() {
  return g_shader_reload.exchange(false);
}

void enhanced_request_tune_reload() {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.reload_requested = true;
}

void enhanced_request_tune_set(const char* line) {
  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.pending_sets.emplace_back(line);
}
