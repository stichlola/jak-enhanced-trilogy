#pragma once

// The runtime's precompiled header (CMake option ENHANCED_PCH, off by default; dev builds turn it on):
// headers nearly every file includes and that rarely change, parsed once instead of in each file.
// FileUtil.h brings windows.h (NOMINMAX, WIN32_LEAN_AND_MEAN) and ghc's filesystem. Not imgui: some
// files define IMGUI_DEFINE_MATH_OPERATORS before including it.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"
