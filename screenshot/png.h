#pragma once

#include <cstddef>
#include <cstdint>

// A small PNG encoder for screenshots (screenshot/screenshot.h): the screen's RGB565 pixels, exactly, as a PNG a
// browser or any image tool opens. Pure (no allocation, and no ESP-IDF but the ROM's CRC-32 on the device): the host
// tests check it against Python's zlib (tests/screenshot_png_check.py), and the 203 Launcher carries the same file
// (src/screenshot/, with a first line naming the HAL commit it matches): change both.
//
// - A frame of 256 colours or fewer (most UI art) is an indexed PNG (colour type 3, 1, 2, 4 or 8 bits a pixel,
//   the palette in the order the colours first appear); any other is 8-bit RGB (colour type 2) with each row's
//   filter chosen by the usual heuristic (the least sum of absolute differences of None, Sub, Up, Average, Paeth).
// - RGB565 is widened to 8 bits a channel by repeating its top bits (r8 = r5 << 3 | r5 >> 2, g8 = g6 << 2 |
//   g6 >> 4), so white stays 255 and the 565 value can be read back from the PNG.
// - The pixels are compressed with deflate (fixed Huffman codes, LZ77 over a window that fits the work memory,
//   looking at the same pixel one and two rows up as well as a hash of what came before), in one zlib stream
//   cut into IDAT chunks of at most kChunkBytes as the output comes.
// - Streaming: the PNG goes out through `sink` a chunk at a time; nothing of the image is kept but the window.
namespace CardenzaHAL::Screenshot::Png {

    // Writes `size` bytes of the PNG; false stops the encoder (Encode returns false).
    using Sink = bool (*)(void* context, const uint8_t* data, std::size_t size);

    // The work memory Encode needs for an image `width` pixels wide (0 when the width is out of range,
    // 1..kMaxWidth): the window, the hash table, two rows, the palette and the output chunk. About 30 KB for the
    // Cardenza's 240 pixels; more makes a larger window (better compression on large images), up to 64 KB.
    inline constexpr int kMaxWidth = 2048;
    inline constexpr int kMaxHeight = 2048;
    inline constexpr std::size_t kChunkBytes = 4096;
    std::size_t WorkBytes(int width);

    struct Result {
        bool ok = false;
        bool indexed = false;     // colour type 3
        int bitDepth = 0;         // 1, 2, 4 or 8
        int colours = 0;          // in the palette (0: more than 256)
        std::size_t bytes = 0;    // the PNG's size
    };

    // `pixels`: `height` rows of `width` RGB565 values (red in bits 15-11, as the processor holds a uint16_t), a
    // row every `stride` values. `work`: WorkBytes(width) bytes or more, any alignment. False (Result::ok) when the
    // sizes are out of range, the work memory is too small, or the sink refused.
    Result Encode(const uint16_t* pixels, int width, int height, int stride, void* work, std::size_t workBytes,
                  Sink sink, void* context);

    // The same from rows the caller makes: `row(rowContext, y)` gives row y (`width` RGB565 values), valid until
    // the next call. Rows are asked for in order, twice over (the colours, then the pixels): the same PNG as
    // Encode of the whole frame, with no frame in memory (the 203 Launcher's way: no PSRAM, and no room for a
    // 65 KB frame beside Wi-Fi).
    using RowSource = const uint16_t* (*)(void* rowContext, int y);
    Result EncodeRows(RowSource row, void* rowContext, int width, int height, void* work, std::size_t workBytes,
                      Sink sink, void* context);

    // RGB565 to 8 bits a channel, as the PNG holds it.
    inline constexpr uint8_t Red8(uint16_t p) { return static_cast<uint8_t>(((p >> 11) << 3) | ((p >> 11) >> 2)); }
    inline constexpr uint8_t Green8(uint16_t p)
    {
        return static_cast<uint8_t>((((p >> 5) & 0x3F) << 2) | (((p >> 5) & 0x3F) >> 4));
    }
    inline constexpr uint8_t Blue8(uint16_t p) { return static_cast<uint8_t>(((p & 0x1F) << 3) | ((p & 0x1F) >> 2)); }

    // The CRC-32 PNG chunks carry (ISO 3309), continued from `crc` (0 to start).
    uint32_t Crc32(uint32_t crc, const uint8_t* data, std::size_t size);

}  // namespace CardenzaHAL::Screenshot::Png
