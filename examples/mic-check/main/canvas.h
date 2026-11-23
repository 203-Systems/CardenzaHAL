#pragma once

// The 240 x 135 RGB565 framebuffer and the few shapes the screens use. Black, white and greys only, as the
// 203 Launcher draws. Pure (no ESP-IDF).

#include <cstdint>

namespace MicCheck {

inline constexpr int kWidth = 240;
inline constexpr int kHeight = 135;

// A neutral grey (0 black .. 255 white) in RGB565, green's sixth bit widened from the five of red and blue
// as the panel widens them (no green or magenta tint).
constexpr uint16_t Grey(int value)
{
    const int five = (value < 0 ? 0 : value > 255 ? 255 : value) >> 3;
    return static_cast<uint16_t>(five << 11 | (five << 1 | five >> 4) << 5 | five);
}

inline constexpr uint16_t kBlack = Grey(0);
inline constexpr uint16_t kWhite = Grey(255);
inline constexpr uint16_t kText = Grey(232);   // body text
inline constexpr uint16_t kDim = Grey(140);    // secondary text
inline constexpr uint16_t kFaint = Grey(70);   // rules, empty bars
inline constexpr uint16_t kShade = Grey(32);   // a panel just off the black

inline void PutPixel(uint16_t* frame, int x, int y, uint16_t colour)
{
    if (x >= 0 && x < kWidth && y >= 0 && y < kHeight) frame[y * kWidth + x] = colour;
}

// colour over what is there, alpha 0..15.
inline void BlendPixel(uint16_t* frame, int x, int y, uint16_t colour, int alpha)
{
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    uint16_t& pixel = frame[y * kWidth + x];
    if (alpha >= 15)
    {
        pixel = colour;
        return;
    }
    const int r = ((colour >> 11) * alpha + (pixel >> 11) * (15 - alpha) + 7) / 15;
    const int g = (((colour >> 5) & 0x3F) * alpha + ((pixel >> 5) & 0x3F) * (15 - alpha) + 7) / 15;
    const int b = ((colour & 0x1F) * alpha + (pixel & 0x1F) * (15 - alpha) + 7) / 15;
    pixel = static_cast<uint16_t>(r << 11 | g << 5 | b);
}

inline void FillRect(uint16_t* frame, int x, int y, int width, int height, uint16_t colour)
{
    for (int row = y; row < y + height; ++row)
        for (int column = x; column < x + width; ++column) PutPixel(frame, column, row, colour);
}

inline void Outline(uint16_t* frame, int x, int y, int width, int height, uint16_t colour)
{
    FillRect(frame, x, y, width, 1, colour);
    FillRect(frame, x, y + height - 1, width, 1, colour);
    FillRect(frame, x, y, 1, height, colour);
    FillRect(frame, x + width - 1, y, 1, height, colour);
}

}  // namespace MicCheck
