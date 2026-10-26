#pragma once

#include <cstdint>

namespace CardenzaHAL::Keyboard::detail {

// Four binary-weighted planes: exactly 16 electrical duty levels.
constexpr uint8_t LedDutyForLevel(uint8_t level)
{
    return level > 15 ? 15 : level;
}

constexpr uint16_t LedGlobalDutyForLevel(uint8_t level, bool perceptual = true)
{
    level = level > 15 ? 15 : level;
    return perceptual
        ? static_cast<uint16_t>((static_cast<uint32_t>(level) * level * 1023 + 112) / 225)
        : static_cast<uint16_t>((static_cast<uint32_t>(level) * 1023 + 7) / 15);
}

}  // namespace CardenzaHAL::Keyboard::detail
