#pragma once

#include <cstdint>

#include "system/menu/logic.h"

namespace CardenzaHAL::SystemMenu::launcher {
struct ListFade;
}

namespace CardenzaHAL::SystemMenu::detail {

// The menu as the 203 Launcher draws its lists on the Cardenza (frame.cpp: its menuFrame and status rail,
// on its own drawing code in system/menu/launcher/): rows [y, y + rows) of the screen into `grey`
// (kWidth * rows bytes, 0 black to 255 white). `fade`: the Launcher's crossfade between selected rows and
// through a scroll (nullptr: none), at `now` on launcher::launcherMillis's clock.
void PaintGrey(const View& view, launcher::ListFade* fade, uint32_t now, int y, int rows, uint8_t* grey);

}  // namespace CardenzaHAL::SystemMenu::detail
