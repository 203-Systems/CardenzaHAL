#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "display/region.h"
#include "usb/camera.h"

// The screen stream's pure parts (usb/camera.h, protocol: docs/screen-stream.md): which 8-pixel segments of the
// screen changed since the last frame sent, the rectangles that cover them, the optional run-length code, and
// the frame records. No ESP-IDF: usb/camera.cpp uses them on the board, tests/camera_test.cpp on the host (and
// writes the streams tools/web/screen-stream.js is tested against).
namespace CardenzaHAL::USB::ScreenStream {

inline constexpr int kWidth = Display::WIDTH;
inline constexpr int kHeight = Display::HEIGHT;
inline constexpr int kSegmentPixels = 8;
inline constexpr int kSegments = kWidth / kSegmentPixels;  // 30: a row's segments are bits of one word
inline constexpr int kBandRows = 8;
inline constexpr int kBands = (kHeight + kBandRows - 1) / kBandRows;
static_assert(kWidth % kSegmentPixels == 0 && kSegments <= 32);

// ---- The records (little-endian) -------------------------------------------------------------------------

inline constexpr uint32_t kMagic = 0x535A4443u;     // "CDZS" in the stream's bytes: 43 44 5A 53
inline constexpr uint32_t kEndMagic = 0x455A4443u;  // "CDZE": 43 44 5A 45
inline constexpr uint8_t kVersion = 1;
inline constexpr uint8_t kFormatRgb565 = 1;  // 16 bits a pixel, little-endian: red 15-11, green 10-5, blue 4-0
inline constexpr std::size_t kHeaderBytes = 32;
inline constexpr std::size_t kRectHeaderBytes = 16;
inline constexpr std::size_t kTrailerBytes = 4;
inline constexpr uint8_t kFlagKeyframe = 0x01;  // the frame paints the whole screen: no earlier frame is needed
inline constexpr uint8_t kEncodingRaw = 0;
inline constexpr uint8_t kEncodingRle = 1;  // the run code (below)
// A whole screen as one raw rectangle: the largest frame, and so one bulk transfer (at most 65,535 bytes).
inline constexpr std::size_t kKeyframeBytes =
    kHeaderBytes + kRectHeaderBytes + static_cast<std::size_t>(kWidth) * kHeight * 2 + kTrailerBytes;
inline constexpr std::size_t kMaxFrameBytes = kKeyframeBytes;
static_assert(kMaxFrameBytes <= 65535);

// The control requests (vendor, to the WebUSB interface, on EP0).
enum Request : uint8_t {
    kRequestStart = 0x20,     // OUT, no data. wValue bit 0: the run code may be used
    kRequestStop = 0x21,      // OUT, no data
    kRequestKeyframe = 0x22,  // OUT, no data: the next frame is a keyframe
    kRequestInfo = 0x23,      // IN, 16 bytes (below)
    kRequestStats = 0x24,     // IN, 64 bytes (usb/camera.h Stats, docs/screen-stream.md)
};
inline constexpr uint16_t kStartAllowRle = 0x0001;
inline constexpr std::size_t kInfoBytes = 16;
inline constexpr std::size_t kStatsBytes = 64;

inline void Put16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}
inline void Put32(uint8_t* out, uint32_t value)
{
    for (int index = 0; index < 4; ++index) out[index] = static_cast<uint8_t>(value >> (8 * index));
}
inline uint16_t Get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | in[1] << 8); }
inline uint32_t Get32(const uint8_t* in)
{
    return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
}

// INFO's answer: what the stream sends, for a reader that wants to know before the first frame.
//   0 u8 version, 1 u8 format, 2 u16 width, 4 u16 height, 6 u8 state (bit 0 started, bit 1 waiting for the
//   host to read), 7 u8 reserved, 8 u32 the next frame's sequence, 12 u32 the largest frame in bytes.
inline void WriteInfo(uint8_t* out, bool started, bool stalled, uint32_t nextSequence)
{
    std::memset(out, 0, kInfoBytes);
    out[0] = kVersion;
    out[1] = kFormatRgb565;
    Put16(out + 2, kWidth);
    Put16(out + 4, kHeight);
    out[6] = static_cast<uint8_t>((started ? 1 : 0) | (stalled ? 2 : 0));
    Put32(out + 8, nextSequence);
    Put32(out + 12, static_cast<uint32_t>(kMaxFrameBytes));
}

