#pragma once

#include <cstdint>

namespace cardenza_web::display {

class St7789;
// The display controller every panel IO on the display's pins talks to (lcd.cpp).
St7789& Controller();
// GPIO33, the controller's reset line (active low).
void ResetLine(bool level);
// The backlight's light, 0..1 (GPIO38: LEDC duty, or a plain output level).
void SetBacklight(float light);
float Backlight();

}  // namespace cardenza_web::display
