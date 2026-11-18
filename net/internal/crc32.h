#pragma once

#include <cstddef>
#include <cstdint>

// CRC-32 with the reflected IEEE polynomial, bit by bit. Pure (no ESP-IDF). The two users start it
// differently: the 203 Launcher's crc32() (zlib's CRC-32) from 0xFFFFFFFF, an NVS page header
// (esp_rom_crc32_le(0xFFFFFFFF, ...)) from 0; both invert the result.
namespace CardenzaHAL::Net::Internal {

    inline uint32_t Crc32Reflected(uint32_t crc, const void* data, std::size_t length)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        for (std::size_t i = 0; i < length; ++i)
        {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) != 0 ? 0xEDB88320u : 0u);
        }
        return crc;
    }

}  // namespace CardenzaHAL::Net::Internal