// STATS' answer (64 bytes): u8 state (bit 0 presented, 1 its endpoint configured, 2 started, 3 stalled), 3 bytes 0,
// then u32 each: frames, keyframes, rectangles, presents, coalesced, bytes, the last frame's bytes, the next
// sequence, capture us (last, most), encode us (last, most), errors; 8 bytes 0.
inline void WriteStats(uint8_t* out, const Camera::Stats& stats)
{
    std::memset(out, 0, kStatsBytes);
    out[0] = static_cast<uint8_t>((stats.enabled ? 1 : 0) | (stats.open ? 2 : 0) | (stats.streaming ? 4 : 0) | (stats.stalled ? 8 : 0));
    const uint32_t values[] = {stats.frames,    stats.keyframes,     stats.rects,    stats.presents,      stats.coalesced,
                               stats.bytes,     stats.lastFrameBytes, stats.sequence, stats.captureUs,     stats.maxCaptureUs,
                               stats.encodeUs,  stats.maxEncodeUs,    stats.errors};
    for (std::size_t index = 0; index < sizeof(values) / sizeof(values[0]); ++index) Put32(out + 4 + 4 * index, values[index]);
}

struct Rect {
    uint16_t x, y, w, h;
};

// ---- What changed ----------------------------------------------------------------------------------------

// The segments of each row that changed since the last frame sent (bit s: pixels 8s to 8s+7).
class Dirty {
  public:
    static constexpr uint32_t kAllSegments = kSegments == 32 ? 0xFFFFFFFFu : (1u << kSegments) - 1;

    void Clear() { std::memset(rows_, 0, sizeof(rows_)); }
    void All()
    {
        for (uint32_t& row : rows_) row = kAllSegments;
    }
    void Mark(int row, uint32_t segments) { rows_[row] |= segments; }
    uint32_t Row(int row) const { return rows_[row]; }
    bool Any() const
    {
        for (uint32_t row : rows_)
            if (row != 0) return true;
        return false;
    }
    bool Everything() const
    {
        for (uint32_t row : rows_)
            if (row != kAllSegments) return false;
        return true;
    }

    // The rectangles over what changed: in each band of eight rows, each run of segments that changed in any of
    // its rows, over the rows where that run changed; a rectangle right under one of the same span joins it.
    // Returns how many (at most `capacity`; more than that: the caller sends a keyframe).
    std::size_t Rects(Rect* out, std::size_t capacity) const
    {
        std::size_t count = 0;
        for (int band = 0; band < kBands; ++band)
        {
            const int top = band * kBandRows;
            const int bottom = top + kBandRows < kHeight ? top + kBandRows : kHeight;
            uint32_t any = 0;
            for (int y = top; y < bottom; ++y) any |= rows_[y];
            int segment = 0;
            while (any >> segment)
            {
                if (!((any >> segment) & 1u))
                {
                    ++segment;
                    continue;
                }
                int end = segment;
                while (end < kSegments && ((any >> end) & 1u)) ++end;
                const uint32_t run = (end - segment == 32 ? 0xFFFFFFFFu : ((1u << (end - segment)) - 1)) << segment;
                int first = -1, last = -1;
                for (int y = top; y < bottom; ++y)
                    if (rows_[y] & run)
                    {
                        if (first < 0) first = y;
                        last = y;
                    }
                const Rect rect{static_cast<uint16_t>(segment * kSegmentPixels), static_cast<uint16_t>(first),
                                static_cast<uint16_t>((end - segment) * kSegmentPixels), static_cast<uint16_t>(last - first + 1)};
                if (!Join(out, count, rect))
                {
                    if (count == capacity) return capacity + 1;
                    out[count++] = rect;
                }
                segment = end;
            }
        }
        return count;
    }

  private:
    // A rectangle that continues one above it (same span, touching) makes it taller.
    static bool Join(Rect* rects, std::size_t count, const Rect& rect)
    {
        for (std::size_t index = count; index-- > 0;)
        {
            Rect& above = rects[index];
            if (above.x == rect.x && above.w == rect.w && above.y + above.h == rect.y)
            {
                above.h = static_cast<uint16_t>(above.h + rect.h);
                return true;
            }
        }
        return false;
    }

