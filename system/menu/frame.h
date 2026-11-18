#pragma once

#include <cstdint>

#include "system/menu/logic.h"

namespace CardenzaHAL::SystemMenu::launcher {
struct ListSlide;
}

namespace CardenzaHAL::SystemMenu::detail {

// The menu as the 203 Launcher draws its lists on the Cardenza (frame.cpp: its menuFrame and status rail,
// on its own drawing code in system/menu/launcher/): rows [y, y + rows) of the screen into `grey`
// (kWidth * rows bytes, 0 black to 255 white). `slide`: the selection bar's motion (the Launcher's
// ListSlide, aimed at the selected row; nullptr: the bar is on it), at `now` on launcher::launcherMillis's
// clock. The bands of one frame are drawn with the same `now`: the first aims the slide, the others find it
// aimed.
void PaintGrey(const View& view, launcher::ListSlide* slide, uint32_t now, int y, int rows, uint8_t* grey);

}  // namespace CardenzaHAL::SystemMenu::detail
