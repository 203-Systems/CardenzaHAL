// display/scan_order.h: how rectangles are cut into DMA windows and in which order they go to the panel.
#include "display/scan_order.h"

#include <array>
#include <cassert>
#include <iostream>
#include <vector>

namespace Display = CardenzaHAL::Display;
using Display::Region;
using Display::ScanOrder;

namespace {

std::vector<Region> Windows(const std::vector<Region>& regions)
{
    std::vector<Region> windows;
    ScanOrder order(regions.data(), regions.size());
    Region window{};
    while (order.Next(window)) windows.push_back(window);
    return windows;
}

// Every pixel of every region is in exactly as many windows as regions cover it; no window leaves its region.
void CheckCover(const std::vector<Region>& regions, const std::vector<Region>& windows)
{
    std::array<int, Display::WIDTH * Display::HEIGHT> want{}, got{};
    for (const auto& r : regions)
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x; x < r.x + r.width; ++x) ++want[y * Display::WIDTH + x];
    for (const auto& w : windows)
    {
        assert(w.width > 0 && w.height > 0 && w.width * w.height <= ScanOrder::kBlockPixels);
        for (int y = w.y; y < w.y + w.height; ++y)
            for (int x = w.x; x < w.x + w.width; ++x) ++got[y * Display::WIDTH + x];
    }
    assert(want == got);
}

void CheckLeftToRight(const std::vector<Region>& windows)
{
    for (std::size_t i = 1; i < windows.size(); ++i)
        assert(windows[i - 1].x < windows[i].x || (windows[i - 1].x == windows[i].x && windows[i - 1].y <= windows[i].y));
}

}  // namespace

int main()
{
    static_assert(ScanOrder::kBlockPixels == 16 * Display::HEIGHT);
    static_assert(ScanOrder::ChunkColumns(Display::HEIGHT) == 16);
    static_assert(ScanOrder::ChunkColumns(8) == 256);   // a band of eight rows: the whole width at once
    static_assert(ScanOrder::ChunkColumns(40) == 48);

    // The whole screen: fifteen bands of 16 columns, left to right, each a whole column of the screen.
    {
        const std::vector<Region> regions{{0, 0, Display::WIDTH, Display::HEIGHT}};
        const auto windows = Windows(regions);
        assert(windows.size() == 15);
        for (std::size_t i = 0; i < windows.size(); ++i)
            assert(windows[i].x == static_cast<int>(i) * 16 && windows[i].width == 16 && windows[i].y == 0 &&
                   windows[i].height == Display::HEIGHT);
        CheckCover(regions, windows);
    }
    // A strip of eight rows across the screen (a refresh band): one window.
    {
        const std::vector<Region> regions{{0, 64, Display::WIDTH, 8}};
        const auto windows = Windows(regions);
        assert(windows.size() == 1 && windows[0].width == Display::WIDTH && windows[0].height == 8);
    }
    // A tall rectangle off the grid: cut from its own left edge, the last piece what is left.
    {
        const std::vector<Region> regions{{30, 0, 60, Display::HEIGHT}};
        const auto windows = Windows(regions);
        assert(windows.size() == 4 && windows[0].x == 30 && windows[0].width == 16 && windows[3].x == 78 &&
               windows[3].width == 12);
        CheckCover(regions, windows);
    }
    // Several rectangles (as PresentChanged finds them, one per band of eight rows, and joined ones): the
    // windows of all of them by their left edge, so the write moves left to right across the screen.
    {
        const std::vector<Region> regions{{0, 0, 240, 8}, {90, 8, 60, 40}, {30, 48, 30, 87}, {210, 120, 30, 15}};
        const auto windows = Windows(regions);
        CheckCover(regions, windows);
        CheckLeftToRight(windows);
        assert(windows.front().x == 0 && windows.front().y == 0);  // the strip first, at x 0 (above the tall one)
    }
    // Pseudo-random rectangles: always covered exactly, always left to right, every window fits a block.
    {
        unsigned seed = 12345;
        const auto next = [&](int below) {
            seed = seed * 1103515245u + 12345u;
            return static_cast<int>((seed >> 8) % static_cast<unsigned>(below));
        };
        for (int round = 0; round < 500; ++round)
        {
            std::vector<Region> regions;
            const int count = 1 + next(static_cast<int>(ScanOrder::kMaxRegions));
            for (int i = 0; i < count; ++i)
            {
                const int x = next(Display::WIDTH), y = next(Display::HEIGHT);
                regions.push_back(Region{x, y, 1 + next(Display::WIDTH - x), 1 + next(Display::HEIGHT - y)});
            }
            const auto windows = Windows(regions);
            CheckCover(regions, windows);
            CheckLeftToRight(windows);
        }
    }
    // More than kMaxRegions: the rest are left for another ScanOrder (Present takes them in turns).
    {
        std::vector<Region> regions(ScanOrder::kMaxRegions + 3, Region{0, 0, 2, 1});
        assert(Windows(regions).size() == ScanOrder::kMaxRegions);
    }
    // Nothing: no window.
    assert(Windows({}).empty());
    std::cout << "scan order ok\n";
    return 0;
}
