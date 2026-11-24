#pragma once

// The SD card lent to a computer as a USB drive (usb/drive.h): the file system lets it go first and does not mount
// it again until it comes back. Internal to the HAL (storage/storage.cpp; usb/drive.cpp drives the card itself
// while it is lent).
namespace CardenzaHAL::Storage::detail {

    // Unmounts the FAT file system (files the firmware left open fail from now on) and marks the card lent: Init
    // does not mount it, IsMounted is false, LentToComputer true. False when the file system did not let go (the
    // card is then not lent).
    bool Lend();
    // The card is back: no longer lent, and mounted again (Init's result).
    bool GiveBack();

}  // namespace CardenzaHAL::Storage::detail
