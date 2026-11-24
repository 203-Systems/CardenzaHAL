// The screenshot's PNG encoder (screenshot/png.h). Pure: no allocation, and no ESP-IDF but the CRC-32: the chip's ROM
// has it (esp_rom_crc32_le, the same function as the table below: 1 KB less in a full partition such as the 203
// Launcher's), the host tests build the table (tests/screenshot_png_check.py checks every chunk's CRC with zlib).
#include "screenshot/png.h"

#include <algorithm>
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_rom_crc.h>
#endif

namespace CardenzaHAL::Screenshot::Png {

namespace {

constexpr int kPaletteSlots = 512;  // open addressing for up to 257 colours
constexpr int kHashBits = 11;       // 2048 heads
constexpr int kHashSize = 1 << kHashBits;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr std::size_t kMinRing = 4096;
constexpr std::size_t kMaxRing = 32768;

constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

// deflate's length codes 257..285 and distance codes 0..29 (RFC 1951 3.2.5).
constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

#ifndef ESP_PLATFORM
struct CrcTable {
    uint32_t value[256];
};

constexpr CrcTable MakeCrcTable()
{
    CrcTable table{};
    for (uint32_t n = 0; n < 256; ++n)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table.value[n] = c;
    }
    return table;
}

constexpr CrcTable kCrc = MakeCrcTable();
#endif

std::size_t RowBytesRgb(int width)
{
    return 1 + static_cast<std::size_t>(width) * 3;
}

std::size_t FixedBytes(int width)
{
    return kPaletteSlots * sizeof(uint32_t) + 2 * static_cast<std::size_t>(width) * 3 + kChunkBytes +
           kHashSize * sizeof(uint32_t);
}

// The smallest ring that holds a row and a match ahead of the window, and the window beside them.
std::size_t MinRing(int width)
{
    std::size_t ring = kMinRing;
    while (ring < 2 * (RowBytesRgb(width) + kMaxMatch)) ring <<= 1;
    return ring;
}

uint8_t* Align4(uint8_t* p)
{
    return reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + 3) & ~uintptr_t{3});
}

void Put32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

int Paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

class Encoder {
  public:
    Encoder(const uint16_t* pixels, int width, int height, int stride, Sink sink, void* context)
        : pixels_(pixels), width_(width), height_(height), stride_(stride), sink_(sink), context_(context)
    {
    }

    // Rows from the caller instead of `pixels` (EncodeRows).
    void Rows(RowSource row, void* rowContext)
    {
        rowSource_ = row;
        rowContext_ = rowContext;
    }

    bool Carve(void* work, std::size_t bytes)
    {
        uint8_t* p = Align4(static_cast<uint8_t*>(work));
        const std::size_t lost = static_cast<std::size_t>(p - static_cast<uint8_t*>(work));
        if (bytes < lost + FixedBytes(width_)) return false;
        const std::size_t left = bytes - lost - FixedBytes(width_);
        palette_ = reinterpret_cast<uint32_t*>(p);
        p += kPaletteSlots * sizeof(uint32_t);
        heads_ = reinterpret_cast<uint32_t*>(p);
        p += kHashSize * sizeof(uint32_t);
        prev_ = p;
        p += static_cast<std::size_t>(width_) * 3;
        cur_ = p;
        p += static_cast<std::size_t>(width_) * 3;
        chunk_ = p;
        p += kChunkBytes;
        std::size_t ring = kMaxRing * 2;
        while (ring > left) ring >>= 1;
        if (ring < MinRing(width_)) return false;
        ring_ = p;
        ringMask_ = static_cast<uint32_t>(ring - 1);
        return true;
    }

