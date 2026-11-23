#pragma once

#include <cstddef>
#include <cstdint>

#include "region.h"

namespace CardenzaHAL::Display {

// The order in which a frame's rectangles go to the panel, and how they are cut into the windows one DMA
// block carries. Pure (no ESP-IDF): host tests and simulators can use it. Every framebuffer Present* sends
// through it.
//
// The panel has no tearing-effect (TE) line, so a frame is written while the panel scans its memory out to
// the glass, and a refresh can show part of the old frame and part of the new. The ST7789 scans its native
// rows, which in this landscape orientation are the screen's columns, from x = 0 to x = WIDTH - 1 (the 203
// Launcher measured it on the Cardenza, 2026-11-06). Whole rows written top to bottom cross that scan, so a
// change that covers the screen always met it somewhere and moving pictures sheared along a diagonal.
// Written in narrow column bands from left to right, a frame runs along the scan instead: a refresh that
// meets the write splits on one straight vertical line, and with the write well ahead of the scan (the
// HAL's 80 MHz bus moves a whole frame in about 7 ms; the panel refreshes about every 16.7 ms) it meets it
// in only some of the frames. Small changes are written in a fraction of a millisecond and rarely meet it.
//
// So every rectangle is cut, from its left edge, into windows of a multiple of kScanColumns columns and at
// most kBlockPixels pixels (one DMA block): a tall rectangle goes as bands of 16 columns, a short wide one
// as one window across them (a strip of 8 rows across the whole screen is one window). Next() gives the
// windows of all the rectangles by their left edge, then their top, so the write moves left to right
// across the screen.
class ScanOrder {
public:
    static constexpr int kScanColumns = 16;
    static constexpr int kBlockPixels = kScanColumns * HEIGHT;  // 2160 pixels, 4320 bytes
    static constexpr std::size_t kMaxRegions = 32;              // per ScanOrder; more are taken in turns
    static_assert(WIDTH % kScanColumns == 0);

    // regions: valid rectangles of the screen (Present checks them), at most kMaxRegions (the rest are
    // ignored: give them to another ScanOrder). The array must outlive this object.
    ScanOrder(const Region* regions, std::size_t count)
        : regions_(regions), count_(count < kMaxRegions ? count : kMaxRegions)
    {
        for (std::size_t index = 0; index < count_; ++index) next_[index] = regions[index].x;
    }

    // The columns one window of a rectangle `height` rows tall may span: a multiple of kScanColumns, as many
    // as kBlockPixels allows (at least kScanColumns: a rectangle is at most HEIGHT rows tall).
    static constexpr int ChunkColumns(int height)
    {
        const int bands = height > 0 ? kBlockPixels / (kScanColumns * height) : 1;
        return kScanColumns * (bands < 1 ? 1 : bands);
    }

    // The next window (false: none left).
    bool Next(Region& window)
    {
        std::size_t best = count_;
        for (std::size_t index = 0; index < count_; ++index)
        {
            const Region& region = regions_[index];
            if (next_[index] >= region.x + region.width) continue;
            if (best == count_ || next_[index] < next_[best] ||
                (next_[index] == next_[best] && region.y < regions_[best].y))
                best = index;
        }
        if (best == count_) return false;
        const Region& region = regions_[best];
        const int left = next_[best];
        int right = left + ChunkColumns(region.height);
        if (right > region.x + region.width) right = region.x + region.width;
        next_[best] = right;
        window = Region{left, region.y, right - left, region.height};
        return true;
    }

private:
    const Region* regions_;
    std::size_t count_;
    int next_[kMaxRegions] = {};
};

}  // namespace CardenzaHAL::Display
