#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "region.h"

namespace CardenzaHAL::Display {

// What changed on screen since the last frame, as rectangles for Present(framebuffer, regions, count).
// Pure (no ESP-IDF): usable in host tests. Display::PresentChanged() uses it (display/presenter.h sets its
// segment and refresh); a firmware that presents its own way can too.
//
// Sending only what changed is what keeps a UI fast: a whole frame is 64,800 bytes on the panel's SPI bus.
// Every row is hashed in segments (30 pixels by default) and compared with the last frame; each band of
// eight rows gives one rectangle over its changed rows and the span of its changed segments, and a band
// right under another of the same span joins it. Some bands are reported whole every frame, a different
// one each time (by default one: the whole screen every kBands frames, about 0.34 s at 50 fps, for 3.8 KB a
// frame), so nothing a hash missed or the panel lost stays on screen.
//
// FindBand is the same comparison for one band given on its own, for a screen painted a band at a time
// (the system menu's overlay, Display::detail::PaintOverlay): the rectangle over what changed in it.
class ChangedRegions {
public:
    // The defaults (CardenzaSynth's, measured on the board) and the table they need.
    static constexpr int kSegmentPixels = 30;
    static constexpr int kSegments = WIDTH / kSegmentPixels;
    static constexpr int kBandRows = 8;
    static constexpr int kBands = (HEIGHT + kBandRows - 1) / kBandRows;
    static constexpr std::size_t kMaxRegions = kBands;
    static constexpr std::size_t kHashCount = static_cast<std::size_t>(HEIGHT) * kSegments;
    static constexpr int kMaxSegments = 32;  // a row's changed segments are bits of one word

    // A segment width the comparison takes: an even divisor of WIDTH with at most 32 segments a row
    // (8, 10, 12, 16, 20, 24, 30, 40, 48, 60, 80, 120 or 240 pixels). Narrower finds smaller rectangles for a
    // larger table and a little more time; 16 lines the rectangles up with the panel's 16-column scan bands.
    static constexpr bool ValidSegment(int pixels)
    {
        return pixels >= 2 && pixels % 2 == 0 && WIDTH % pixels == 0 && WIDTH / pixels <= kMaxSegments;
    }
    // The table's words for a segment width (HEIGHT * segments a row; 0 for an invalid width).
    static constexpr std::size_t HashCount(int segmentPixels)
    {
        return ValidSegment(segmentPixels) ? static_cast<std::size_t>(HEIGHT) * (WIDTH / segmentPixels) : 0;
    }
    // hashes: HashCount(segmentPixels) words the caller owns (any RAM: they are read once a frame).
    // segmentPixels: see ValidSegment (an invalid width takes kSegmentPixels). refreshBands: bands reported
    // whole every frame, 0 (none) to kBands (every frame whole).
    explicit ChangedRegions(uint32_t* hashes, int segmentPixels = kSegmentPixels, int refreshBands = 1)
        : hashes_(hashes),
          segmentPixels_(ValidSegment(segmentPixels) ? segmentPixels : kSegmentPixels),
          segments_(WIDTH / segmentPixels_),
          refreshBands_(refreshBands < 0 ? 0 : refreshBands > kBands ? kBands : refreshBands)
    {
        Reset();
    }

    int SegmentPixels() const { return segmentPixels_; }
    int RefreshBands() const { return refreshBands_; }

    // The next Find reports the whole screen (after the panel lost its pixels, say).
    void Reset()
    {
        std::memset(hashes_, 0, HashCount(segmentPixels_) * sizeof(uint32_t));
        everything_ = true;
    }

    // framebuffer: WIDTH * HEIGHT RGB565, 4-byte aligned. Fills regions (kMaxRegions of room) and
    // returns how many it filled.
    std::size_t Find(const uint16_t* framebuffer, Region* regions)
    {
        std::size_t count = 0;
        for (int band = 0; band < kBands; ++band)
        {
            const int top = band * kBandRows;
            const int bottom = top + kBandRows < HEIGHT ? top + kBandRows : HEIGHT;
            Region region = FindBand(framebuffer + top * WIDTH, WIDTH, top, bottom, everything_);
            if ((band - refreshBand_ + kBands) % kBands < refreshBands_) region = Region{0, top, WIDTH, bottom - top};
            if (region.width == 0) continue;
            Region* above = count > 0 ? &regions[count - 1] : nullptr;
            if (above != nullptr && above->x == region.x && above->width == region.width && above->y + above->height == region.y)
                above->height += region.height;
            else
                regions[count++] = region;
        }
        refreshBand_ = (refreshBand_ + refreshBands_) % kBands;
        everything_ = false;
        return count;
    }

    // Rows [top, bottom) of the screen, given on their own: `pixels` is row `top`, `stride` pixels a row
    // (a multiple of 2, 4-byte aligned). The rectangle over the rows that changed since these rows were
    // last given and the span of their changed segments (width 0: nothing changed); `everything`: the
    // whole width and every row (the hashes are taken all the same).
    Region FindBand(const uint16_t* pixels, int stride, int top, int bottom, bool everything)
    {
        uint32_t changed = 0;
        int first = -1, last = -1;
        for (int y = top; y < bottom; ++y)
        {
            uint32_t row = 0;
            const uint16_t* line = pixels + (y - top) * stride;
            uint32_t* previous = hashes_ + y * segments_;
            for (int segment = 0; segment < segments_; ++segment)
            {
                const uint32_t hash = HashSegment(line + segment * segmentPixels_, segmentPixels_);
                if (hash == previous[segment] && !everything) continue;
                previous[segment] = hash;
                row |= 1u << segment;
            }
            if (row == 0) continue;
            if (first < 0) first = y;
            last = y;
            changed |= row;
        }
        if (changed == 0) return Region{0, top, 0, 0};
        const int left = LowestBit(changed);
        const int right = HighestBit(changed);
        return Region{left * segmentPixels_, first, (right - left + 1) * segmentPixels_, last - first + 1};
    }

    // One segment's hash (FNV-1a over its words). A word at a time: segments start on word boundaries in a
    // 4-byte aligned buffer. ESP-IDF builds with -fno-builtin-memcpy, where std::memcpy is a ROM call even
    // for one word; __builtin_memcpy of an aligned word is one load (hashing a frame in internal RAM went
    // from 3.6 ms to 0.4 ms, CardenzaSynth 2026-11-05).
    static uint32_t HashSegment(const uint16_t* pixels, int count)
    {
        uint32_t hash = 2166136261u;
        for (int index = 0; index < count; index += 2)
        {
            uint32_t word;
#if defined(__GNUC__)
            __builtin_memcpy(&word, __builtin_assume_aligned(pixels + index, 4), sizeof(word));
#else
            std::memcpy(&word, pixels + index, sizeof(word));
#endif
            hash = (hash ^ word) * 16777619u;
        }
        return hash ^ (hash >> 15);
    }

private:
    static int LowestBit(uint32_t bits)
    {
        int index = 0;
        while ((bits & 1u) == 0) { bits >>= 1; ++index; }
        return index;
    }

    static int HighestBit(uint32_t bits)
    {
        int index = -1;
        while (bits != 0) { bits >>= 1; ++index; }
        return index;
    }

    uint32_t* hashes_;
    int segmentPixels_;
    int segments_;
    int refreshBands_;
    int refreshBand_ = 0;
    bool everything_ = true;
};

static_assert(ChangedRegions::ValidSegment(ChangedRegions::kSegmentPixels));

}  // namespace CardenzaHAL::Display
