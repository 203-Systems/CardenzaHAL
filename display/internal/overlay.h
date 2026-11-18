#pragma once

#include <cstdint>

// The system menu's hold on the screen (system/menu.h). Internal to the HAL.
namespace CardenzaHAL::Display::detail {

    // Paints columns [x, x + width) of screen rows [y, y + rows) into `pixels` (width * rows, RGB565 as the
    // panel takes it).
    using RegionPainter = void (*)(void* context, int x, int y, int width, int rows, uint16_t* pixels);

    // The firmware has shown a frame (the panel is on).
    bool Ready();
    // Every frame the firmware presents is copied into `frame` (WIDTH * HEIGHT, as presented) until
    // SetCapture(nullptr); CaptureValid: one has been copied since (the menu's backdrop is made from it).
    void SetCapture(uint16_t* frame);
    bool CaptureValid();
    // From now on the firmware's Present* send nothing (and return true; frames are still captured). Makes
    // the overlay's table of what it sent (ChangedRegions' row segments, about 4 KB, PSRAM when there is
    // some; EndOverlay gives it back).
    bool BeginOverlay();
    // The whole screen from `painter`, eight rows at a time: of each band only the rectangle over the rows
    // that changed since the overlay last sent it and the span of their 30-pixel segments goes out
    // (ChangedRegions::FindBand), so a frame of a sliding selection bar sends the strips its edges swept.
    // Everything after anything else was sent, and every band whole when there was no room for the table.
    bool PaintOverlay(RegionPainter painter, void* context);
    // The backlight while the menu is there: its PWM duty (0 to kBacklightFullDuty, display/
    // backlight_brightness.h), with `launcherPwm` at the 203 Launcher's PWM frequency (256 Hz), where a duty is
    // as bright as in the Launcher; -1: the firmware's brightness and the HAL's frequency again.
    void SetBacklightDuty(int duty, bool launcherPwm);
    // The duty of the firmware's brightness.
    int FirmwareBacklightDuty();
    // Back to the firmware: its next frame is sent whole, its brightness back; the table goes.
    void EndOverlay();
    // Before deep sleep: the panel asleep, the backlight off and held off.
    bool SleepPanel();

}  // namespace CardenzaHAL::Display::detail
