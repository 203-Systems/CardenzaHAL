#pragma once

#include <cstdint>

namespace CardenzaHAL::Keyboard::detail {

// All 16 app settings remain distinct. Low-end rounding is forced monotonic.
constexpr uint8_t LedDutyForLevel(uint8_t level, bool perceptual)
{
    constexpr uint8_t gamma[16] = {
        0, 1, 2, 3, 4, 7, 10, 14, 18, 23, 28, 34, 40, 47, 55, 63
    };
    level = level > 15 ? 15 : level;
    return perceptual ? gamma[level] : static_cast<uint8_t>((level * 63 + 7) / 15);
}

constexpr uint16_t LedGlobalDutyForLevel(uint8_t level)
{
    level = level > 15 ? 15 : level;
    return static_cast<uint16_t>((static_cast<uint32_t>(level) * level * 1023 + 112) / 225);
}

}  // namespace CardenzaHAL::Keyboard::detail
