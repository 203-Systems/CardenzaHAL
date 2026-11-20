#pragma once

#include <cstdint>

namespace CardenzaHAL::Storage {

    // Where Init mounts the card's FAT file system.
    inline constexpr char kMountPoint[] = "/sdcard";

    bool Init();
    bool IsMounted();
    // Call after filesystem I/O fails, with the file closed. A healthy card
    // stays mounted (e.g. disk full); a lost card is retired on the next Init.
    bool CheckAfterIoError();
    uint64_t GetCapacity();
}
