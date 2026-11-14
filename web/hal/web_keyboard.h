#pragma once

#include <cstdint>

// The page's side of the web keyboard (keyboard.cpp).
namespace cardenza_web::keyboard {

struct LedSnapshot {
    uint32_t serial;
    uint8_t levels[56];  // per key, y * 14 + x: the duty level 0..15 the scan drives (0 when disabled)
    uint8_t global;      // setLEDGlobalBrightness, 0..15
    uint8_t enabled;
    uint8_t reserved[2];
};

void SetKey(int x, int y, bool down);
void ReleaseAll();
const LedSnapshot& Leds();

}  // namespace cardenza_web::keyboard