    Result Run()
    {
        Result result;
        CountColours();
        rowBytes_ = indexed_ ? 1 + (static_cast<std::size_t>(width_) * bitDepth_ + 7) / 8 : RowBytesRgb(width_);
        total_ = static_cast<uint32_t>(rowBytes_ * height_);
        bpp_ = indexed_ ? 1 : 3;
        const std::size_t ring = std::size_t{ringMask_} + 1;
        maxDistance_ = static_cast<uint32_t>(std::min<std::size_t>(32768, ring - rowBytes_ - kMaxMatch - 1));
        std::memset(heads_, 0, kHashSize * sizeof(uint32_t));
        std::memset(prev_, 0, static_cast<std::size_t>(width_) * 3);

        if (!Header()) return result;
        // zlib: deflate with a 32 KB window, no dictionary (0x78 0x01: the check bits make it a multiple of 31).
        PutByte(0x78);
        PutByte(0x01);
        // One deflate block for the whole stream: BFINAL 1, BTYPE 01 (fixed Huffman codes).
        PutBits(1, 1);
        PutBits(1, 2);
        Compress();
        PutCode(256);
        if (bitCount_ > 0) PutBits(0, 8 - bitCount_);
        const uint32_t adler = (adlerB_ << 16) | adlerA_;
        PutByte(static_cast<uint8_t>(adler >> 24));
        PutByte(static_cast<uint8_t>(adler >> 16));
        PutByte(static_cast<uint8_t>(adler >> 8));
        PutByte(static_cast<uint8_t>(adler));
        if (!FlushChunk()) return result;
        if (!Chunk("IEND", nullptr, 0)) return result;
        if (failed_) return result;
        result.ok = true;
        result.indexed = indexed_;
        result.bitDepth = bitDepth_;
        result.colours = indexed_ ? colours_ : 0;
        result.bytes = written_;
        return result;
    }

  private:
    // ---- Output -----------------------------------------------------------------------------------------

    bool Send(const uint8_t* data, std::size_t size)
    {
        if (failed_) return false;
        if (size == 0) return true;
        if (!sink_(context_, data, size))
        {
            failed_ = true;
            return false;
        }
        written_ += size;
        return true;
    }

    // A whole chunk: its length, type, data and CRC.
    bool Chunk(const char* type, const uint8_t* data, std::size_t size)
    {
        uint8_t head[8];
        Put32(head, static_cast<uint32_t>(size));
        std::memcpy(head + 4, type, 4);
        uint32_t crc = Crc32(0, head + 4, 4);
        crc = Crc32(crc, data, size);
        uint8_t tail[4];
        Put32(tail, crc);
        return Send(head, 8) && Send(data, size) && Send(tail, 4);
    }

    bool Header()
    {
        if (!Send(kSignature, sizeof(kSignature))) return false;
        uint8_t ihdr[13];
        Put32(ihdr, static_cast<uint32_t>(width_));
        Put32(ihdr + 4, static_cast<uint32_t>(height_));
        ihdr[8] = static_cast<uint8_t>(bitDepth_);
        ihdr[9] = indexed_ ? 3 : 2;
        ihdr[10] = 0;  // deflate
        ihdr[11] = 0;  // adaptive filtering
        ihdr[12] = 0;  // no interlace
        if (!Chunk("IHDR", ihdr, sizeof(ihdr))) return false;
        if (!indexed_) return true;
        // The palette, in the chunk buffer before any IDAT uses it.
        for (int slot = 0; slot < kPaletteSlots; ++slot)
        {
            const uint32_t entry = palette_[slot];
            if (!(entry & 0x80000000u)) continue;
            const int index = static_cast<int>((entry >> 16) & 0xFF);
            const uint16_t colour = static_cast<uint16_t>(entry);
            chunk_[index * 3] = Red8(colour);
            chunk_[index * 3 + 1] = Green8(colour);
            chunk_[index * 3 + 2] = Blue8(colour);
        }
        return Chunk("PLTE", chunk_, static_cast<std::size_t>(colours_) * 3);
    }

    bool FlushChunk()
    {
        if (chunkUsed_ == 0) return !failed_;
        const bool ok = Chunk("IDAT", chunk_, chunkUsed_);
        chunkUsed_ = 0;
        return ok;
    }

