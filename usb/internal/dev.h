#pragma once

#include <cstddef>
#include <cstdint>

// The development channel (docs/usb-dev.md) between usb/usb.cpp, which carries it, and usb/dev.cpp, which
// answers it. Internal to the HAL.
namespace CardenzaHAL::USB::detail {

    // usb.cpp → dev.cpp, on the USB task. A request: what followed the command byte (id, op, args; the args
    // packed when it came as SysEx).
    void DevRequest(const uint8_t* data, std::size_t length, bool sysex);
    // The computer went away: keys it held are let go, an answer under way stops.
    void DevHostGone();

    // dev.cpp → usb.cpp. One answer message: a report 0xCB (63 bytes), or a whole SysEx (F0 … F7). They wait
    // up to `waitMs` for room (the HID queue; each USB-MIDI packet), and give up when the computer is gone.
    // Never wait from the USB task (waitMs 0): it is the one that makes room.
    bool DevSendHid(const uint8_t report[63], uint32_t waitMs);
    bool DevSendSysex(const uint8_t* message, std::size_t length, uint32_t waitMs);
    // The product's name (USB::Config::product).
    const char* Product();

}  // namespace CardenzaHAL::USB::detail
