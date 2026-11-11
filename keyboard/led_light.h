#pragma once

#include <array>
#include <cstdint>

#include "keyboard/internal/led_brightness.h"

// Key LED light as the eye sees it. Pure: a firmware, its simulator and its tests can all use it.
//
// setLED and setLEDFrame levels are linear duty: a key at level 7 is lit 7/15 of the time, which the
// eye sees as far brighter than half, and the low levels bunch together at the top. A firmware that
// thinks in perceived brightness maps its levels with PerceivedToLevel (gamma 2.6, judged on the
// board's LEDs) before it sends them; every lit perceived level stays lit (at least level 1).
// setLEDGlobalBrightness is already corrected (gamma 2) and takes its level as it is.
namespace CardenzaHAL::Keyboard::Light {

inline constexpr float kPerceivedGamma = 2.6f;

// round(15 * (perceived / 15) ^ 2.6), at least 1 for a lit key.
inline constexpr std::array<uint8_t, 16> kLevelForPerceived{{0, 1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 7, 8, 10, 13, 15}};

// Perceived brightness 0..15 -> the level for setLED / setLEDFrame (linear duty 0..15).
constexpr uint8_t PerceivedToLevel(uint8_t perceived)
{
    return kLevelForPerceived[perceived > 15 ? 15 : perceived];
}

// The fraction of the time a key at this level is lit.
constexpr float LevelDuty(uint8_t level)
{
    return static_cast<float>(detail::LedDutyForLevel(level)) / 15.0f;
}

// The fraction the global level lets through (setLEDGlobalBrightness: the LED enable pin's PWM).
constexpr float GlobalDuty(uint8_t global)
{
    return static_cast<float>(detail::LedGlobalDutyForLevel(global)) / 1023.0f;
}

// The light of a key at a perceived level under a global level, in thousandths of full: what a
// simulator draws for that key.
constexpr uint16_t LightPermille(uint8_t perceived, uint8_t global)
{
    return static_cast<uint16_t>(LevelDuty(PerceivedToLevel(perceived)) * GlobalDuty(global) * 1000.0f + 0.5f);
}

}  // namespace CardenzaHAL::Keyboard::Light
