#include "display/changed_regions.h"

#include <array>
#include <cassert>
#include <iostream>

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
    std::cout << "changed regions ok\n";
    return 0;
}
