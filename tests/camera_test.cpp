// usb/camera_logic.h: the screen stream's capture (exact, by segment), its rectangles, the run code, the frames,
// and a reader that rebuilds the screen from them byte for byte, also from a stream joined mid-frame. With a
// directory argument it also writes a recorded stream (stream.bin) and the screens it must rebuild (frames.bin,
// one WIDTH * HEIGHT RGB565 screen after each frame), for tools/web/screen-stream.js's test
// (tests/screen_stream_test.mjs). It prints what each scene costs.
#include "usb/camera_logic.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace CardenzaHAL::USB::ScreenStream;

namespace {

using Screen = std::vector<uint16_t>;

Screen Blank(uint16_t colour = 0) { return Screen(static_cast<std::size_t>(kWidth) * kHeight, colour); }

void Fill(Screen& s, int x, int y, int w, int h, uint16_t colour)
{
    for (int row = y; row < y + h && row < kHeight; ++row)
        for (int column = x; column < x + w && column < kWidth; ++column)
            if (row >= 0 && column >= 0) s[row * kWidth + column] = colour;
}

// A 4x4 Bayer dither of `colour` over black at `level` of 16, as the groovebox fills areas.
void Dither(Screen& s, int x, int y, int w, int h, uint16_t colour, int level)
{
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int row = y; row < y + h && row < kHeight; ++row)
        for (int column = x; column < x + w && column < kWidth; ++column)
            s[row * kWidth + column] = bayer[row & 3][column & 3] < level ? colour : 0;
}

// Text-like marks: short strokes in a cell.
void Glyphs(Screen& s, int x, int y, int count, uint16_t colour, unsigned seed)
{
    std::mt19937 random(seed);
    for (int index = 0; index < count; ++index)
        for (int stroke = 0; stroke < 4; ++stroke)
        {
            const int gx = x + index * 6 + static_cast<int>(random() % 4), gy = y + static_cast<int>(random() % 6);
            Fill(s, gx, gy, 1 + static_cast<int>(random() % 3), 1 + static_cast<int>(random() % 3), colour);
        }
}

// A sequencer-like page: a header, a 4x4 grid of cells with labels and dithered fills, a footer.
Screen Page(unsigned seed)
{
    Screen s = Blank();
    std::mt19937 random(seed);
    Fill(s, 0, 0, kWidth, 10, 0x0000);
    Glyphs(s, 4, 2, 20, 0xE71C, seed);
    Fill(s, 0, 11, kWidth, 1, 0x39E7);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
        {
            const int x = 10 + column * 55, y = 14 + row * 27;
            if (random() % 3 == 0) Dither(s, x, y, 52, 24, 0x07E0, 4 + static_cast<int>(random() % 8));
            Glyphs(s, x + 2, y + 2, 2, 0x8410, seed + row * 4 + column);
            Glyphs(s, x + 2, y + 12, 3, 0xFFFF, seed * 7 + row * 4 + column);
            Fill(s, x, y + 22, 50, 2, 0x39E7);
        }
    Fill(s, 0, 125, kWidth, 10, 0x0000);
    Glyphs(s, 10, 127, 18, 0xA01F, seed + 99);
    return s;
}

// The playhead: a 2-pixel column over the grid at step `step` of 16.
Screen Playhead(const Screen& page, int step)
{
    Screen s = page;
    Fill(s, 10 + (step % 16) * 13, 14, 2, 108, 0x07E0);
    return s;
}

// A full-screen animation: a moving gradient with noise.
Screen Animation(int frame)
{
    Screen s = Blank();
    std::mt19937 random(static_cast<unsigned>(frame));
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
        {
            const int r = (x + frame * 3) & 31, g = (y * 2 + frame) & 63, b = static_cast<int>(random() & 3) + ((x ^ y) & 15);
            s[y * kWidth + x] = static_cast<uint16_t>(r << 11 | g << 5 | (b & 31));
        }
    return s;
}

// ---- The encoder as the device runs it (the hook, then the task), on the host ----------------------------

