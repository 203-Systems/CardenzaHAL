#include "st7789.h"

#include <cstring>

namespace cardenza_web::display {

namespace {
constexpr uint8_t kSwReset = 0x01;
constexpr uint8_t kSleepIn = 0x10;
constexpr uint8_t kSleepOut = 0x11;
constexpr uint8_t kInvertOff = 0x20;
constexpr uint8_t kInvertOn = 0x21;
constexpr uint8_t kDisplayOff = 0x28;
constexpr uint8_t kDisplayOn = 0x29;
constexpr uint8_t kColumnSet = 0x2A;
constexpr uint8_t kRowSet = 0x2B;
constexpr uint8_t kMemoryWrite = 0x2C;
constexpr uint8_t kMadctl = 0x36;
constexpr uint8_t kColmod = 0x3A;
constexpr uint8_t kMemoryWriteContinue = 0x3C;
constexpr uint8_t kRamControl = 0xB0;
constexpr uint8_t kMy = 0x80;
constexpr uint8_t kMx = 0x40;
constexpr uint8_t kMv = 0x20;
constexpr uint8_t kBgr = 0x08;
constexpr uint8_t kRamLittleEndian = 0x08;  // RAMCTRL second byte, bit 3
}  // namespace

St7789::St7789()
{
    // Cold boot: whatever the memory happened to hold; black is what a viewer can stand.
    std::memset(memory_, 0, sizeof(memory_));
}

void St7789::Reset()
{
    madctl_ = 0;
    colmod_ = 0x66;
    inverted_ = false;
    sleeping_ = true;
    displayOn_ = false;
    littleEndian_ = false;
    columnStart_ = 0;
    columnEnd_ = kMemoryColumns - 1;
    rowStart_ = 0;
    rowEnd_ = kMemoryRows - 1;
    writing_ = false;
    pendingBytes_ = 0;
    ++changes_;
}

void St7789::Command(uint8_t command, const uint8_t* p, std::size_t length)
{
    writing_ = false;
    pendingBytes_ = 0;
    switch (command)
    {
        case kSwReset: Reset(); break;
        case kSleepIn: sleeping_ = true; ++changes_; break;
        case kSleepOut: sleeping_ = false; ++changes_; break;
        case kInvertOff: inverted_ = false; ++changes_; break;
        case kInvertOn: inverted_ = true; ++changes_; break;
        case kDisplayOff: displayOn_ = false; ++changes_; break;
        case kDisplayOn: displayOn_ = true; ++changes_; break;
        case kColumnSet:
            if (length >= 4)
            {
                columnStart_ = (p[0] << 8) | p[1];
                columnEnd_ = (p[2] << 8) | p[3];
            }
            break;
        case kRowSet:
            if (length >= 4)
            {
                rowStart_ = (p[0] << 8) | p[1];
                rowEnd_ = (p[2] << 8) | p[3];
            }
            break;
        case kMadctl:
            if (length >= 1) madctl_ = p[0];
            ++changes_;
            break;
        case kColmod:
            if (length >= 1) colmod_ = p[0];
            break;
        case kRamControl:
            if (length >= 2) littleEndian_ = (p[1] & kRamLittleEndian) != 0;
            break;
        case kMemoryWrite: StartWrite(true); break;
        case kMemoryWriteContinue: StartWrite(false); break;
        default: break;  // gamma, porch and power settings change nothing a viewer could tell
    }
}

void St7789::StartWrite(bool fromStart)
{
    writing_ = true;
    pendingBytes_ = 0;
    if (fromStart)
    {
        x_ = columnStart_;
        y_ = rowStart_;
    }
}

void St7789::Store(uint16_t pixel)
{
    // The driver's (x, y) in the window, to the memory's portrait (column, row).
    int column = (madctl_ & kMv) ? y_ : x_;
    int row = (madctl_ & kMv) ? x_ : y_;
    if (madctl_ & kMx) column = kMemoryColumns - 1 - column;
    if (madctl_ & kMy) row = kMemoryRows - 1 - row;
    if (column >= 0 && column < kMemoryColumns && row >= 0 && row < kMemoryRows) memory_[row * kMemoryColumns + column] = pixel;
    // The address counter walks the window and wraps, as the controller does.
    if (++x_ > columnEnd_)
    {
        x_ = columnStart_;
        if (++y_ > rowEnd_) y_ = rowStart_;
    }
}

void St7789::WritePixels(const uint8_t* data, std::size_t bytes)
{
    if (!writing_) return;
    const bool eighteenBit = (colmod_ & 0x07) == 0x06;
    const int perPixel = eighteenBit ? 3 : 2;
    for (std::size_t index = 0; index < bytes; ++index)
    {
        pending_[pendingBytes_++] = data[index];
        if (pendingBytes_ < perPixel) continue;
        pendingBytes_ = 0;
        uint16_t pixel;
        if (eighteenBit)
            pixel = static_cast<uint16_t>(((pending_[0] & 0xF8) << 8) | ((pending_[1] & 0xFC) << 3) | (pending_[2] >> 3));
        else if (littleEndian_)
            pixel = static_cast<uint16_t>(pending_[0] | (pending_[1] << 8));
        else
            pixel = static_cast<uint16_t>((pending_[0] << 8) | pending_[1]);
        Store(pixel);
    }
    ++changes_;
}

void St7789::RenderView(uint16_t* view) const
{
    if (!Showing())
    {
        std::memset(view, 0, sizeof(uint16_t) * kViewWidth * kViewHeight);
        return;
    }
    const bool bgr = (madctl_ & kBgr) != 0;
    for (int vy = 0; vy < kViewHeight; ++vy)
    {
        const int column = kGlassLastColumn - vy;
        for (int vx = 0; vx < kViewWidth; ++vx)
        {
            const int row = kGlassFirstRow + vx;
            uint16_t pixel = memory_[row * kMemoryColumns + column];
            if (bgr) pixel = static_cast<uint16_t>(((pixel & 0x1F) << 11) | (pixel & 0x07E0) | (pixel >> 11));
            // The IPS glass shows the colours right with inversion on (the HAL turns it on).
            if (!inverted_) pixel = static_cast<uint16_t>(~pixel);
            view[vy * kViewWidth + vx] = pixel;
        }
    }
}

}  // namespace cardenza_web::display
