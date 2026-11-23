// The presenter's benchmark: what PresentChanged finds and sends for typical screens (the HAL's own
// display/changed_regions.h and display/scan_order.h), how long finding it takes on this computer, and a model
// of the board: SPI time per frame and the chance that a frame tears, before this presenter (8-row bands at
// 40 MHz, each band waiting for the next RTOS tick) and with it (scan-order windows at 80 MHz).
//
//   CardenzaDisplayBench            the table (200 frames of each screen)
//   CardenzaDisplayBench --quick    a few frames, checks the model's sums (ctest)
//
// The board model's numbers are stated below with where they come from; FrameStats (display/presenter.h)
// measures the real ones on a board.

#include "display/changed_regions.h"
#include "display/scan_order.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Display = CardenzaHAL::Display;
using Display::ChangedRegions;
using Display::Region;
using Display::ScanOrder;

namespace {

constexpr int W = Display::WIDTH, H = Display::HEIGHT;

// ---- the board model ----------------------------------------------------------------------------------
// ESP-IDF's figures for the ESP32-S3 (spi_master.rst, "Transaction Duration"): a polled transaction about
// 11 us, an interrupt transaction about 26 us. A window is CASET and RASET polled, RAMWR and the pixels queued:
// with the task's wake-up, about 65 us of a core.
constexpr double kWindowOverheadUs = 65;
// Copying out of the framebuffer into a DMA block: PSRAM through the cache, about 40 MB/s (quad at 120 MHz;
// an estimate: FrameStats::sendUs gives the board's); internal RAM about 400 MB/s.
constexpr double kCopyPsramUsPerByte = 1.0 / 40;
constexpr double kCopyInternalUsPerByte = 1.0 / 400;
// The panel (ST7789 defaults: 60 Hz, 320 gate lines and two porches of 12): a refresh every 16.67 ms, a line
// (one of the screen's columns in landscape) every 48.4 us, scanned from x = 0 (the 203 Launcher, 2026-11-06).
constexpr double kRefreshUs = 1e6 / 60;
constexpr double kLineUs = kRefreshUs / (320 + 24);
// Before: the HAL waited for each 8-row band on a semaphore its DMA interrupt gave without asking for a
// switch, so the task woke at the next tick (1 ms at the board's 1000 Hz): CardenzaSynth measured 17 ms a
// whole screen (2026-11-05), which this reproduces.
constexpr double kTickUs = 1000;

struct Window {
    Region r;
    double startUs;  // its first pixel goes out
    double pixelUs;  // a pixel's time on the wire
};

struct Send {
    double us = 0;         // until the last pixel is on the panel
    int windows = 0;
    long bytes = 0;
    double tearChance = 0; // of a refresh showing part of this frame and part of the last
};

// The chance that a refresh meets the write: a refresh starting at s shows pixel p new iff s > d(p), with
// d(p) = (when p is written) - (when the scan reads its column, from the refresh's start). It shows a mix iff
// a start falls between the smallest and the largest d: (max d - min d) / refresh period, at most 1.
double TearChance(const std::vector<Window>& windows)
{
    if (windows.empty()) return 0;
    double low = 1e18, high = -1e18;
    for (const auto& w : windows)
    {
        // Written row by row: d is smallest at the first row's last column, largest at the last row's first.
        const double first = w.startUs + (w.r.width - 1) * w.pixelUs - (w.r.x + w.r.width - 1) * kLineUs;
        const double last = w.startUs + ((w.r.height - 1) * w.r.width) * w.pixelUs - w.r.x * kLineUs;
        low = std::min({low, first, last});
        high = std::max({high, first, last});
    }
    return std::min(1.0, (high - low) / kRefreshUs);
}

// Before: each region in bands of eight rows at 40 MHz from a PSRAM frame, copy then send, every band ending
// at the next tick (the frame starts at a random point of a tick: half a tick on average).
Send SendBefore(const Region* regions, std::size_t count, double tickUs = kTickUs)
{
    Send send;
    std::vector<Window> windows;
    const double pixelUs = 16.0 / 40;  // 16 bits at 40 MHz
    double t = tickUs / 2;
    const double start = t;
    for (std::size_t i = 0; i < count; ++i)
        for (int y = regions[i].y; y < regions[i].y + regions[i].height; y += 8)
        {
            const Region band{regions[i].x, y, regions[i].width, std::min(8, regions[i].y + regions[i].height - y)};
            const long bytes = 2L * band.width * band.height;
            t += bytes * kCopyPsramUsPerByte + kWindowOverheadUs;
            windows.push_back(Window{band, t, pixelUs});
            t += band.width * band.height * pixelUs;
            t = std::ceil(t / tickUs) * tickUs;  // woken at the next tick
            ++send.windows;
            send.bytes += bytes;
        }
    send.us = t - start;
    send.tearChance = TearChance(windows);
    return send;
}

// With this presenter: scan-order windows at `mhz`, waking as soon as a window is done; with two blocks the
// next window is copied while the last one goes out.
Send SendNow(const Region* regions, std::size_t count, double mhz, bool pipeline, double copyUsPerByte)
{
    Send send;
    std::vector<Window> windows;
    const double pixelUs = 16.0 / mhz;
    double t = 0, busFree = 0;
    for (std::size_t first = 0; first < count; first += ScanOrder::kMaxRegions)
    {
        ScanOrder order(regions + first, count - first);
        Region r;
        while (order.Next(r))
        {
            const long bytes = 2L * r.width * r.height;
            const double copy = bytes * copyUsPerByte;
            // Two blocks: the copy runs while the bus is busy; one: after it.
            t = pipeline ? std::max(t + copy, busFree) : std::max(t, busFree) + copy;
            t += kWindowOverheadUs;
            windows.push_back(Window{r, t, pixelUs});
            busFree = t + r.width * r.height * pixelUs;
            ++send.windows;
            send.bytes += bytes;
        }
    }
    send.us = std::max(t, busFree);
    send.tearChance = TearChance(windows);
    return send;
}

// ---- the screens ----------------------------------------------------------------------------------------

using Frame = std::array<uint16_t, W * H>;

void Fill(Frame& f, int x, int y, int w, int h, uint16_t c)
{
    for (int row = std::max(0, y); row < std::min(H, y + h); ++row)
        for (int col = std::max(0, x); col < std::min(W, x + w); ++col) f[row * W + col] = c;
}

// A UI that stands still: a header, text-like marks, a grid of cells.
void Background(Frame& f)
{
    f.fill(0x0000);
    Fill(f, 0, 0, W, 10, 0x2104);
    for (int i = 0; i < 40; ++i) Fill(f, 6 + (i % 10) * 23, 16 + (i / 10) * 28, 18, 7, static_cast<uint16_t>(0x8410 + i));
}

struct Screen {
    const char* name;
    const char* what;
    void (*draw)(Frame& f, int t);
};

const Screen kScreens[] = {
    {"still", "nothing moves", [](Frame& f, int) { Background(f); }},
    {"counter", "a 60 x 14 number changes", [](Frame& f, int t) {
         Background(f);
         Fill(f, 170, 2, 60, 14, 0x0000);
         for (int digit = 0; digit < 5; ++digit)
             Fill(f, 172 + digit * 12, 4, 9, 10, static_cast<uint16_t>(0xffff - ((t >> digit) & 7) * 1111));
     }},
    {"sprite", "a 24 x 24 sprite moves 2 px a frame", [](Frame& f, int t) {
         Background(f);
         Fill(f, (t * 2) % (W - 24), 60, 24, 24, 0xf800);
     }},
    {"list", "a 240 x 18 selection bar slides 3 px a frame", [](Frame& f, int t) {
         Background(f);
         Fill(f, 0, 12 + (t * 3) % (H - 30), W, 18, 0xffff);
     }},
    {"scroll", "the whole screen scrolls 1 px a frame", [](Frame& f, int t) {
         for (int y = 0; y < H; ++y)
             for (int x = 0; x < W; ++x) f[y * W + x] = static_cast<uint16_t>(((x + t) / 8 + y / 8) % 2 ? 0x07e0 : 0x001f);
     }},
    {"pages", "a new page every 25 frames", [](Frame& f, int t) {
         const int page = t / 25;
         for (int y = 0; y < H; ++y)
             for (int x = 0; x < W; ++x) f[y * W + x] = static_cast<uint16_t>((x / (4 + page % 5)) * 31 + y * page);
     }},
};

struct Row {
    double findHostUs = 0, regions = 0, before = 0, beforeTear = 0, now = 0, nowTear = 0, single = 0, internal = 0;
    double windows = 0, bytes = 0;
};

}  // namespace

