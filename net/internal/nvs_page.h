#pragma once

#include <cstddef>
#include <cstdint>

#include "net/internal/crc32.h"

// What one NVS page holds, from its 32-byte header and its 32-byte entry state table, as ESP-IDF 5.3's
// Page::load reads them (components/nvs_flash/src/nvs_page.cpp). Pure (no ESP-IDF): the host tests
// check it against a page made by ESP-IDF's nvs_partition_gen.py.
//
// Net::ReadLauncherSettings opens the NVS partition through a read-only copy of its partition entry. In
// most states that need a repair, the repair's write is refused and the open fails; in two it is not
// reported (a fixed-size item found twice after an interrupted write: the page it is on is then dropped
// in RAM, its items unreadable, and the open still succeeds). Every repair changes the count of written
// entries NVS keeps in RAM, so comparing nvs_get_stats' used_entries with the sum of `written` over the
// pages that hold data finds them all.
namespace CardenzaHAL::Net::NvsPage {

    inline constexpr std::size_t kPageBytes = 4096;
    inline constexpr std::size_t kHeaderBytes = 32;      // state, sequence number, version, reserved, CRC
    inline constexpr std::size_t kEntryTableOffset = 32;
    inline constexpr std::size_t kEntryTableBytes = 32;  // 2 bits per entry
    inline constexpr std::size_t kEntryCount = 126;

    inline constexpr uint32_t kStateUninitialized = 0xFFFFFFFFu;
    inline constexpr uint32_t kStateActive = 0xFFFFFFFEu;
    inline constexpr uint32_t kStateFull = 0xFFFFFFFCu;
    inline constexpr uint32_t kStateFreeing = 0xFFFFFFF8u;
    inline constexpr uint8_t kOldestVersion = 0xFE;  // NVS_VERSION: lower is a newer format

    enum class Kind : uint8_t {
        Empty,     // erased (NVS checks the whole page; a page that is not all 0xFF counts as corrupt)
        Data,      // active or full: its items are read
        Freeing,   // a page move was interrupted: opening the partition would finish it (writes)
        Corrupt,   // bad header CRC or an unknown state: NVS ignores it
        Newer,     // written by a newer NVS format: opening fails (ESP_ERR_NVS_NEW_VERSION_FOUND)
    };

    struct Summary {
        Kind kind = Kind::Empty;
        uint32_t written = 0;  // entries marked written in its table (Data pages only)
    };

    inline uint32_t ReadLe32(const uint8_t* bytes)
    {
        return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
               static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
    }

    // `header` is the page's first 32 bytes, `table` the 32 after them.
    inline Summary Summarize(const uint8_t header[kHeaderBytes], const uint8_t table[kEntryTableBytes])
    {
        Summary summary;
        const uint32_t state = ReadLe32(header);
        if (state == kStateUninitialized) return summary;
        // The CRC covers the sequence number, the version and the reserved bytes (bytes 4..27).
        const uint32_t crc = Internal::Crc32Reflected(0, header + 4, 24) ^ 0xFFFFFFFFu;
        if (crc != ReadLe32(header + 28))
        {
            summary.kind = Kind::Corrupt;
            return summary;
        }
        if (header[8] < kOldestVersion)
        {
            summary.kind = Kind::Newer;
            return summary;
        }
        if (state == kStateFreeing)
        {
            summary.kind = Kind::Freeing;
            return summary;
        }
        if (state != kStateActive && state != kStateFull)
        {
            summary.kind = Kind::Corrupt;
            return summary;
        }
        summary.kind = Kind::Data;
        // Entry i is bits 2 (i % 16) of little-endian word i / 16: 0b11 empty, 0b10 written, 0b00 erased.
        for (std::size_t i = 0; i < kEntryCount; ++i)
        {
            const uint32_t word = ReadLe32(table + 4 * (i / 16));
            if (((word >> (2 * (i % 16))) & 0x3u) == 0x2u) ++summary.written;
        }
        return summary;
    }

}  // namespace CardenzaHAL::Net::NvsPage
