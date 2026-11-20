#pragma once

#include <cstdint>

// The development channel's screenshot (usb/dev.cpp). Internal to the HAL.
namespace CardenzaHAL::Display::detail {

    // The next frame the firmware presents (Present, PresentChanged) is copied into `frame` (WIDTH * HEIGHT,
    // as presented); nullptr withdraws the request. Costs the firmware one copy, once.
    void RequestSnapshot(uint16_t* frame);
    // The requested frame has been copied (true once, then the request is over).
    bool SnapshotTaken();
    // The framebuffer the firmware last presented (null: it presents none, or draws through the panel's
    // handles itself). Its memory is the firmware's: read it as it is now.
    const uint16_t* LastFramebuffer();

}  // namespace CardenzaHAL::Display::detail
