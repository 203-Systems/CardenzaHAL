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
    // SetCapture(nullptr); CaptureValid: one has been copied since.
    void SetCapture(uint16_t* frame);
    bool CaptureValid();
    // The curtain: columns left of its edge are the menu's, and the firmware's frames are sent only right of
    // it (0: all the firmware's). Moves it to `edge` and paints the columns it passed from `painter`, in one
    // step no frame of the firmware's can fall into: going out, the new columns (the menu); going back, the
    // columns given back (`restore`: the painter has the firmware's picture for them; without, they stay
    // until the firmware's next frame, which then goes whole). A strip of few columns is sent in one piece
    // (see PaintColumns).
    bool MoveCurtain(RegionPainter painter, void* context, int edge, bool restore, int maxRows = 8);
    // Sends columns [x, x + width) of every row from `painter` now: eight rows at a time, or, when the strip
    // is narrow, as many rows at a time as the transfer buffer holds (1920 pixels), at most `maxRows` (what
    // the painter can paint at once).
    bool PaintColumns(RegionPainter painter, void* context, int x, int width, int maxRows = 8);

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
    // Back to the firmware: its next frame is sent whole, its brightness back; the table goes. `curtain`: the
    // menu keeps the columns left of it (it goes back to the left from there, MoveCurtain).
    void EndOverlay(int curtain = 0);
    // Before deep sleep: the panel asleep, the backlight off and held off.
    bool SleepPanel();

}  // namespace CardenzaHAL::Display::detail
