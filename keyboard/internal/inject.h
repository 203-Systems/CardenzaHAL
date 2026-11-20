#pragma once

#include <cstddef>
#include <cstdint>

#include "../layout.h"

// Keys pressed from a computer (the development channel, usb/dev.cpp): the firmware's updateKeyList and the
// system menu see them on top of the real ones. Internal to the HAL.
namespace CardenzaHAL::Keyboard::detail {

    // A key number (1..56, y * 14 + x + 1) as the scan has it: row 0..7 and the input column's bit.
    // False for a number outside the matrix.
    constexpr bool KeyScanSlot(int number, int& row, int& column)
    {
        if (number < 1 || number > 56) return false;
        const int x = (number - 1) % 14;
        const int y = (number - 1) / 14;
        column = x / 2;
        // KeyPosition (keyboard_key_scan.cpp): rows 4..7 give X_map_chart[column].x_1, rows 0..3 x_2;
        // y = 3 - (row % 4).
        row = (x == X_map_chart[column].x_1 ? 4 : 0) + (3 - y);
        return true;
    }

    // Down (true) or up. Any task.
    void InjectKey(int number, bool down);
    // Every key the computer holds, let go.
    void ReleaseInjectedKeys();
    // The key numbers the computer holds now (at most `capacity`).
    std::size_t InjectedKeys(uint8_t* numbers, std::size_t capacity);

}  // namespace CardenzaHAL::Keyboard::detail