struct Encoder {
    Screen shadow = Blank();
    Dirty dirty;
    bool haveFrame = false, keyPending = true;
    uint32_t sequence = 0;
    uint16_t presents = 0;
    bool allowRle = true;
    std::vector<uint8_t> frame = std::vector<uint8_t>(kMaxFrameBytes);

    // A Present of the whole screen (candidates: everything).
    void Present(const Screen& screen)
    {
        alignas(4) static uint16_t aligned[kWidth * kHeight];
        std::memcpy(aligned, screen.data(), sizeof(aligned));
        Capture(aligned, shadow.data(), 0, kHeight, 0, kSegments, dirty);
        haveFrame = true;
        ++presents;
    }
    // The task: the next frame, or empty.
    std::vector<uint8_t> Next()
    {
        if (!haveFrame || (!keyPending && !dirty.Any())) return {};
        static Rect rects[96];
        const Rect whole{0, 0, kWidth, kHeight};
        std::size_t count = 0;
        bool key = keyPending || dirty.Everything();
        if (!key)
        {
            count = dirty.Rects(rects, 96);
            key = count > 96 || RawFrameBytes(rects, count) >= kKeyframeBytes;
        }
        FrameInfo info;
        info.sequence = sequence++;
        info.presents = presents;
        info.allowRle = allowRle;
        info.keyframe = key;
        std::size_t bytes = key ? 0 : WriteFrame(shadow.data(), rects, count, info, frame.data(), frame.size());
        if (bytes == 0)
        {
            info.keyframe = true;
            bytes = WriteFrame(shadow.data(), &whole, 1, info, frame.data(), frame.size());
        }
        assert(bytes > 0 && bytes <= kMaxFrameBytes);
        dirty.Clear();
        keyPending = false;
        presents = 0;
        return std::vector<uint8_t>(frame.begin(), frame.begin() + static_cast<long>(bytes));
    }
};

void RunCodeTests()
{
    std::mt19937 random(7);
    const auto roundTrip = [&](const std::vector<uint16_t>& pixels, int width) {
        const int height = static_cast<int>(pixels.size()) / width;
        std::vector<const uint16_t*> rows;
        for (int row = 0; row < height; ++row) rows.push_back(pixels.data() + row * width);
        std::vector<uint8_t> code(pixels.size() * 2 + 64);
        const std::size_t bytes = RleEncode(rows.data(), width, height, code.data(), code.size());
        if (bytes == 0) return std::size_t(0);
        std::vector<uint16_t> back(pixels.size());
        assert(RleDecode(code.data(), bytes, back.data(), back.size()));
        assert(back == pixels);
        // A byte less than it needs: it says so (0), never writes past.
        std::vector<uint8_t> tight(bytes - 1 + 8, 0xAA);
        assert(RleEncode(rows.data(), width, height, tight.data(), bytes - 1) == 0);
        for (std::size_t index = bytes - 1; index < tight.size(); ++index) assert(tight[index] == 0xAA);
        return bytes;
    };
    // Flat: one run per 16383 pixels.
    assert(roundTrip(std::vector<uint16_t>(kWidth * kHeight, 0x1234), kWidth) == 4 * ((kWidth * kHeight + kChunkMax - 1) / kChunkMax));
    // Two-pixel dither: pairs.
    std::vector<uint16_t> dither(kWidth * 8);
    for (std::size_t index = 0; index < dither.size(); ++index) dither[index] = index & 1 ? 0x07E0 : 0;
    assert(roundTrip(dither, kWidth) == 6);
    // Noise: literals (and every short mix).
    for (int trial = 0; trial < 300; ++trial)
    {
        const int width = 1 + static_cast<int>(random() % 64), height = 1 + static_cast<int>(random() % 20);
        std::vector<uint16_t> pixels(static_cast<std::size_t>(width) * height);
        const int palette = 1 + static_cast<int>(random() % 4);
        for (auto& pixel : pixels) pixel = static_cast<uint16_t>(random() % palette);
        roundTrip(pixels, width);
    }
    // Long literals cross the 14-bit count.
    std::vector<uint16_t> noise(kWidth * kHeight);
    for (std::size_t index = 0; index < noise.size(); ++index) noise[index] = static_cast<uint16_t>(index * 2654435761u >> 7);
    roundTrip(noise, kWidth);
    // Broken codes are refused.
    uint16_t out[4];
    const uint8_t zeroCount[] = {0x00, 0x80, 0x34, 0x12};
    assert(!RleDecode(zeroCount, sizeof(zeroCount), out, 4));
    const uint8_t tooMany[] = {0x05, 0x80, 0x34, 0x12};
    assert(!RleDecode(tooMany, sizeof(tooMany), out, 4));
    const uint8_t reserved[] = {0x04, 0xC0, 0x34, 0x12};
    assert(!RleDecode(reserved, sizeof(reserved), out, 4));
    const uint8_t shortRun[] = {0x04, 0x80, 0x34};
    assert(!RleDecode(shortRun, sizeof(shortRun), out, 4));
}

