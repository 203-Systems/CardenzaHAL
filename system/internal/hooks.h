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

    // One of the Launcher's tickets (system/launcher.h, LauncherTicket) for the next restart.
    void WriteLauncherTicket(uint32_t value);
    // Writes it after everything that would overwrite it (ESP-IDF's timekeeping sync) and restarts.
    [[noreturn]] void RestartWithTicket(uint32_t value);

    // The side key's pad as System::Init sets it (an input with its pull-up). ESP-IDF's SD driver resets
    // GPIO0 when it lets a 4-bit slot go (its unused d4-d7 are 0): Storage calls this after a mount fails
    // and after an unmount.
    void RestoreFuncButton();

}  // namespace CardenzaHAL::System::detail