    void PutByte(uint8_t byte)
    {
        chunk_[chunkUsed_++] = byte;
        if (chunkUsed_ == kChunkBytes) (void)FlushChunk();
    }

    void PutBits(uint32_t value, int count)
    {
        bits_ |= value << bitCount_;
        bitCount_ += count;
        while (bitCount_ >= 8)
        {
            PutByte(static_cast<uint8_t>(bits_));
            bits_ >>= 8;
            bitCount_ -= 8;
        }
    }

    // A Huffman code goes out from its most significant bit (RFC 1951 3.1.1).
    void PutReversed(uint32_t code, int length)
    {
        uint32_t reversed = 0;
        for (int i = 0; i < length; ++i) reversed |= ((code >> i) & 1u) << (length - 1 - i);
        PutBits(reversed, length);
    }

    // A literal/length symbol in the fixed code (RFC 1951 3.2.6).
    void PutCode(int symbol)
    {
        if (symbol < 144) PutReversed(0x30 + symbol, 8);
        else if (symbol < 256) PutReversed(0x190 + symbol - 144, 9);
        else if (symbol < 280) PutReversed(symbol - 256, 7);
        else PutReversed(0xC0 + symbol - 280, 8);
    }

    void PutMatch(int length, uint32_t distance)
    {
        int code = 28;
        while (kLengthBase[code] > length) --code;
        PutCode(257 + code);
        if (kLengthExtra[code]) PutBits(static_cast<uint32_t>(length - kLengthBase[code]), kLengthExtra[code]);
        int dcode = 29;
        while (kDistanceBase[dcode] > distance) --dcode;
        PutReversed(static_cast<uint32_t>(dcode), 5);
        if (kDistanceExtra[dcode]) PutBits(distance - kDistanceBase[dcode], kDistanceExtra[dcode]);
    }

    // ---- Colours ----------------------------------------------------------------------------------------

    // Finds `colour` in the palette, adding it while there is room. -1: a 257th colour.
    int PaletteIndex(uint16_t colour, bool add)
    {
        uint32_t slot = (colour * 0x9E37u) >> 7 & (kPaletteSlots - 1);
        while (true)
        {
            const uint32_t entry = palette_[slot];
            if (!(entry & 0x80000000u))
            {
                if (!add || colours_ >= 256) return -1;
                palette_[slot] = 0x80000000u | (static_cast<uint32_t>(colours_) << 16) | colour;
                return colours_++;
            }
            if (static_cast<uint16_t>(entry) == colour) return static_cast<int>((entry >> 16) & 0xFF);
            slot = (slot + 1) & (kPaletteSlots - 1);
        }
    }

    void CountColours()
    {
        std::memset(palette_, 0, kPaletteSlots * sizeof(uint32_t));
        colours_ = 0;
        indexed_ = true;
        for (int y = 0; y < height_ && indexed_; ++y)
        {
            const uint16_t* row = Row(y);
            uint16_t last = 0;
            bool haveLast = false;
            for (int x = 0; x < width_; ++x)
            {
                if (haveLast && row[x] == last) continue;
                last = row[x];
                haveLast = true;
                if (PaletteIndex(last, true) < 0)
                {
                    indexed_ = false;
                    break;
                }
            }
        }
        if (!indexed_) bitDepth_ = 8;
        else bitDepth_ = colours_ <= 2 ? 1 : colours_ <= 4 ? 2 : colours_ <= 16 ? 4 : 8;
    }

    // ---- Rows ---------------------------------------------------------------------------------------------

    const uint16_t* Row(int y) const
    {
        return rowSource_ != nullptr ? rowSource_(rowContext_, y) : pixels_ + static_cast<std::size_t>(y) * stride_;
    }

    void Append(uint8_t byte)
    {
        ring_[produced_ & ringMask_] = byte;
        ++produced_;
        adlerA_ += byte;
        adlerB_ += adlerA_;
        if (++adlerRun_ == 5552)
        {
            adlerA_ %= 65521;
            adlerB_ %= 65521;
            adlerRun_ = 0;
        }
    }

