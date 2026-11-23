#pragma once

// The board's screen brightness setting (system/settings.h). Internal to the HAL.
namespace CardenzaHAL::Display::detail {

    // The backlight at the system setting: `percent` (10 to 100) on the 203 Launcher's duty curve and at its
    // PWM frequency (256 Hz), the same light as in the Launcher and the system menu. It is what the screen
    // shows until the firmware calls SetBrightness (then its own level, as before), and again after every
    // change of the setting.
    void SetSystemBrightness(int percent);

}  // namespace CardenzaHAL::Display::detail