int main(int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    const int frames = quick ? 30 : 200;

    // The model reproduces what was measured before: a whole screen at 40 MHz, 17 bands, 17 ms.
    const Region whole{0, 0, W, H};
    const Send beforeWhole = SendBefore(&whole, 1);
    const Send nowWhole = SendNow(&whole, 1, 80, true, kCopyPsramUsPerByte);
    assert(beforeWhole.windows == 17 && std::abs(beforeWhole.us - 17000) < 600);
    assert(nowWhole.windows == 15 && nowWhole.bytes == 2L * W * H);
    assert(beforeWhole.tearChance == 1.0 && nowWhole.tearChance < 0.5);

    std::printf("Whole screen (64,800 bytes): before %.1f ms in %d bands; now %.1f ms in %d windows (80 MHz, two blocks)\n",
                beforeWhole.us / 1000, beforeWhole.windows, nowWhole.us / 1000, nowWhole.windows);
    std::printf("  one block %.1f ms; at 40 MHz %.1f ms; before, at ESP-IDF's default tick of 100 Hz: %.0f ms (now the same at any tick)\n",
                SendNow(&whole, 1, 80, false, kCopyPsramUsPerByte).us / 1000,
                SendNow(&whole, 1, 40, true, kCopyPsramUsPerByte).us / 1000, SendBefore(&whole, 1, 10000).us / 1000);
    std::printf("  tear chance of a whole-screen change: before %.0f%% (a diagonal), now %.0f%% (a straight vertical line)\n\n",
                beforeWhole.tearChance * 100, nowWhole.tearChance * 100);

    std::printf("%-8s %-44s %7s %6s %6s %8s | %8s %6s | %8s %6s %8s %8s\n", "screen", "", "find", "rects", "windows",
                "bytes", "before", "tear", "now", "tear", "1 block", "internal");
    std::printf("%-8s %-44s %7s %6s %6s %8s | %8s %6s | %8s %6s %8s %8s\n", "", "", "us host", "/frame", "/frame", "/frame",
                "ms", "chance", "ms", "chance", "ms", "frame ms");
    for (const Screen& screen : kScreens)
    {
        Frame frame{};
        std::vector<uint32_t> hashes(ChangedRegions::kHashCount);
        std::vector<uint32_t> changeHashes(ChangedRegions::kHashCount);
        ChangedRegions finder(hashes.data());
        ChangedRegions changes(changeHashes.data(), ChangedRegions::kSegmentPixels, 0);
        Region regions[ChangedRegions::kMaxRegions], changed[ChangedRegions::kMaxRegions];
        screen.draw(frame, 0);
        (void)finder.Find(frame.data(), regions);  // the first frame is whole: not counted
        (void)changes.Find(frame.data(), changed);
        Row row;
        for (int t = 1; t <= frames; ++t)
        {
            screen.draw(frame, t);
            const auto started = std::chrono::steady_clock::now();
            const std::size_t count = finder.Find(frame.data(), regions);
            row.findHostUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count();
            const Send before = SendBefore(regions, count);
            const Send now = SendNow(regions, count, 80, true, kCopyPsramUsPerByte);
            row.regions += static_cast<double>(count);
            row.windows += now.windows;
            row.bytes += static_cast<double>(now.bytes);
            row.before += before.us;
            row.now += now.us;
            row.single += SendNow(regions, count, 80, false, kCopyPsramUsPerByte).us;
            row.internal += SendNow(regions, count, 80, true, kCopyInternalUsPerByte).us;
            assert(now.bytes == before.bytes);  // the same pixels, cut differently
            // The tear chance of what changed: the refresh band is resent unchanged and shows the same pixels
            // either way (what changed is what a finder without refresh bands finds).
            const std::size_t changedCount = changes.Find(frame.data(), changed);
            row.beforeTear += SendBefore(changed, changedCount).tearChance;
            row.nowTear += SendNow(changed, changedCount, 80, true, kCopyPsramUsPerByte).tearChance;
        }
        const double n = frames;
        std::printf("%-8s %-44s %7.1f %6.1f %6.1f %8.0f | %8.2f %5.0f%% | %8.2f %5.0f%% %8.2f %8.2f\n", screen.name, screen.what,
                    row.findHostUs / n, row.regions / n, row.windows / n, row.bytes / n, row.before / n / 1000,
                    row.beforeTear / n * 100, row.now / n / 1000, row.nowTear / n * 100, row.single / n / 1000,
                    row.internal / n / 1000);
    }
    std::printf("\nfind: hashing the frame on this computer (CardenzaSynth measured 0.4 ms on the board with its frame in\n"
                "internal RAM; from PSRAM expect more, FrameStats::findUs says). Times include the refresh band (one\n"
                "band of 8 rows resent every frame). before: 8-row bands, 40 MHz, each band woken at the next 1 ms tick.\n"
                "now: scan-order windows at 80 MHz from a PSRAM frame, two DMA blocks; 1 block: one block; internal: the\n"
                "frame in internal RAM. tear chance: of a refresh showing part of the frame and part of the one before.\n");
    return 0;
}
