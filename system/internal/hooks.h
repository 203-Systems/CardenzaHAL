#pragma once

#include <cstdint>

// What the system menu (system/menu.h) needs from the rest of the HAL. Internal to the HAL.
namespace CardenzaHAL::System::detail {

    // The side key (HOME, GPIO0) as the pad reads it.
    bool FuncButtonRaw();
    // The menu has the side key (true): GetFuncButton reports it released. Then false: hidden until it is
    // let go, so the press that opened or closed the menu never reaches the firmware.
    void HoldFuncButton(bool held);

    // USB (usb/usb.cpp) sets how to leave the host before a restart when it started; StopUsb runs it once
    // (nothing when USB never started, and nothing of TinyUSB is linked for it).
    void SetUsbStop(void (*stop)());
    void StopUsb();

    // One of the Launcher's tickets (system/launcher.h, LauncherTicket) for the next restart or wake.
    void WriteLauncherTicket(uint32_t value);

}  // namespace CardenzaHAL::System::detail
