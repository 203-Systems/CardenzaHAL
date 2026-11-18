#pragma once

#include <cstddef>

#include "../types.h"

// The system menu's hold on the keys and the key lights (system/menu.h). Internal to the HAL.
namespace CardenzaHAL::Keyboard::detail {

    // From now on the firmware's updateKeyList reports no key and the lights show `lights` (the
    // firmware's levels are kept). Needs Keyboard::init.
    void BeginOverlay(const LedFrame& lights);
    // Back to the firmware: its lights again; keys still down stay hidden from it until released.
    void EndOverlay();
    // The keys down now, whatever the overlay hides (debounced, at most `capacity`).
    std::size_t OverlayKeys(Point2D_t* keys, std::size_t capacity);
    // Every key light off and the LED enable held off through deep sleep.
    void LightsOffForSleep();

}  // namespace CardenzaHAL::Keyboard::detail