void CaptureAndRectTests()
{
    alignas(4) static uint16_t frame[kWidth * kHeight], shadow[kWidth * kHeight];
    std::memset(frame, 0, sizeof(frame));
    std::memset(shadow, 0, sizeof(shadow));
    Dirty dirty;
    assert(!Capture(frame, shadow, 0, kHeight, 0, kSegments, dirty) && !dirty.Any());
    // One pixel: one segment of one row, exactly.
    frame[50 * kWidth + 100] = 1;
    assert(Capture(frame, shadow, 0, kHeight, 0, kSegments, dirty));
    assert(dirty.Row(50) == 1u << (100 / kSegmentPixels) && std::memcmp(frame, shadow, sizeof(frame)) == 0);
    Rect rects[8];
    assert(dirty.Rects(rects, 8) == 1);
    assert(rects[0].x == 96 && rects[0].y == 50 && rects[0].w == 8 && rects[0].h == 1);
    // Outside the segments asked for: not seen (the presenter said it did not change).
    dirty.Clear();
    frame[10] = 7;
    assert(!Capture(frame, shadow, 0, kHeight, 2, kSegments, dirty));
    assert(Capture(frame, shadow, 0, 1, 0, 2, dirty));

    // Random changes: the rectangles cover every dirty segment, stay on the screen and do not overlap.
    std::mt19937 random(3);
    for (int trial = 0; trial < 500; ++trial)
    {
        Dirty d;
        const int marks = 1 + static_cast<int>(random() % 40);
        for (int index = 0; index < marks; ++index) d.Mark(static_cast<int>(random() % kHeight), 1u << (random() % kSegments));
        if (trial % 50 == 0) d.All();
        static Rect r[512];
        const std::size_t count = d.Rects(r, 512);
        assert(count <= 512);
        static bool covered[kHeight][kSegments];
        std::memset(covered, 0, sizeof(covered));
        for (std::size_t index = 0; index < count; ++index)
        {
            assert(r[index].x % kSegmentPixels == 0 && r[index].w % kSegmentPixels == 0 && r[index].w > 0 && r[index].h > 0);
            assert(r[index].x + r[index].w <= kWidth && r[index].y + r[index].h <= kHeight);
            for (int y = r[index].y; y < r[index].y + r[index].h; ++y)
                for (int s = r[index].x / kSegmentPixels; s < (r[index].x + r[index].w) / kSegmentPixels; ++s)
                {
                    assert(!covered[y][s]);
                    covered[y][s] = true;
                }
        }
        for (int y = 0; y < kHeight; ++y)
            for (int s = 0; s < kSegments; ++s)
                if (d.Row(y) & (1u << s)) assert(covered[y][s]);
    }
    // Everything: whole bands joined into one rectangle, the whole screen.
    Dirty all;
    all.All();
    assert(all.Everything() && all.Rects(rects, 8) == 1 && rects[0].w == kWidth && rects[0].h == kHeight);
    // Too many rectangles: says so.
    Dirty checker;
    for (int y = 0; y < kHeight; ++y) checker.Mark(y, 0x15555555u);
    assert(checker.Rects(rects, 8) == 9);
}