    uint32_t rows_[kHeight] = {};
};

// Compares rows [top, bottom) of `frame` with `shadow`, segments [firstSegment, lastSegment) of each, copies the
// segments that differ into the shadow and marks them. Exact (no hash): what the shadow holds is what the
// firmware presented. Both buffers WIDTH * HEIGHT, 4-byte aligned. True when anything changed.
inline bool Capture(const uint16_t* frame, uint16_t* shadow, int top, int bottom, int firstSegment, int lastSegment, Dirty& dirty)
{
    bool any = false;
    for (int y = top; y < bottom; ++y)
    {
        const uint16_t* from = frame + y * kWidth;
        uint16_t* to = shadow + y * kWidth;
        uint32_t changed = 0;
        for (int segment = firstSegment; segment < lastSegment; ++segment)
        {
            const int x = segment * kSegmentPixels;
            uint32_t a[4], b[4];
            // A segment is 16 bytes: four aligned words. __builtin_memcpy is a load, std::memcpy a ROM call under
            // ESP-IDF's -fno-builtin-memcpy.
#if defined(__GNUC__)
            __builtin_memcpy(a, __builtin_assume_aligned(from + x, 4), sizeof(a));
            __builtin_memcpy(b, __builtin_assume_aligned(to + x, 4), sizeof(b));
#else
            std::memcpy(a, from + x, sizeof(a));
            std::memcpy(b, to + x, sizeof(b));
#endif
            if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]) continue;
#if defined(__GNUC__)
            __builtin_memcpy(__builtin_assume_aligned(to + x, 4), a, sizeof(a));
#else
            std::memcpy(to + x, a, sizeof(a));
#endif
            changed |= 1u << segment;
        }
        if (changed != 0)
        {
            dirty.Mark(y, changed);
            any = true;
        }
    }
    return any;
}

// The segments a presented region covers (its columns, rounded out to whole segments).
inline void SegmentSpan(const Display::Region& region, int& first, int& last)
{
    first = region.x / kSegmentPixels;
    last = (region.x + region.width + kSegmentPixels - 1) / kSegmentPixels;
    if (first < 0) first = 0;
    if (last > kSegments) last = kSegments;
}

// ---- The run code -----------------------------------------------------------------------------------------
//
// A rectangle's pixels row after row as one sequence, in chunks: a u16 control, little-endian, whose top two bits
// say what follows and whose low 14 bits are the count of pixels n (1 to 16383):
//   00 literal: n pixels as they are;   10 run: one pixel, n times;   01 pair: two pixels A B, then A B A B ... for
//   n pixels (the UI's 2-pixel dithers and stripes);   11: reserved.
// Runs are taken from 3 equal pixels, pairs from 6 alternating ones (shorter ones cost less as literals). On
// CardenzaSynth's recorded screens it sends 30-45 % of the raw bytes (runs alone: 40-48 %).
inline constexpr uint16_t kChunkLiteral = 0x0000;
inline constexpr uint16_t kChunkRun = 0x8000;
inline constexpr uint16_t kChunkPair = 0x4000;
inline constexpr std::size_t kChunkMax = 0x3FFF;

