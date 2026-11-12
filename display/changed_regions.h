#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "region.h"

namespace CardenzaHAL::Display {

// What changed on screen since the last frame, as rectangles for Present(framebuffer, regions, count).
// Pure (no ESP-IDF): usable in host tests. Display::PresentChanged() uses it; a firmware that
// presents its own way can too.
//
// A whole frame is 17 ms of SPI, most of a frame at 50 fps, so sending only what changed is what
// keeps the UI fast. Every row is hashed in segments of 30 pixels and compared with the last frame;
// each band of eight rows (what Present sends at a time) gives one rectangle over its changed rows and
// the span of its changed segments, and a band right under another of the same span joins it. One band
// is reported whole every frame, a different one each time (the whole screen every kBands frames,
// about 0.34 s at 50 fps, for about 1 ms of SPI a frame), so nothing a hash missed or the panel lost
// stays on screen.
class ChangedRegions {
public:
    static constexpr int kSegmentPixels = 30;
    static constexpr int kSegments = WIDTH / kSegmentPixels;
    static constexpr int kBandRows = 8;
    static constexpr int kBands = (HEIGHT + kBandRows - 1) / kBandRows;
    static constexpr std::size_t kMaxRegions = kBands;
    static constexpr std::size_t kHashCount = static_cast<std::size_t>(HEIGHT) * kSegments;
    static_assert(kSegments * kSegmentPixels == WIDTH && kSegments <= 32 && kSegmentPixels % 2 == 0);

    // hashes: kHashCount words the caller owns (any RAM: they are read once a frame).
    explicit ChangedRegions(uint32_t* hashes) : hashes_(hashes) { Reset(); }

    // The next Find reports the whole screen (after the panel lost its pixels, say).
    void Reset()
    {
        std::memset(hashes_, 0, kHashCount * sizeof(uint32_t));
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
            uint32_t changed = 0;
            int first = -1, last = -1;
            for (int y = top; y < bottom; ++y)
            {
                uint32_t row = 0;
                for (int segment = 0; segment < kSegments; ++segment)
                {
                    const uint32_t hash = HashSegment(framebuffer + y * WIDTH + segment * kSegmentPixels);
                    uint32_t& previous = hashes_[y * kSegments + segment];
                    if (hash == previous && !everything_) continue;
                    previous = hash;
                    row |= 1u << segment;
                }
                if (row == 0) continue;
                if (first < 0) first = y;
                last = y;
                changed |= row;
            }
            if (band == refreshBand_)
            {
                changed = (1u << kSegments) - 1;
                first = top;
                last = bottom - 1;
            }
            if (changed == 0) continue;
            const int left = LowestBit(changed);
            const int right = HighestBit(changed);
            const Region region{left * kSegmentPixels, first, (right - left + 1) * kSegmentPixels, last - first + 1};
            Region* above = count > 0 ? &regions[count - 1] : nullptr;
            if (above != nullptr && above->x == region.x && above->width == region.width && above->y + above->height == region.y)
                above->height += region.height;
            else
                regions[count++] = region;
        }
        refreshBand_ = (refreshBand_ + 1) % kBands;
        everything_ = false;
        return count;
    }

private:
    // A word at a time: segments start on word boundaries in a 4-byte aligned buffer. ESP-IDF builds
    // with -fno-builtin-memcpy, where std::memcpy is a ROM call even for one word; __builtin_memcpy of an
    // aligned word is one load (hashing a frame went from 3.6 ms to 0.4 ms).
    static uint32_t HashSegment(const uint16_t* pixels)
    {
        uint32_t hash = 2166136261u;
        for (int index = 0; index < kSegmentPixels; index += 2)
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
    int refreshBand_ = 0;
    bool everything_ = true;
};

}  // namespace CardenzaHAL::Display
