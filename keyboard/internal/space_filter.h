#pragma once

#include <cstdint>

// The screenshot's look at space (screenshot/screenshot.h, HOME + space). Internal to the HAL.
namespace CardenzaHAL::Keyboard::detail {

    // Decides, each time the firmware reads the keys (updateKeyList), whether it sees space (key 56) down:
    // `down` is the scan's (debounced, a computer's keys too), `nowMs` the time. nullptr: space as it is.
    using SpaceFilter = bool (*)(bool down, uint32_t nowMs);
    void SetSpaceFilter(SpaceFilter filter);

    // Key `number` (1..56) is down now, as the scan reads it (a computer's keys too), whatever the firmware is
    // shown (the system menu, the space filter).
    bool KeyDown(int number);

}  // namespace CardenzaHAL::Keyboard::detail