// Encodes into `out`, at most `capacity` bytes. Returns the bytes, or 0 when it would not fit (then send raw).
// `rows`: the rectangle's rows (each `width` pixels). Walks them in place: no copy, no division per pixel.
inline std::size_t RleEncode(const uint16_t* const* rows, int width, int height, uint8_t* out, std::size_t capacity)
{
    struct Pos {
        int row = 0, column = 0;
    };
    const auto at = [&](const Pos& p) { return rows[p.row][p.column]; };
    const auto step = [&](Pos& p) {
        if (++p.column == width)
        {
            p.column = 0;
            ++p.row;
        }
    };
    const std::size_t total = static_cast<std::size_t>(width) * height;
    std::size_t written = 0, index = 0;
    Pos here, literal;
    std::size_t literalCount = 0;
    const auto flush = [&]() -> bool {
        while (literalCount > 0)
        {
            const std::size_t chunk = literalCount < kChunkMax ? literalCount : kChunkMax;
            if (written + 2 + 2 * chunk > capacity) return false;
            Put16(out + written, static_cast<uint16_t>(kChunkLiteral | chunk));
            written += 2;
            for (std::size_t k = 0; k < chunk; ++k, step(literal))
            {
                Put16(out + written, at(literal));
                written += 2;
            }
            literalCount -= chunk;
        }
        return true;
    };
    while (index < total)
    {
        const uint16_t value = at(here);
        // Equal pixels from here.
        std::size_t run = 1;
        Pos next = here;
        step(next);
        while (index + run < total && run < kChunkMax && at(next) == value)
        {
            ++run;
            step(next);
        }
        if (run >= 3)
        {
            if (!flush() || written + 4 > capacity) return 0;
            Put16(out + written, static_cast<uint16_t>(kChunkRun | run));
            Put16(out + written + 2, value);
            written += 4;
            index += run;
            here = next;
            literal = here;
            continue;
        }
        if (run == 1 && index + 1 < total)
        {
            // Two pixels alternating from here.
            const uint16_t second = at(next);
            std::size_t pair = 2;
            Pos after = next;
            step(after);
            while (index + pair < total && pair < kChunkMax && at(after) == ((pair & 1) ? second : value))
            {
                ++pair;
                step(after);
            }
            if (pair >= 6)
            {
                if (!flush() || written + 6 > capacity) return 0;
                Put16(out + written, static_cast<uint16_t>(kChunkPair | pair));
                Put16(out + written + 2, value);
                Put16(out + written + 4, second);
                written += 6;
                index += pair;
                here = after;
                literal = here;
                continue;
            }
        }
        if (literalCount == 0) literal = here;
        literalCount += run;
        index += run;
        here = next;
    }
    if (!flush()) return 0;
    return written;
}

// Decodes `bytes` into `pixels` (count of them). False when the code is broken or does not fill it exactly.
inline bool RleDecode(const uint8_t* bytes, std::size_t length, uint16_t* pixels, std::size_t count)
{
    std::size_t at = 0, filled = 0;
    while (at < length)
    {
        if (at + 2 > length) return false;
        const uint16_t control = Get16(bytes + at);
        at += 2;
        const std::size_t n = control & kChunkMax;
        if (n == 0 || filled + n > count) return false;
        switch (control & 0xC000)
        {
            case kChunkLiteral:
                if (at + 2 * n > length) return false;
                for (std::size_t k = 0; k < n; ++k, at += 2) pixels[filled++] = Get16(bytes + at);
                break;
            case kChunkRun:
            {
                if (at + 2 > length) return false;
                const uint16_t value = Get16(bytes + at);
                at += 2;
                for (std::size_t k = 0; k < n; ++k) pixels[filled++] = value;
                break;
            }
            case kChunkPair:
            {
                if (at + 4 > length) return false;
                const uint16_t a = Get16(bytes + at), b = Get16(bytes + at + 2);
                at += 4;
                for (std::size_t k = 0; k < n; ++k) pixels[filled++] = k & 1 ? b : a;
                break;
            }
            default: return false;
        }
    }
    return filled == count;
}

// ---- A frame --------------------------------------------------------------------------------------------
//
//   header (32 bytes): u32 magic "CDZS", u8 version, u8 flags (bit 0 keyframe), u16 header bytes (32), u32 frame
//     bytes (the whole record, trailer included), u32 sequence, u16 width, u16 height, u8 format (1: RGB565), u8 0,
//     u16 rectangles, u32 the device's time in ms at the newest frame it holds, u16 frames the firmware presented
//     since the last one sent (they are all in this one), u16 0.
//   each rectangle (16 bytes, then its data padded to 4 bytes): u16 x, u16 y, u16 width, u16 height, u8 encoding
//     (0 raw, 1 the run code), u8 0, u16 0, u32 data bytes (before the padding).
//   trailer: u32 "CDZE".
struct FrameInfo {
    uint32_t sequence = 0;
    uint32_t timeMs = 0;
    uint16_t presents = 0;
    bool keyframe = false;
    bool allowRle = false;
};