    void FinishAdler()
    {
        adlerA_ %= 65521;
        adlerB_ %= 65521;
        adlerRun_ = 0;
    }

    // Row `row_` filtered, into the ring.
    void ProduceRow()
    {
        const uint16_t* source = Row(row_);
        if (indexed_)
        {
            Append(0);  // filter None (PNG's advice for indexed images): deflate finds the rows above itself
            uint32_t acc = 0;
            int filled = 0;
            for (int x = 0; x < width_; ++x)
            {
                acc = (acc << bitDepth_) | static_cast<uint32_t>(PaletteIndex(source[x], false));
                filled += bitDepth_;
                if (filled == 8)
                {
                    Append(static_cast<uint8_t>(acc));
                    acc = 0;
                    filled = 0;
                }
            }
            if (filled > 0) Append(static_cast<uint8_t>(acc << (8 - filled)));
        }
        else
        {
            const int n = width_ * 3;
            for (int x = 0; x < width_; ++x)
            {
                cur_[x * 3] = Red8(source[x]);
                cur_[x * 3 + 1] = Green8(source[x]);
                cur_[x * 3 + 2] = Blue8(source[x]);
            }
            // The filter with the least sum of absolute differences (PNG's usual heuristic).
            uint32_t sums[5] = {};
            for (int i = 0; i < n; ++i)
            {
                const int x = cur_[i], a = i >= 3 ? cur_[i - 3] : 0, b = prev_[i], c = i >= 3 ? prev_[i - 3] : 0;
                const uint8_t v[5] = {static_cast<uint8_t>(x), static_cast<uint8_t>(x - a), static_cast<uint8_t>(x - b),
                                      static_cast<uint8_t>(x - ((a + b) >> 1)), static_cast<uint8_t>(x - Paeth(a, b, c))};
                for (int f = 0; f < 5; ++f) sums[f] += v[f] < 128 ? v[f] : 256 - v[f];
            }
            int best = 0;
            for (int f = 1; f < 5; ++f)
                if (sums[f] < sums[best]) best = f;
            Append(static_cast<uint8_t>(best));
            for (int i = 0; i < n; ++i)
            {
                const int x = cur_[i], a = i >= 3 ? cur_[i - 3] : 0, b = prev_[i], c = i >= 3 ? prev_[i - 3] : 0;
                int predicted = 0;
                switch (best)
                {
                    case 1: predicted = a; break;
                    case 2: predicted = b; break;
                    case 3: predicted = (a + b) >> 1; break;
                    case 4: predicted = Paeth(a, b, c); break;
                    default: break;
                }
                Append(static_cast<uint8_t>(x - predicted));
            }
            std::memcpy(prev_, cur_, static_cast<std::size_t>(n));
        }
        ++row_;
    }

    // ---- LZ77 ---------------------------------------------------------------------------------------------

    uint8_t At(uint32_t position) const { return ring_[position & ringMask_]; }

    uint32_t Hash(uint32_t position) const
    {
        const uint32_t key = (uint32_t{At(position)} << 16) | (uint32_t{At(position + 1)} << 8) | At(position + 2);
        return (key * 0x9E3779B1u) >> (32 - kHashBits);
    }

    int MatchLength(uint32_t candidate, uint32_t position, int limit) const
    {
        int length = 0;
        while (length < limit && At(candidate + length) == At(position + length)) ++length;
        return length;
    }

    void Insert(uint32_t position)
    {
        if (position + kMinMatch <= produced_) heads_[Hash(position)] = position + 1;
    }