// Sends `screens` through the encoder, `every`th present read by the computer (coalescing), and checks that the
// reader's screen equals the shadow after every frame. Returns the bytes sent; appends to the recording.
std::size_t RunScene(const std::vector<Screen>& screens, int every, bool allowRle, std::vector<uint8_t>* stream,
                     std::vector<uint16_t>* expected, double* encodeUs = nullptr, std::size_t* firstFrame = nullptr)
{
    Encoder encoder;
    encoder.allowRle = allowRle;
    Screen screen = Blank(0xFFFF);
    Reader reader(screen.data());
    std::size_t bytes = 0;
    double us = 0;
    int sent = 0;
    for (std::size_t index = 0; index < screens.size(); ++index)
    {
        encoder.Present(screens[index]);
        if (static_cast<int>(index) % every != every - 1 && index + 1 != screens.size()) continue;
        const auto start = std::chrono::steady_clock::now();
        const std::vector<uint8_t> frame = encoder.Next();
        us += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        if (frame.empty()) continue;
        ++sent;
        if (sent == 1 && firstFrame) *firstFrame = frame.size();
        bytes += frame.size();
        uint32_t applied = 0;
        // Fed in pieces of every size, as a reader's transfers may split it.
        std::size_t at = 0;
        while (at < frame.size())
        {
            const std::size_t piece = std::min<std::size_t>(frame.size() - at, 1 + (at * 7919 + index) % 4096);
            assert(reader.Push(frame.data() + at, piece, &applied));
            at += piece;
        }
        assert(applied == 1);
        assert(screen == encoder.shadow);
        assert(screen == screens[index]);
        if (stream) stream->insert(stream->end(), frame.begin(), frame.end());
        if (expected) expected->insert(expected->end(), screen.begin(), screen.end());
    }
    if (encodeUs) *encodeUs = sent ? us / sent : 0;
    return bytes;
}