// Writes the frame of `rects` from `shadow` into `out` (kMaxFrameBytes of room). Each rectangle goes raw, or
// in the run code when that is smaller and allowed. Returns the frame's bytes; 0 when it does not fit (the caller sends
// a keyframe instead: a whole screen always fits).
inline std::size_t WriteFrame(const uint16_t* shadow, const Rect* rects, std::size_t count, const FrameInfo& info, uint8_t* out,
                              std::size_t capacity)
{
    if (capacity < kHeaderBytes + kTrailerBytes || count > 0xFFFF) return 0;
    std::size_t at = kHeaderBytes;
    for (std::size_t index = 0; index < count; ++index)
    {
        const Rect& rect = rects[index];
        const std::size_t raw = static_cast<std::size_t>(rect.w) * rect.h * 2;
        if (at + kRectHeaderBytes > capacity) return 0;
        uint8_t* head = out + at;
        uint8_t* data = head + kRectHeaderBytes;
        const std::size_t room = capacity - at - kRectHeaderBytes;
        std::size_t bytes = 0;
        uint8_t encoding = kEncodingRaw;
        if (info.allowRle && rect.w * rect.h >= 8)
        {
            const uint16_t* rows[kHeight];
            for (int row = 0; row < rect.h; ++row) rows[row] = shadow + (rect.y + row) * kWidth + rect.x;
            // Worth it only when it saves something: capacity of one byte less than raw.
            bytes = RleEncode(rows, rect.w, rect.h, data, raw - 1 < room ? raw - 1 : room);
            if (bytes != 0) encoding = kEncodingRle;
        }
        if (encoding == kEncodingRaw)
        {
            if (raw > room) return 0;
            for (int row = 0; row < rect.h; ++row)
                std::memcpy(data + static_cast<std::size_t>(row) * rect.w * 2, shadow + (rect.y + row) * kWidth + rect.x,
                            static_cast<std::size_t>(rect.w) * 2);
            bytes = raw;
        }
        const std::size_t padded = (bytes + 3) & ~std::size_t(3);
        if (at + kRectHeaderBytes + padded > capacity) return 0;
        std::memset(data + bytes, 0, padded - bytes);
        std::memset(head, 0, kRectHeaderBytes);
        Put16(head, rect.x);
        Put16(head + 2, rect.y);
        Put16(head + 4, rect.w);
        Put16(head + 6, rect.h);
        head[8] = encoding;
        Put32(head + 12, static_cast<uint32_t>(bytes));
        at += kRectHeaderBytes + padded;
    }
    if (at + kTrailerBytes > capacity) return 0;
    Put32(out + at, kEndMagic);
    at += kTrailerBytes;
    std::memset(out, 0, kHeaderBytes);
    Put32(out, kMagic);
    out[4] = kVersion;
    out[5] = info.keyframe ? kFlagKeyframe : 0;
    Put16(out + 6, kHeaderBytes);
    Put32(out + 8, static_cast<uint32_t>(at));
    Put32(out + 12, info.sequence);
    Put16(out + 16, kWidth);
    Put16(out + 18, kHeight);
    out[20] = kFormatRgb565;
    Put16(out + 22, static_cast<uint16_t>(count));
    Put32(out + 24, info.timeMs);
    Put16(out + 28, info.presents);
    return at;
}

// The bytes a frame of `rects` takes raw (headers included): to choose a keyframe when the pieces would cost
// more than the whole screen.
inline std::size_t RawFrameBytes(const Rect* rects, std::size_t count)
{
    std::size_t bytes = kHeaderBytes + kTrailerBytes;
    for (std::size_t index = 0; index < count; ++index)
        bytes += kRectHeaderBytes + ((static_cast<std::size_t>(rects[index].w) * rects[index].h * 2 + 3) & ~std::size_t(3));
    return bytes;
}

// ---- Reading the stream (tests here; tools/web/screen-stream.js is the reference for computers) -------------

