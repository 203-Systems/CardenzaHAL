// Crc32 (screenshot/png.h) for host builds (the host tests, a simulator): the CRC-32 by a table, the same function
// as the chip ROM's in png_crc_esp.cpp (tests/screenshot_png_check.py checks every chunk's CRC with Python's zlib).
// Never in the device build: the component's CMakeLists.txt takes png_crc_esp.cpp.
#include "screenshot/png.h"

namespace CardenzaHAL::Screenshot::Png {

namespace {

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

}  // namespace

uint32_t Crc32(uint32_t crc, const uint8_t* data, std::size_t size)
{
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = kCrc.value[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace CardenzaHAL::Screenshot::Png
