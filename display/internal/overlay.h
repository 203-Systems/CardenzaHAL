#pragma once

#include <cstdint>

// The system menu's hold on the screen (system/menu.h). Internal to the HAL.
namespace CardenzaHAL::Display::detail {

    // Paints screen rows [y, y + rows) into `band` (WIDTH * rows pixels, RGB565 as the panel takes it).
    using BandPainter = void (*)(void* context, int y, int rows, uint16_t* band);

    // From now on the firmware's Present* send nothing (and return true); the backlight is at least half.
    bool BeginOverlay();
    // The whole screen, eight rows at a time, from `painter`.
    bool PaintOverlay(BandPainter painter, void* context);
    // Back to the firmware: its next frame is sent whole, its brightness comes back.
    void EndOverlay();
    // Before deep sleep: the panel asleep, the backlight off and held off.
    bool SleepPanel();

}  // namespace CardenzaHAL::Display::detail
