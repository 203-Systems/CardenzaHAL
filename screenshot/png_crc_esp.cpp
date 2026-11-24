// Crc32 (screenshot/png.h) on the device: the chip's ROM has the CRC-32 PNG chunks carry (esp_rom_crc32_le
// inverts before and after, as png_crc_host.cpp's table does), so a firmware or the 203 Launcher carries no 1 KB
// table. The component's CMakeLists.txt builds this file; host builds take png_crc_host.cpp instead.
#include "screenshot/png.h"

#include <esp_rom_crc.h>

namespace CardenzaHAL::Screenshot::Png {

uint32_t Crc32(uint32_t crc, const uint8_t* data, std::size_t size)
{
    return esp_rom_crc32_le(crc, data, static_cast<uint32_t>(size));
}

}  // namespace CardenzaHAL::Screenshot::Png
