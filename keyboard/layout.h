#pragma once

#include <vector>

#include "../board/pins.h"
#include "types.h"

namespace CardenzaHAL::Keyboard {

inline const std::vector<int> output_list(Pins::kKeyboardOutputs.begin(), Pins::kKeyboardOutputs.end());
inline const std::vector<int> input_list(Pins::kKeyboardInputs.begin(), Pins::kKeyboardInputs.end());
inline const std::vector<int> led_pin_list(Pins::kKeyboardLedPins.begin(), Pins::kKeyboardLedPins.end());

inline constexpr Chart_t X_map_chart[7] = {
    {1, 0, 1},
    {2, 2, 3},
    {4, 4, 5},
    {8, 6, 7},
    {16, 8, 9},
    {32, 10, 11},
    {64, 12, 13},
};

}  // namespace CardenzaHAL::Keyboard
