#pragma once

#include <cstdint>

#include "usb/drive_logic.h"

// The USB drive: the SD card lent to the computer as a removable disk ("Cardenza Storage", SCSI "203 Cardenza SD"),
// for copying files to and from the card without taking it out. Opt-in: Kconfig CONFIG_CARDENZA_HAL_USB_DRIVE
// (CardenzaHAL > USB); without it Share fails with NotBuilt and nothing of it is linked but these calls.
//
// The card has one owner at a time. Share() lends it:
//   1. your listener hears State::Starting, from the drive's task: close the files you have open on the card there
//      (a file still open afterwards fails with EBADF; nothing else of yours is touched);
//   2. the HAL unmounts the file system (Storage::IsMounted() false, Storage::LentToComputer() true, Storage::Init
//      and every file operation under /sdcard fail until the card comes back) and starts the card alone;
//   3. the drive is added to the USB device: the computer finds the device again (its MIDI ports and audio
//      streams start again), with the drive's IN endpoint taken from what gives way (usb/usb.h: first the
//      playback's feedback endpoint, then HID, ...).
// The card comes back (the drive is removed, the computer sees the disk go, the file system mounts the card again
// and your listener hears State::Off, or Failed when it could not mount it) when:
//   - you call Stop();
//   - the computer ejects the disk (Finder's eject, Windows' "Eject", Linux's eject / udisksctl power-off);
//   - the host unconfigures the device (the cable goes, a USB reset that does not configure it again), or stays
//     away (suspended: asleep, or a cable pulled on a board running on battery) for DriveLogic::kHostAwayMs;
//   - no computer touches the drive within DriveLogic::kHostFirstMs of sharing (nothing plugged in).
//
// What the computer writes reaches the card before the computer is told it is written (no cache on the device), so
// the card holds exactly what the computer wrote when the cable goes: what the computer had not written yet (its
// own cache: eject first, as with any USB stick) is lost, and a file it was in the middle of writing is cut. The
// FAT is the computer's to keep consistent; a card unplugged mid-write may need the computer's disk check.
// While the computer reads or writes, the USB task carries each 4 KB block to and from the card (a few
// milliseconds; a card's slow write can take a few hundred): MIDI, HID and the system channel wait meanwhile, USB
// audio does not (it runs from its FIFOs).
namespace CardenzaHAL::USB::Drive {

    using State = DriveLogic::State;
    using Reason = DriveLogic::Reason;

    struct Status {
        State state = State::Off;
        Reason reason = Reason::None;  // why the last share ended, or why it failed
        bool hostSeen = false;         // the computer has sent the drive a command
        uint64_t bytesRead = 0;        // by the computer, this share
        uint64_t bytesWritten = 0;
        uint32_t ioErrors = 0;         // card reads and writes that failed, this share
    };

    // Lends the card to the computer. Waits up to `waitMs` for the drive to be there (State::Shared): true then.
    // With 0 it returns at once (true: asked; the listener tells the rest). False: not built, USB not started, no
    // computer configured the device, no card, or no room for the drive (Status says which).
    bool Share(uint32_t waitMs = 3000);
    // Takes the card back. Waits up to `waitMs` for the file system to have it again (State::Off): true then.
    bool Stop(uint32_t waitMs = 3000);
    Status GetStatus();
    // Told every change of state, from the drive's task (keep it short; close your files on Starting).
    using Listener = void (*)(const Status& status);
    void SetListener(Listener listener);

    // Names for logs and screens.
    inline const char* StateName(State state) { return DriveLogic::StateName(state); }
    inline const char* ReasonName(Reason reason) { return DriveLogic::ReasonName(reason); }

}  // namespace CardenzaHAL::USB::Drive
