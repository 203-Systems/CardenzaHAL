#pragma once

#include <cstddef>
#include <cstdint>

#include "display/region.h"

// The screen stream (usb/camera.h) sees every frame the firmware presents. Internal to the HAL.
namespace CardenzaHAL::Display::detail {

    // Called at every framebuffer Present* while set, with the display's lock held, in the firmware's task: the
    // frame, and the regions the firmware presented (for PresentChanged: those it found changed, and its refresh
    // band; while the system menu is open: the whole screen). Keep it short. Null: none (the present paths then
    // test this one pointer and nothing else).
    using PresentHook = void (*)(const uint16_t* framebuffer, const Region* regions, std::size_t count);
    void SetPresentHook(PresentHook hook);

}  // namespace CardenzaHAL::Display::detail