    void Compress()
    {
        uint32_t position = 0;
        while (position < total_)
        {
            // What is ahead: a row more until a whole match fits (or the image is done).
            while (row_ < height_ && produced_ - position < kMaxMatch) ProduceRow();
            const int ahead = static_cast<int>(std::min<uint32_t>(produced_ - position, kMaxMatch));
            int bestLength = 0;
            uint32_t bestDistance = 0;
            if (ahead >= kMinMatch)
            {
                const uint32_t hash = Hash(position);
                const uint32_t candidates[4] = {heads_[hash] != 0 ? position - (heads_[hash] - 1) : 0, bpp_,
                                                static_cast<uint32_t>(rowBytes_), static_cast<uint32_t>(rowBytes_ * 2)};
                for (const uint32_t distance : candidates)
                {
                    if (distance == 0 || distance > maxDistance_ || distance > position) continue;
                    const int length = MatchLength(position - distance, position, ahead);
                    if (length > bestLength || (length == bestLength && distance < bestDistance))
                    {
                        bestLength = length;
                        bestDistance = distance;
                    }
                }
                heads_[hash] = position + 1;
            }
            if (bestLength >= kMinMatch)
            {
                PutMatch(bestLength, bestDistance);
                for (int i = 1; i < bestLength; ++i) Insert(position + i);
                position += static_cast<uint32_t>(bestLength);
            }
            else
            {
                PutCode(At(position));
                ++position;
            }
        }
        FinishAdler();
    }

    const uint16_t* pixels_;
    int width_, height_, stride_;
    RowSource rowSource_ = nullptr;
    void* rowContext_ = nullptr;
    Sink sink_;
    void* context_;

    uint32_t* palette_ = nullptr;
    uint32_t* heads_ = nullptr;
    uint8_t* prev_ = nullptr;
    uint8_t* cur_ = nullptr;
    uint8_t* chunk_ = nullptr;
    uint8_t* ring_ = nullptr;
    uint32_t ringMask_ = 0;

    bool indexed_ = false;
    int colours_ = 0;
    int bitDepth_ = 8;
    std::size_t rowBytes_ = 0;
    uint32_t total_ = 0;
    uint32_t bpp_ = 3;
    uint32_t maxDistance_ = 0;

    int row_ = 0;
    uint32_t produced_ = 0;
    uint32_t adlerA_ = 1, adlerB_ = 0, adlerRun_ = 0;

    std::size_t chunkUsed_ = 0;
    uint32_t bits_ = 0;
    int bitCount_ = 0;
    std::size_t written_ = 0;
    bool failed_ = false;
};

}  // namespace

uint32_t Crc32(uint32_t crc, const uint8_t* data, std::size_t size)
{
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(crc, data, static_cast<uint32_t>(size));
#else
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = kCrc.value[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
#endif
}

std::size_t WorkBytes(int width)
{
    if (width < 1 || width > kMaxWidth) return 0;
    std::size_t ring = MinRing(width);
    // A larger window where it costs little: 16 KB for the Cardenza's 240 pixels.
    if (ring < 16384) ring = 16384;
    return FixedBytes(width) + ring + 4;  // + 4: the work memory's alignment
}

Result Encode(const uint16_t* pixels, int width, int height, int stride, void* work, std::size_t workBytes, Sink sink,
              void* context)
{
    if (pixels == nullptr || work == nullptr || sink == nullptr || width < 1 || width > kMaxWidth || height < 1 ||
        height > kMaxHeight || stride < width)
        return Result{};
    Encoder encoder(pixels, width, height, stride, sink, context);
    if (!encoder.Carve(work, workBytes)) return Result{};
    return encoder.Run();
}

Result EncodeRows(RowSource row, void* rowContext, int width, int height, void* work, std::size_t workBytes, Sink sink,
                  void* context)
{
    if (row == nullptr || work == nullptr || sink == nullptr || width < 1 || width > kMaxWidth || height < 1 ||
        height > kMaxHeight)
        return Result{};
    Encoder encoder(nullptr, width, height, width, sink, context);
    encoder.Rows(row, rowContext);
    if (!encoder.Carve(work, workBytes)) return Result{};
    return encoder.Run();
}

}  // namespace CardenzaHAL::Screenshot::Png
