#pragma once

// The Cardenza's display controller, an ST7789 with a 1.14" 135 x 240 glass, as a model: its frame
// memory (240 x 320), the commands a driver sends (sleep, display on/off, inversion, MADCTL,
// COLMOD, RAMCTRL byte order, CASET/RASET windows, RAMWR / RAMWRC), and what the glass shows.
//
// The glass is the part of the frame memory the panel shows: columns 52..186 and rows 40..279 in
// the controller's portrait addressing, seen in landscape. MADCTL maps the driver's addresses to
// the memory (MV exchanges them, MX mirrors the column, MY the row), so the HAL's own setup
// (swap_xy + mirror_x, gap 40/53) gives an upright landscape picture, and anything else a firmware
// sends shows on the glass as it would on the device (mirrored, offset or rotated). The glass needs
// colour inversion (INVON) to show the colours right, as the HAL sends it.
//
// Pure: tested on the host (tests/web).

#include <cstddef>
#include <cstdint>

namespace cardenza_web::display {

class St7789 {
public:
    static constexpr int kMemoryColumns = 240;
    static constexpr int kMemoryRows = 320;
    static constexpr int kGlassFirstColumn = 52;
    static constexpr int kGlassLastColumn = 186;
    static constexpr int kGlassFirstRow = 40;
    static constexpr int kGlassLastRow = 279;
    static constexpr int kViewWidth = kGlassLastRow - kGlassFirstRow + 1;        // 240
    static constexpr int kViewHeight = kGlassLastColumn - kGlassFirstColumn + 1; // 135

    St7789();

    // The reset line or SWRESET: sleeping, display off, defaults; the memory keeps what it had.
    void Reset();
    // A command and its parameters (CASET, MADCTL, ...), then pixel data for RAMWR / RAMWRC.
    void Command(uint8_t command, const uint8_t* parameters, std::size_t length);
    void WritePixels(const uint8_t* data, std::size_t bytes);

    // What the glass shows, in landscape (kViewWidth x kViewHeight), RGB565: black while the panel
    // sleeps or is off. The backlight is not the controller's business.
    void RenderView(uint16_t* view) const;
    bool Showing() const { return displayOn_ && !sleeping_; }
    uint32_t Changes() const { return changes_; }

    uint8_t Madctl() const { return madctl_; }
    uint8_t Colmod() const { return colmod_; }
    bool Inverted() const { return inverted_; }
    bool Sleeping() const { return sleeping_; }
    bool DisplayOn() const { return displayOn_; }
    bool LittleEndian() const { return littleEndian_; }
    // The memory at a controller (portrait) address.
    uint16_t Memory(int column, int row) const { return memory_[row * kMemoryColumns + column]; }

private:
    void Store(uint16_t pixel);
    void StartWrite(bool fromStart);

    uint16_t memory_[kMemoryColumns * kMemoryRows];
    uint8_t madctl_ = 0;
    uint8_t colmod_ = 0x66;
    bool inverted_ = false;
    bool sleeping_ = true;
    bool displayOn_ = false;
    bool littleEndian_ = false;
    int columnStart_ = 0;
    int columnEnd_ = kMemoryColumns - 1;
    int rowStart_ = 0;
    int rowEnd_ = kMemoryRows - 1;
    int x_ = 0;
    int y_ = 0;
    uint8_t pending_[3] = {};
    int pendingBytes_ = 0;
    bool writing_ = false;
    uint32_t changes_ = 0;
};

}  // namespace cardenza_web::display
