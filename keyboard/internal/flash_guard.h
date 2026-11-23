#pragma once

// The key lights through a flash write (system/settings.cpp). Internal to the HAL.
namespace CardenzaHAL::Keyboard::detail {

    // true: every key light off from the scan's next step on (the keys are still read), and so through a
    // flash write: the scan's interrupt is not in IRAM, so while the caches are off it does not run, and the
    // row it had lit would stay lit at full duty (a flash of light; the user saw it when CardenzaSynth wrote
    // NVS, 2026-11-05). Waits until the lights are off. false: back on at the next step.
    void HoldLightsDark(bool dark);

    // Whether the master level has a floor while the system menu is open (on: its keys show even when the
    // firmware dimmed the lights to nothing). Off while the menu's key light setting is being changed, so
    // the level shows as it is.
    void SetOverlayFloor(bool on);

}  // namespace CardenzaHAL::Keyboard::detail
