#pragma once

// What the page feeds into the board and the drivers read (web_main.cpp sets it).

namespace cardenza_web::board {

// The HOME button (the side key) on GPIO0, active low.
void SetHome(bool pressed);
bool HomePressed();

}  // namespace cardenza_web::board
