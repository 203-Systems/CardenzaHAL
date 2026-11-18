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
    // Columns left of `edge` are the menu's: the firmware's frames are sent only right of it. 0: all the
    // firmware's. `refresh`: when the edge moves left, the firmware's next frame is sent whole (the menu did
    // not give those columns back its picture).
    void SetCurtain(int edge, bool refresh);
    // Sends columns [x, x + width) of every row from `painter` now, eight rows at a time.
    bool PaintColumns(RegionPainter painter, void* context, int x, int width);

    // From now on the firmware's Present* send nothing (and return true; frames are still captured). Makes
    // the overlay's table of what it sent (ChangedRegions' row segments, about 4 KB, PSRAM when there is
    // some; EndOverlay gives it back).
    bool BeginOverlay();
    // The whole screen from `painter`, eight rows at a time: of each band only the rectangle over the rows
    // that changed since the overlay last sent it and the span of their 30-pixel segments goes out
    // (ChangedRegions::FindBand), so a frame of a sliding selection bar sends the strips its edges swept.
    // Everything after anything else was sent, and every band whole when there was no room for the table.
    bool PaintOverlay(RegionPainter painter, void* context);
    // The backlight while the menu is there (0-255); -1: the firmware's brightness again.
    void SetBacklight(int value);
    // Back to the firmware: its next frame is sent whole, no curtain, its brightness back; the table goes.
    void EndOverlay();
    // Before deep sleep: the panel asleep, the backlight off and held off.
    bool SleepPanel();

}  // namespace CardenzaHAL::Display::detail