void PipelineTests(const char* directory)
{
    const Screen page = Page(1), other = Page(2);
    // Idle: one keyframe, then nothing.
    std::vector<Screen> idle(50, page);
    // The playhead walking over the grid at 50 fps (a step every 6 frames).
    std::vector<Screen> playhead;
    for (int index = 0; index < 100; ++index) playhead.push_back(Playhead(page, index / 6));
    // A page change.
    std::vector<Screen> change = {page, page, other, other};
    // Full-screen animation.
    std::vector<Screen> animation;
    for (int index = 0; index < 20; ++index) animation.push_back(Animation(index));

    struct Scene {
        const char* name;
        const std::vector<Screen>* screens;
    } scenes[] = {{"idle", &idle}, {"playhead", &playhead}, {"page change", &change}, {"full-screen animation", &animation}};
    std::vector<uint8_t> stream;
    std::vector<uint16_t> expected;
    for (const Scene& scene : scenes)
        for (int every : {1, 3})
        {
            double encodeUs = 0;
            std::size_t rawKey = 0, codedKey = 0;
            const std::size_t raw = RunScene(*scene.screens, every, false, nullptr, nullptr, nullptr, &rawKey) - rawKey;
            const std::size_t coded =
                RunScene(*scene.screens, every, true, every == 1 ? &stream : nullptr, every == 1 ? &expected : nullptr, &encodeUs, &codedKey) -
                codedKey;
            // After the first frame (the keyframe), at 50 presents a second.
            const double seconds = (scene.screens->size() - 1) / 50.0;
            if (every == 1)
                std::printf("  %-22s keyframe raw %5zu coded %5zu B | then raw %7.1f KB/s, coded %7.1f KB/s (%3.0f %%) | %4.0f us a frame (host)\n",
                            scene.name, rawKey, codedKey, raw / 1024.0 / seconds, coded / 1024.0 / seconds, raw ? 100.0 * coded / raw : 0.0,
                            encodeUs);
        }

    // The idle scene sends its keyframe and nothing after.
    {
        Encoder encoder;
        encoder.Present(page);
        const auto key = encoder.Next();
        assert(!key.empty() && (key[5] & kFlagKeyframe));
        for (int index = 0; index < 10; ++index)
        {
            encoder.Present(page);
            assert(encoder.Next().empty());
        }
    }
    // Sequence numbers count frames and wrap.
    {
        Encoder encoder;
        encoder.sequence = 0xFFFFFFFEu;
        Screen screen = Blank();
        Reader reader(screen.data());
        for (int index = 0; index < 4; ++index)
        {
            encoder.Present(Playhead(page, index));
            const auto frame = encoder.Next();
            assert(reader.Push(frame.data(), frame.size()));
            assert(reader.Sequence() == 0xFFFFFFFEu + static_cast<uint32_t>(index));
        }
    }
    // A reader that joins mid-stream skips to the next frame, ignores deltas until a keyframe, then follows.
    {
        Encoder encoder;
        std::vector<uint8_t> all;
        std::vector<std::size_t> starts;
        for (int index = 0; index < 12; ++index)
        {
            if (index == 6) encoder.keyPending = true;  // the reader asks for one (kRequestKeyframe)
            encoder.Present(Playhead(index < 9 ? page : other, index));
            starts.push_back(all.size());
            const auto frame = encoder.Next();
            all.insert(all.end(), frame.begin(), frame.end());
        }
        Screen screen = Blank();
        Reader reader(screen.data());
        const std::size_t join = starts[2] + 1000;  // inside frame 2
        reader.Push(all.data() + join, starts[6] - join);
        assert(!reader.HaveKeyframe());
        assert(reader.Push(all.data() + starts[6], all.size() - starts[6]));
        assert(reader.HaveKeyframe() && screen == encoder.shadow);
    }
    // Garbage between frames (a reader's stale transfer) does not stop it.
    {
        Encoder encoder;
        Screen screen = Blank();
        Reader reader(screen.data());
        encoder.Present(page);
        auto frame = encoder.Next();
        std::vector<uint8_t> junk(300, 0x43);
        junk.insert(junk.end(), frame.begin(), frame.end());
        reader.Push(junk.data(), junk.size());
        assert(screen == page);
    }

    if (directory != nullptr)
    {
        const std::string dir(directory);
        FILE* file = std::fopen((dir + "/stream.bin").c_str(), "wb");
        assert(file != nullptr);
        std::fwrite(stream.data(), 1, stream.size(), file);
        std::fclose(file);
        file = std::fopen((dir + "/frames.bin").c_str(), "wb");
        assert(file != nullptr);
        std::fwrite(expected.data(), 2, expected.size(), file);
        std::fclose(file);
        std::printf("  wrote %s/stream.bin (%zu bytes, %zu frames)\n", directory, stream.size(), expected.size() / (kWidth * kHeight));
    }
}

void InfoTests()
{
    uint8_t info[kInfoBytes];
    WriteInfo(info, true, false, 42);
    assert(info[0] == kVersion && info[1] == kFormatRgb565 && Get16(info + 2) == kWidth && Get16(info + 4) == kHeight);
    assert(info[6] == 1 && Get32(info + 8) == 42 && Get32(info + 12) == kMaxFrameBytes);
    CardenzaHAL::USB::Camera::Stats stats;
    stats.streaming = true;
    stats.frames = 5;
    stats.errors = 9;
    uint8_t out[kStatsBytes];
    WriteStats(out, stats);
    assert(out[0] == 4 && Get32(out + 4) == 5 && Get32(out + 52) == 9);
    static_assert(kKeyframeBytes == 64852);
}

}  // namespace

int main(int argc, char** argv)
{
    RunCodeTests();
    CaptureAndRectTests();
    InfoTests();
    PipelineTests(argc > 1 ? argv[1] : nullptr);
    std::cout << "camera tests passed\n";
    return 0;
}
