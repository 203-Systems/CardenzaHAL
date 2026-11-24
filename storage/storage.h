#pragma once

#include <cstdint>

namespace CardenzaHAL::Storage {

    // Where Init mounts the card's FAT file system.
    inline constexpr char kMountPoint[] = "/sdcard";

    // Mounts the card at kMountPoint. False without a card, and while it is lent to a computer (below).
    bool Init();
    bool IsMounted();
    // The card is lent to a computer as a USB drive (usb/drive.h, USB::Drive::Share): the file system is
    // unmounted until the computer gives it back (Init and every file operation fail meanwhile).
    bool LentToComputer();
    // Call after filesystem I/O fails, with the file closed. A healthy card
    // stays mounted (e.g. disk full); a lost card is retired on the next Init.
    bool CheckAfterIoError();
    uint64_t GetCapacity();
}
