#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "fmt/color.h"
#ifdef _WIN32  // see lg::initialize
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/string_util.h"

namespace lg {
struct Logger {
  Logger() = default;

  bool initialized = false;
  FILE* fp = nullptr;
  level stdout_log_level = level::trace;
  level file_log_level = level::trace;
  level flush_level = level::trace;
  std::mutex mutex;
  bool disable_colors = false;
  bool file_muted = false;

  ~Logger() {
    // will run when program exits.
    if (fp) {
      fclose(fp);
    }
  }
};

Logger gLogger;

namespace internal {
/*!
 * Enhanced: the console only takes valid UTF-8 (on Windows fmt converts what it prints for the
 * console and throws on anything else, which closed the game): stray bytes, from a driver's message
 * or the game's own text, are printed as '?'. The log file keeps them as they are.
 */
std::string console_safe(const char* message) {
  std::string out;
  const auto* s = reinterpret_cast<const unsigned char*>(message);
  while (*s) {
    const unsigned char c = *s;
    const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
    bool ok = len > 0;
    for (int i = 1; i < len && ok; i++) {
      ok = (s[i] & 0xc0) == 0x80;
    }
    if (ok) {
      out.append(reinterpret_cast<const char*>(s), len);
      s += len;
    } else {
      out.push_back('?');
      s++;
    }
  }
  return out;
}

/*!
 * Enhanced: the log file without the console's colours (goalc colours its errors and warnings;
 * ESC [ ... m in a file only gets in the way of reading and grepping it).
 */
void write_file(const char* message, size_t len) {
  const char* esc = static_cast<const char*>(memchr(message, '\x1b', len));
  if (!esc) {
    fwrite(message, len, 1, gLogger.fp);
    return;
  }
  std::string out;
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    if (message[i] == '\x1b' && i + 1 < len && message[i + 1] == '[') {
      size_t j = i + 2;
      while (j < len && ((message[j] >= '0' && message[j] <= '9') || message[j] == ';' ||
                         message[j] == '?')) {
        j++;
      }
      if (j < len && ((message[j] >= 'A' && message[j] <= 'Z') ||
                      (message[j] >= 'a' && message[j] <= 'z'))) {
        i = j;
        continue;
      }
    }
    out.push_back(message[i]);
  }
  fwrite(out.data(), out.size(), 1, gLogger.fp);
}

const char* log_level_names[] = {"trace", "debug", "info", "warn", "error", "die", "die"};
const fmt::color log_colors[] = {
    fmt::color::gray, fmt::color::turquoise, fmt::color::light_green, fmt::color::yellow,
    fmt::color::red,  fmt::color::hot_pink,  fmt::color::hot_pink};

void log_message(level log_level, LogTime& now, const char* message) {
#ifdef __linux__
  char date_time_buffer[128];
  time_t now_seconds = now.tv.tv_sec;
  auto now_milliseconds = now.tv.tv_usec / 1000;
  strftime(date_time_buffer, 128, "%M:%S", localtime(&now_seconds));
  std::string time_string = fmt::format("[{}:{:03d}]", date_time_buffer, now_milliseconds);
#else
  char date_time_buffer[128];
  strftime(date_time_buffer, 128, "%M:%S", localtime(&now.tim));
  std::string time_string = fmt::format("[{}]", date_time_buffer);
#endif

  {
    std::lock_guard<std::mutex> lock(gLogger.mutex);
    if (gLogger.fp && log_level >= gLogger.file_log_level &&
        (!gLogger.file_muted || log_level >= level::warn)) {
      // log to file
      std::string file_string =
          fmt::format("{} [{}] {}\n", time_string, log_level_names[int(log_level)], message);
      write_file(file_string.c_str(), file_string.length());
      if (log_level >= gLogger.flush_level) {
        fflush(gLogger.fp);
      }
    }

    if (log_level >= gLogger.stdout_log_level ||
        (log_level == level::die && gLogger.stdout_log_level == level::off_unless_die)) {
      fmt::print("{} [", time_string);
      if (gLogger.disable_colors) {
        fmt::print("{}", log_level_names[int(log_level)]);
      } else {
        fmt::print(fg(log_colors[int(log_level)]), "{}", log_level_names[int(log_level)]);
      }
      fmt::print("] {}\n", console_safe(message));
      if (log_level >= gLogger.flush_level) {
        fflush(stdout);
        fflush(stderr);
      }
    }
  }

  if (log_level == level::die) {
    fflush(stdout);
    fflush(stderr);
    if (gLogger.fp) {
      fflush(gLogger.fp);
    }
    abort();
  }
}

void log_print(const char* message) {
  {
    // We always immediately flush prints because since it has no associated level
    // it could be anything from a fatal error to a useless debug log.
    std::lock_guard<std::mutex> lock(gLogger.mutex);
    if (gLogger.fp && !gLogger.file_muted) {
      // Log to File
      write_file(message, strlen(message));
      fflush(gLogger.fp);
    }

    if (gLogger.stdout_log_level < lg::level::off_unless_die) {
      fmt::print("{}", internal::console_safe(message));
      fflush(stdout);
      fflush(stderr);
    }
  }
}

void log_print_to(const char* message, bool to_file, bool to_console) {
  std::lock_guard<std::mutex> lock(gLogger.mutex);
  if (to_file && gLogger.fp) {
    write_file(message, strlen(message));
    fflush(gLogger.fp);
  }
  if (to_console && gLogger.stdout_log_level < lg::level::off_unless_die) {
    fmt::print("{}", internal::console_safe(message));
    fflush(stdout);
    fflush(stderr);
  }
}

void log_vprintf(const char* format, va_list arg_list) {
  {
    // We always immediately flush prints because since it has no associated level
    // it could be anything from a fatal error to a useless debug log.
    std::lock_guard<std::mutex> lock(gLogger.mutex);
    va_list arg_list_2;
    va_copy(arg_list_2, arg_list);
    if (gLogger.fp && !gLogger.file_muted) {
      // Log to File
      vfprintf(gLogger.fp, format, arg_list);
      fflush(gLogger.fp);
    }

    if (gLogger.stdout_log_level < lg::level::off_unless_die) {
      vprintf(format, arg_list_2);
      fflush(stdout);
      fflush(stderr);
    }
  }
}
}  // namespace internal