// Applies every whole frame in `bytes` to `screen` (WIDTH * HEIGHT). The stream may begin mid-frame (a reader that
// joined late): bytes before the first magic are skipped. Returns the frames applied; `sequence` the last one's.
class Reader {
  public:
    explicit Reader(uint16_t* screen) : screen_(screen) {}
    // Takes the next bytes of the stream (any split). Returns false when it skipped something (bytes that were not
    // a frame, a broken frame): the reader then looks for the next magic, one byte further each time.
    bool Push(const uint8_t* bytes, std::size_t length, uint32_t* framesApplied = nullptr)
    {
        bool ok = true;
        while (length > 0)
        {
            const std::size_t take = length < sizeof(buffer_) - buffered_ ? length : sizeof(buffer_) - buffered_;
            std::memcpy(buffer_ + buffered_, bytes, take);
            buffered_ += take;
            bytes += take;
            length -= take;
            ok = Scan(framesApplied) && ok;
        }
        return ok;
    }
    uint32_t Sequence() const { return sequence_; }
    bool HaveKeyframe() const { return keyframe_; }

  private:
    void Drop(std::size_t count)
    {
        std::memmove(buffer_, buffer_ + count, buffered_ - count);
        buffered_ -= count;
    }
    bool Scan(uint32_t* framesApplied)
    {
        bool ok = true;
        while (buffered_ >= 12)
        {
            const uint32_t frameBytes = Get32(buffer_ + 8);
            if (Get32(buffer_) != kMagic || frameBytes < kHeaderBytes + kTrailerBytes || frameBytes > kMaxFrameBytes)
            {
                if (Get32(buffer_) == kMagic) ok = false;
                Drop(1);
                continue;
            }
            if (buffered_ < frameBytes) break;
            if (Apply(buffer_, frameBytes))
            {
                if (framesApplied) ++*framesApplied;
                Drop(frameBytes);
            }
            else
            {
                ok = false;
                Drop(1);
            }
        }
        // A buffer full of something that is not a frame: keep only what could start one.
        if (buffered_ == sizeof(buffer_)) Drop(1), ok = false;
        return ok;
    }

    bool Apply(const uint8_t* frame, std::size_t length)
    {
        if (frame[4] != kVersion || Get16(frame + 6) != kHeaderBytes || Get16(frame + 16) != kWidth || Get16(frame + 18) != kHeight ||
            frame[20] != kFormatRgb565 || Get32(frame + length - 4) != kEndMagic)
            return false;
        const bool key = frame[5] & kFlagKeyframe;
        if (!key && !keyframe_) return false;  // a delta before any keyframe paints over nothing
        const uint16_t count = Get16(frame + 22);
        std::size_t at = kHeaderBytes;
        for (uint16_t index = 0; index < count; ++index)
        {
            if (at + kRectHeaderBytes > length - kTrailerBytes) return false;
            const uint8_t* head = frame + at;
            const int x = Get16(head), y = Get16(head + 2), w = Get16(head + 4), h = Get16(head + 6);
            const uint32_t bytes = Get32(head + 12);
            const std::size_t padded = (bytes + 3) & ~std::size_t(3);
            if (x + w > kWidth || y + h > kHeight || at + kRectHeaderBytes + padded > length - kTrailerBytes) return false;
            const uint8_t* data = head + kRectHeaderBytes;
            if (head[8] == kEncodingRaw)
            {
                if (bytes != static_cast<uint32_t>(w * h * 2)) return false;
                for (int row = 0; row < h; ++row)
                    for (int column = 0; column < w; ++column)
                        screen_[(y + row) * kWidth + x + column] = Get16(data + (row * w + column) * 2);
            }
            else if (head[8] == kEncodingRle)
            {
                static uint16_t pixels[kWidth * kHeight];
                if (!RleDecode(data, bytes, pixels, static_cast<std::size_t>(w) * h)) return false;
                for (int row = 0; row < h; ++row) std::memcpy(screen_ + (y + row) * kWidth + x, pixels + row * w, w * 2);
            }
            else
                return false;
            at += kRectHeaderBytes + padded;
        }
        sequence_ = Get32(frame + 12);
        keyframe_ = keyframe_ || key;
        return true;
    }

    uint16_t* screen_;
    uint8_t buffer_[kMaxFrameBytes];
    std::size_t buffered_ = 0;
    uint32_t sequence_ = 0;
    bool keyframe_ = false;
};

}  // namespace CardenzaHAL::USB::ScreenStream
