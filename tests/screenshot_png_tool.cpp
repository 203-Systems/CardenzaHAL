// The screenshot PNG encoder's test frames (screenshot/png.h), for tests/screenshot_png_check.py: writes each
// frame's PNG and its RGB565 pixels (u16 little-endian) into a folder, and the work memory's limits. EncodeRows
// (rows from a callback) must make the very bytes Encode makes; its PNG is written too (<frame>_rows.png).
//   screenshot_png_tool <folder>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "screenshot/png.h"

using CardenzaHAL::Screenshot::Png::Encode;
using CardenzaHAL::Screenshot::Png::EncodeRows;
using CardenzaHAL::Screenshot::Png::WorkBytes;

namespace {

uint32_t g_seed = 12345;
uint32_t Random()
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

uint16_t Rgb(int r, int g, int b)
{
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

struct Frame {
    std::string name;
    int width, height;
    std::vector<uint16_t> pixels;
};

bool ToFile(void* context, const uint8_t* data, std::size_t size)
{
    return std::fwrite(data, 1, size, static_cast<FILE*>(context)) == size;
}

bool Refuse(void* context, const uint8_t*, std::size_t size)
{
    auto* left = static_cast<std::size_t*>(context);
    if (size > *left) return false;
    *left -= size;
    return true;
}

bool ToVector(void* context, const uint8_t* data, std::size_t size)
{
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    out->insert(out->end(), data, data + size);
    return true;
}

// EncodeRows' rows: one buffer, overwritten at every call (valid until the next, as png.h says, so an encoder that
// kept a row pointer would read the wrong row), and the order they are asked in: each pass from row 0, one row on.
struct RowFeed {
    const Frame* frame;
    std::vector<uint16_t> row;
    int last = -1;
    int passes = 1;
    bool ordered = true;
};

const uint16_t* FeedRow(void* context, int y)
{
    auto* feed = static_cast<RowFeed*>(context);
    std::fill(feed->row.begin(), feed->row.end(), uint16_t{0xA5A5});
    if (y < 0 || y >= feed->frame->height)
    {
        feed->ordered = false;
        return feed->row.data();
    }
    if (y != feed->last + 1)
    {
        if (y == 0) ++feed->passes;
        else feed->ordered = false;
    }
    feed->last = y;
    const uint16_t* source = feed->frame->pixels.data() + static_cast<std::size_t>(y) * feed->frame->width;
    std::copy(source, source + feed->frame->width, feed->row.begin());
    return feed->row.data();
}

// The PNG's RGB565 pixels beside it, for the check script.
void WriteRaw(const std::string& path, const std::vector<uint16_t>& pixels)
{
    FILE* raw = std::fopen(path.c_str(), "wb");
    if (raw == nullptr) std::exit(2);
    for (uint16_t p : pixels)
    {
        const uint8_t le[2] = {static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 8)};
        std::fwrite(le, 1, 2, raw);
    }
    std::fclose(raw);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: screenshot_png_tool <folder>\n");
        return 2;
    }
    const std::string folder = argv[1];
    std::vector<Frame> frames;
    const auto add = [&](const std::string& name, int w, int h, auto colour) {
        Frame f{name, w, h, std::vector<uint16_t>(static_cast<std::size_t>(w) * h)};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) f.pixels[static_cast<std::size_t>(y) * w + x] = colour(x, y);
        frames.push_back(std::move(f));
    };
    add("black", 240, 135, [](int, int) { return uint16_t{0}; });
    add("white", 240, 135, [](int, int) { return uint16_t{0xFFFF}; });
    add("two", 240, 135, [](int x, int y) { return ((x / 8 + y / 8) & 1) ? uint16_t{0xFFFF} : uint16_t{0}; });
    add("four", 240, 135, [](int x, int y) { return static_cast<uint16_t>(((x / 5 + y / 3) & 3) * 0x1111); });
    add("sixteen", 240, 135, [](int x, int y) { return Rgb((x * 16 / 240) * 16, (y * 16 / 135) * 16, 128) ; });
    add("ui", 240, 135, [](int x, int y) {
        if (y < 10) return Rgb(255, 90, 54);
        if (x < 6) return ((x + y) & 3) == 0 ? Rgb(54, 214, 230) : uint16_t{0};
        if ((x / 52 + y / 24) % 3 == 0 && (x % 52) > 3 && (y % 24) > 3) return Rgb(72, 152, 255);
        return ((x * 7 + y * 3) % 11 == 0) ? Rgb(232, 232, 224) : uint16_t{0};
    });
    add("c256", 240, 135, [](int x, int y) { return static_cast<uint16_t>(((y * 240 + x) % 256) * 197); });
    add("c257", 240, 135, [](int x, int y) { return static_cast<uint16_t>(((y * 240 + x) % 257) * 197); });
    add("gradient", 240, 135, [](int x, int y) { return Rgb(x, y * 255 / 134, (x + y) & 255); });
    add("noise", 240, 135, [](int, int) { return static_cast<uint16_t>(Random()); });
    add("one", 1, 1, [](int, int) { return Rgb(10, 200, 30); });
    add("odd", 13, 7, [](int x, int y) { return static_cast<uint16_t>((x * 31 + y * 17) & 3); });
    add("tall", 3, 600, [](int x, int y) { return Rgb(x * 80, y % 256, 0); });
    add("wide", 1031, 9, [](int x, int y) { return static_cast<uint16_t>(Random() % 3 == 0 ? x * y : 0x0841 * (x % 7)); });

    int failures = 0;
    for (const Frame& f : frames)
    {
        std::vector<uint8_t> work(WorkBytes(f.width));
        if (work.empty())
        {
            std::fprintf(stderr, "%s: no work size\n", f.name.c_str());
            ++failures;
            continue;
        }
        FILE* png = std::fopen((folder + "/" + f.name + ".png").c_str(), "wb");
        FILE* raw = std::fopen((folder + "/" + f.name + ".rgb565").c_str(), "wb");
        if (png == nullptr || raw == nullptr) return 2;
        const auto result = Encode(f.pixels.data(), f.width, f.height, f.width, work.data(), work.size(), ToFile, png);
        std::fclose(png);
        for (uint16_t p : f.pixels)
        {
            const uint8_t le[2] = {static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 8)};
            std::fwrite(le, 1, 2, raw);
        }
        std::fclose(raw);
        std::printf("%s %dx%d ok=%d indexed=%d depth=%d colours=%d bytes=%zu\n", f.name.c_str(), f.width, f.height,
                    result.ok, result.indexed, result.bitDepth, result.colours, result.bytes);
        if (!result.ok) ++failures;
        // Work memory one byte short of the minimum, and a sink that refuses, both fail cleanly.
        std::vector<uint8_t> small(work.size() / 4);
        if (Encode(f.pixels.data(), f.width, f.height, f.width, small.data(), 64, ToFile, stdout).ok) ++failures;
        std::size_t room = result.bytes / 2;
        if (Encode(f.pixels.data(), f.width, f.height, f.width, work.data(), work.size(), Refuse, &room).ok) ++failures;

        // EncodeRows: the same bytes as Encode, rows asked for in order (two passes at most), the same refusals.
        std::vector<uint8_t> whole, rows;
        Encode(f.pixels.data(), f.width, f.height, f.width, work.data(), work.size(), ToVector, &whole);
        RowFeed feed{&f, std::vector<uint16_t>(f.width)};
        const auto fromRows = EncodeRows(FeedRow, &feed, f.width, f.height, work.data(), work.size(), ToVector, &rows);
        const bool same = fromRows.ok && rows == whole && fromRows.bytes == result.bytes &&
                          fromRows.indexed == result.indexed && fromRows.bitDepth == result.bitDepth &&
                          fromRows.colours == result.colours;
        std::printf("%s rows: ok=%d same=%d passes=%d ordered=%d\n", f.name.c_str(), fromRows.ok, same, feed.passes,
                    feed.ordered);
        if (!same || !feed.ordered || feed.passes > 2) ++failures;
        FILE* rowsPng = std::fopen((folder + "/" + f.name + "_rows.png").c_str(), "wb");
        if (rowsPng == nullptr) return 2;
        std::fwrite(rows.data(), 1, rows.size(), rowsPng);
        std::fclose(rowsPng);
        WriteRaw(folder + "/" + f.name + "_rows.rgb565", f.pixels);
        RowFeed again{&f, std::vector<uint16_t>(f.width)};
        if (EncodeRows(FeedRow, &again, f.width, f.height, small.data(), 64, ToFile, stdout).ok) ++failures;
        room = result.bytes / 2;
        if (EncodeRows(FeedRow, &again, f.width, f.height, work.data(), work.size(), Refuse, &room).ok) ++failures;
    }
    // EncodeRows without rows, or with sizes out of range: refused before anything is written.
    {
        std::vector<uint8_t> work(WorkBytes(240));
        std::vector<uint8_t> out;
        Frame none{"none", 240, 135, std::vector<uint16_t>(240 * 135)};
        RowFeed feed{&none, std::vector<uint16_t>(240)};
        if (EncodeRows(nullptr, &feed, 240, 135, work.data(), work.size(), ToVector, &out).ok) ++failures;
        if (EncodeRows(FeedRow, &feed, 0, 135, work.data(), work.size(), ToVector, &out).ok) ++failures;
        if (EncodeRows(FeedRow, &feed, 240, 4096, work.data(), work.size(), ToVector, &out).ok) ++failures;
        if (EncodeRows(FeedRow, &feed, 240, 135, nullptr, work.size(), ToVector, &out).ok) ++failures;
        if (!out.empty() || feed.last != -1) ++failures;
    }
    // A stride wider than the image: the 240 x 135 frame cut out of a 260-pixel-wide buffer.
    {
        std::vector<uint16_t> wide(260 * 135, 0x1234);
        for (int y = 0; y < 135; ++y)
            for (int x = 0; x < 240; ++x) wide[y * 260 + x] = Rgb(x, y, 7);
        std::vector<uint8_t> work(WorkBytes(240));
        FILE* png = std::fopen((folder + "/stride.png").c_str(), "wb");
        FILE* raw = std::fopen((folder + "/stride.rgb565").c_str(), "wb");
        const auto result = Encode(wide.data(), 240, 135, 260, work.data(), work.size(), ToFile, png);
        std::fclose(png);
        for (int y = 0; y < 135; ++y)
            for (int x = 0; x < 240; ++x)
            {
                const uint16_t p = wide[y * 260 + x];
                const uint8_t le[2] = {static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 8)};
                std::fwrite(le, 1, 2, raw);
            }
        std::fclose(raw);
        std::printf("stride 240x135 ok=%d bytes=%zu\n", result.ok, result.bytes);
        if (!result.ok) ++failures;
    }
    std::printf("work bytes for 240: %zu\n", WorkBytes(240));
    return failures == 0 ? 0 : 1;
}