void printstd(const char* format, va_list arg_list) {
  internal::log_vprintf(format, arg_list);
}

// how many extra log files for a single program should be kept
constexpr int LOG_ROTATE_MAX = 10;

void set_file(const std::string& filename,
              const bool should_rotate,
              const bool append,
              const std::string& dir) {
  ASSERT(!gLogger.fp);
  std::string file_path;
  if (!dir.empty()) {
    file_path = file_util::combine_path(dir, filename);
  } else {
    file_path = file_util::get_file_path({"log", filename});
  }
  file_util::create_dir_if_needed_for_file(file_path);
  std::string complete_filename = file_path;
  if (should_rotate) {
    complete_filename += "." + str_util::current_local_timestamp_no_colons() + ".log";
    // remove any log files with the old format
    auto old_log_files =
        file_util::find_files_in_dir(fs::path(complete_filename).parent_path(),
                                     std::regex(fmt::format("{}\\.(\\d\\.)?log", filename)));
    for (const auto& file : old_log_files) {
      lg::debug("removing {}", file.string());
      fs::remove(file);
    }
    // remove the oldest log file if there are more than LOG_ROTATE_MAX
    auto existing_log_files = file_util::find_files_in_dir(
        fs::path(complete_filename).parent_path(), std::regex(fmt::format("{}.*\\.log", filename)));
    // sort the names and remove them
    existing_log_files = file_util::sort_filepaths(existing_log_files, true);
    if (existing_log_files.size() > (LOG_ROTATE_MAX - 1)) {
      lg::debug("removing {} log files", existing_log_files.size() - (LOG_ROTATE_MAX - 1));
      for (int i = 0; i < (int)existing_log_files.size() - (LOG_ROTATE_MAX - 1); i++) {
        lg::debug("removing {}", existing_log_files.at(i).string());
        fs::remove(existing_log_files.at(i));
      }
    }
  } else {
    if (!str_util::ends_with(complete_filename, ".log")) {
      complete_filename += ".log";
    }
  }

  if (append) {
    gLogger.fp = file_util::open_file(complete_filename.c_str(), "a");
  } else {
    gLogger.fp = file_util::open_file(complete_filename.c_str(), "w");
  }
  ASSERT(gLogger.fp);
}

void set_flush_level(level log_level) {
  gLogger.flush_level = log_level;
}

void set_file_level(level log_level) {
  gLogger.file_log_level = log_level;
}

void set_stdout_level(level log_level) {
  gLogger.stdout_log_level = log_level;
}

void set_max_debug_levels() {
  gLogger.flush_level = level::trace;
  gLogger.stdout_log_level = level::trace;
  gLogger.file_log_level = level::trace;
}

void disable_ansi_colors() {
  gLogger.disable_colors = true;
}

void set_file_muted(bool muted) {
  std::lock_guard<std::mutex> lock(gLogger.mutex);
  gLogger.file_muted = muted;
}

void initialize() {
  ASSERT(!gLogger.initialized);

#ifdef _WIN32
  // Enable some console terminal flags. Some of these may not be enabled by default, unless
  // the user edits their registry. This is clearly unwanted, so we just set them here ourselves.

  // VIRTUAL_TERMINAL_PROCESSING enables support for ANSI colors in the stdout text, used by the
  // logging tool.
  // QUICK_EDIT_MODE enables various mouse-related stdin functions, such as right-click for
  // copy-paste, scroll wheel to - shocker - scroll, etc.

  // Get handle to stdout
  HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
  // get current stdout mode
  DWORD modeStdOut;
  GetConsoleMode(hStdOut, &modeStdOut);
  // printf("stdout mode is: %08x", modeStdOut);

  // Get handle to stdin
  HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
  // get current stdin mode
  DWORD modeStdIn;
  GetConsoleMode(hStdIn, &modeStdIn);
  // printf("stdin mode is: %08x", modeStdIn);

  // enable VIRTUAL_TERMINAL_PROCESSING on stdout
  modeStdOut |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  SetConsoleMode(hStdOut, modeStdOut);

  // enable ENABLE_QUICK_EDIT_MODE on stdin
  modeStdIn |= ENABLE_QUICK_EDIT_MODE;
  SetConsoleMode(hStdIn, modeStdIn);
#endif

  gLogger.initialized = true;
}

void finish() {
  {
    std::lock_guard<std::mutex> lock(gLogger.mutex);
    if (gLogger.fp) {
      fclose(gLogger.fp);
      gLogger.fp = nullptr;
    }
  }
}

}  // namespace lg
