#pragma once

/*!
 * Jak 3 Enhanced: values the game hands to the renderer through the kernel (kmachine.cpp).
 */

// The key light ("sun") direction of the current time of day, world space, as the game's
// time-of-day light-group 0 dir0. Set every frame by pc-enhanced-set-sun.
struct EnhancedSun {
  float dir[3] = {0.f, 1.f, 0.f};
  bool valid = false;
};
extern EnhancedSun g_enhanced_sun;
