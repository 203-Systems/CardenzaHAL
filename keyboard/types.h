#pragma once

#include <array>
#include <cstddef>
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
// Sanity limit for sampled matrix keys, including modifiers.
inline constexpr std::size_t KEY_PRESS_LIMIT = 4;
inline constexpr std::size_t LED_COLUMNS = 14;
inline constexpr std::size_t LED_ROWS = 4;
// Row-major electrical brightness, indexed as y * LED_COLUMNS + x.
using LedFrame = std::array<uint8_t, LED_ROWS * LED_COLUMNS>;

}  // namespace CardenzaHAL::Keyboard
