#pragma once

#include <cstdint>

namespace CardenzaHAL::Keyboard {

struct Chart_t
{
    uint8_t value;
    uint8_t x_1;
    uint8_t x_2;
};

struct Point2D_t
{
    int x;
    int y;
};

inline constexpr uint8_t LED_BRIGHTNESS_MAX = 15;

}  // namespace CardenzaHAL::Keyboard
