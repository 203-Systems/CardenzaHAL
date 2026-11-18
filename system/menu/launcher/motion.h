#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>

// The Launcher's motion for the system menu: its easing and its lists' selection bar, which slides by its
// two edges (ListSlide), copied from its src/display.cpp (motion.inc), and its hold-to-act sheet's curves
// (power_state.h).
namespace CardenzaHAL::SystemMenu::launcher {

// The Launcher's Settings > Display > Motion. Always on here.
inline bool motion = true;
// Milliseconds, as the Launcher's clock: the menu's (system/menu.cpp) and the host tests' own.
uint32_t launcherMillis();

#include "motion.inc"

}  // namespace CardenzaHAL::SystemMenu::launcher

#include "power_state.h"
#include "ui_motion.h"
