#include "display/changed_regions.h"

#include <array>
#include <cassert>
#include <iostream>
#include <vector>

namespace Display = CardenzaHAL::Display;
using Finder = Display::ChangedRegions;

namespace {

alignas(4) std::array<uint16_t, Display::WIDTH * Display::HEIGHT> frame{};
std::array<uint32_t, Finder::kHashCount> hashes{};
std::array<Display::Region, Finder::kMaxRegions> regions{};

long Area(std::size_t count)
{
    long area = 0;
    for (std::size_t index = 0; index < count; ++index) area += static_cast<long>(regions[index].width) * regions[index].height;
    return area;
}

bool Covers(std::size_t count, int x, int y)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& r = regions[index];
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) return true;
    }
    return false;
}

}  // namespace

int main()
{
    for (std::size_t index = 0; index < frame.size(); ++index) frame[index] = static_cast<uint16_t>(index * 7);
    Finder finder(hashes.data());
    // The first frame is everything, as one region.
    std::size_t count = finder.Find(frame.data(), regions.data());
    assert(count == 1 && regions[0].x == 0 && regions[0].y == 0 && regions[0].width == Display::WIDTH &&
           regions[0].height == Display::HEIGHT);
    // Unchanged: the refresh band alone (band 1 now: band 0 went with the first frame).
    count = finder.Find(frame.data(), regions.data());
    assert(count == 1 && regions[0].y == 8 && regions[0].height == 8 && regions[0].width == Display::WIDTH);
    // One pixel: its 30-pixel segment of its row, beside the refresh band (band 2).
    frame[100 * Display::WIDTH + 95] ^= 1;
    count = finder.Find(frame.data(), regions.data());
    assert(count == 2 && Covers(count, 95, 100) && Area(count) == 30 + Display::WIDTH * 8);
    // The last row of one band and the first of the next, same columns: one region over both.
    frame[47 * Display::WIDTH + 40] ^= 1;
    frame[48 * Display::WIDTH + 40] ^= 1;
    count = finder.Find(frame.data(), regions.data());
    bool joined = false;
    for (std::size_t index = 0; index < count; ++index)
        joined |= regions[index].x == 30 && regions[index].y == 47 && regions[index].height == 2 && regions[index].width == 30;
    assert(joined);
    // Unchanged frames bring every row back once in kBands frames.
    std::array<int, Display::HEIGHT> seen{};
    for (int round = 0; round < Finder::kBands; ++round)
    {
        count = finder.Find(frame.data(), regions.data());
        for (std::size_t index = 0; index < count; ++index)
            for (int y = regions[index].y; y < regions[index].y + regions[index].height; ++y) ++seen[y];
    }
    for (int y = 0; y < Display::HEIGHT; ++y) assert(seen[y] == 1);
    // Reset: the whole screen again.
    finder.Reset();
    count = finder.Find(frame.data(), regions.data());
    assert(Area(count) == static_cast<long>(Display::WIDTH) * Display::HEIGHT);

    // The segment widths a comparison takes, and their tables.
    static_assert(Finder::ValidSegment(8) && Finder::ValidSegment(16) && Finder::ValidSegment(240));
    static_assert(!Finder::ValidSegment(6) && !Finder::ValidSegment(15) && !Finder::ValidSegment(7) && !Finder::ValidSegment(0));
    static_assert(Finder::HashCount(30) == Finder::kHashCount && Finder::HashCount(16) == 135u * 15 && Finder::HashCount(7) == 0);
    {
        // 16-pixel segments: a changed pixel's rectangle is its 16 columns; no refresh band.
        std::vector<uint32_t> table(Finder::HashCount(16));
        Finder narrow(table.data(), 16, 0);
        assert(narrow.SegmentPixels() == 16 && narrow.RefreshBands() == 0);
        count = narrow.Find(frame.data(), regions.data());
        assert(Area(count) == static_cast<long>(Display::WIDTH) * Display::HEIGHT);
        assert(narrow.Find(frame.data(), regions.data()) == 0);  // unchanged, no refresh: nothing
        frame[20 * Display::WIDTH + 33] ^= 1;
        count = narrow.Find(frame.data(), regions.data());
        assert(count == 1 && regions[0].x == 32 && regions[0].width == 16 && regions[0].y == 20 && regions[0].height == 1);
        // An invalid width takes the default.
        Finder fallback(hashes.data(), 7, 1);
        assert(fallback.SegmentPixels() == Finder::kSegmentPixels);
    }
    {
        // Two refresh bands a frame: the whole screen in nine frames, each row once.
        Finder twice(hashes.data(), Finder::kSegmentPixels, 2);
        (void)twice.Find(frame.data(), regions.data());
        std::array<int, Display::HEIGHT> rows{};
        for (int round = 0; round < (Finder::kBands + 1) / 2; ++round)
        {
            count = twice.Find(frame.data(), regions.data());
            for (std::size_t index = 0; index < count; ++index)
                for (int y = regions[index].y; y < regions[index].y + regions[index].height; ++y) ++rows[y];
        }
        int covered = 0;
        for (int y = 0; y < Display::HEIGHT; ++y) covered += rows[y] > 0 ? 1 : 0;
        assert(covered == Display::HEIGHT);
        // Every band every frame: the whole screen each time, as one region.
        Finder always(hashes.data(), Finder::kSegmentPixels, 99);
        assert(always.RefreshBands() == Finder::kBands);
        (void)always.Find(frame.data(), regions.data());
        count = always.Find(frame.data(), regions.data());
        assert(count == 1 && Area(count) == static_cast<long>(Display::WIDTH) * Display::HEIGHT);
    }
    std::cout << "changed regions ok\n";
    return 0;
}
