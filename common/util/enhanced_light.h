#pragma once

#include "common/util/FileUtil.h"

/*!
 * Enhanced Light: the single-game build for the OpenGOAL launcher (a file named enhanced-light in the
 * data folder, put there by make_release.py --light). It keeps the graphics, the HD pause maps, the
 * weather and night and the Enhanced menu, and leaves out what changes the game itself: the start
 * menu, the nap and beds, the game switch, photo mode, weather that affects play and the Xbox buttons.
 */
inline bool enhanced_light() {
  static const bool light = fs::exists(file_util::get_jak_project_dir() / "enhanced-light");
  return light;
}
