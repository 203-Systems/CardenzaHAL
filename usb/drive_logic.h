#pragma once

#include <cstdint>

// The USB drive's decisions (usb/drive.h): when the SD card goes to the computer and when it comes back to the
// firmware. Pure (no ESP-IDF, no TinyUSB): usb/drive.cpp's worker carries out the actions, tests/drive_test.cpp
// walks every transition.
//
// The card has one owner at a time. Lending: the firmware's listener hears Starting (it closes its files), the
// file system lets go, the HAL starts the card alone, then the drive is added to the USB device (the computer finds
// the device again). Giving back: the drive reports no medium, the drive is removed (the computer sees the disk
// go), the card is let go, the file system mounts it again.
namespace CardenzaHAL::USB::DriveLogic {

    enum class State : uint8_t {
        Off,        // the firmware has the card
        Starting,   // lending it
        Shared,     // the computer has it
        Returning,  // giving it back
        Failed,     // a lend did not happen, or the card did not come back (reason)
    };

    // Why the last share ended, or why it failed.
    enum class Reason : uint8_t {
        None,
        Stopped,     // the firmware's Stop
        Ejected,     // the computer ejected the disk
        Unplugged,   // the cable went (the host unconfigured the device)
        HostAway,    // the host stayed away (suspended, asleep or unplugged) for kHostAwayMs
        NoHost,      // no computer has configured the device, or none used the drive within kHostFirstMs
        NotBuilt,    // the firmware is built without the drive (CONFIG_CARDENZA_HAL_USB_DRIVE)
        NotStarted,  // USB is not started
        NoRoom,      // the drive could not be added to the USB device (no endpoint left to take)
        NoCard,      // no card answered
        CardError,   // the file system did not let the card go, or could not mount it again
    };

    // A computer that stays away this long (suspended: asleep, or a cable pulled on a board on battery, which the
    // ESP32-S3 sees as a suspended bus; or unconfigured) gives the card back. Linux may autosuspend an idle device
    // after 2 s when its power control is "auto": longer than that.
    inline constexpr uint32_t kHostAwayMs = 10000;
    // A drive no computer touches this long after sharing (nothing plugged in) gives the card back.
    inline constexpr uint32_t kHostFirstMs = 15000;

    enum class Action : uint8_t {
        None,
        Lend,     // tell the listener, unmount the file system, start the card alone
        Present,  // add the drive to the USB device
        Return,   // remove the drive, let the card go, mount the file system
    };

    class Machine {
      public:
        State GetState() const { return state_; }
        Reason GetReason() const { return reason_; }
        bool HostSeen() const { return hostSeen_; }
        // The card is the computer's to read and write (the MSC callbacks answer with it).
        bool MediumReady() const { return state_ == State::Shared && !ejected_; }
        // The firmware is not to touch the card (the file system is let go).
        bool CardLent() const { return state_ == State::Starting || state_ == State::Shared || state_ == State::Returning; }

        // The firmware asks to share. `built`: the firmware has the drive; `started`: USB runs; `host`: a computer
        // has configured the device (else nobody would take the disk: the card stays the firmware's).
        Action Share(bool built, bool started, bool host)
        {
            if (state_ == State::Starting || state_ == State::Shared) return Action::None;  // already
            if (state_ == State::Returning)
            {
                shareAfterReturn_ = true;  // once the card is back
                return Action::None;
            }
            if (!built) return Fail(Reason::NotBuilt);
            if (!started) return Fail(Reason::NotStarted);
            if (!host) return Fail(Reason::NoHost);
            state_ = State::Starting;
            reason_ = Reason::None;
            hostSeen_ = false;
            ejected_ = false;
            awayMs_ = 0;
            sinceSharedMs_ = 0;
            stopAfterStart_ = false;
            shareAfterReturn_ = false;
            return Action::Lend;
        }

        // The firmware asks for the card back.
        Action Stop()
        {
            shareAfterReturn_ = false;
            if (state_ == State::Starting)
            {
                stopAfterStart_ = true;
                return Action::None;
            }
            if (state_ == State::Shared) return Return(Reason::Stopped);
            return Action::None;
        }

        // The worker lent the card: `ok`, else why not (NoCard, CardError).
        Action Lent(bool ok, Reason why = Reason::NoCard)
        {
            if (state_ != State::Starting) return Action::None;
            if (!ok)
            {
                // Nothing to remove from USB: the file system takes the card back.
                state_ = State::Returning;
                reason_ = why;
                failAfterReturn_ = true;
                return Action::Return;
            }
            return Action::Present;
        }

        // The worker added the drive to the USB device, or could not.
        Action Presented(bool ok)
        {
            if (state_ != State::Starting) return Action::None;
            if (!ok)
            {
                state_ = State::Returning;
                reason_ = Reason::NoRoom;
                failAfterReturn_ = true;
                return Action::Return;
            }
            state_ = State::Shared;
            if (stopAfterStart_)
            {
                stopAfterStart_ = false;
                return Return(Reason::Stopped);
            }
            return Action::None;
        }

        // The computer sent the drive a command (it has the disk).
        void HostCommand()
        {
            if (state_ == State::Shared) hostSeen_ = true;
        }

        // The computer ejected the disk (START STOP UNIT with LoEj).
        Action HostEject()
        {
            if (state_ != State::Shared) return Action::None;
            ejected_ = true;
            return Return(Reason::Ejected);
        }

        // The host unconfigured the device or the cable went (tud_umount_cb). The device's own re-enumeration while
        // the drive is added comes before the computer has used it: not counted.
        Action HostGone()
        {
            if (state_ != State::Shared || !hostSeen_) return Action::None;
            return Return(Reason::Unplugged);
        }

        // Every `elapsedMs` while shared: is the host there (configured and not suspended)?
        Action Tick(uint32_t elapsedMs, bool hostThere)
        {
            if (state_ != State::Shared) return Action::None;
            sinceSharedMs_ += elapsedMs;
            awayMs_ = hostThere ? 0 : awayMs_ + elapsedMs;
            if (hostSeen_ && awayMs_ >= kHostAwayMs) return Return(Reason::HostAway);
            if (!hostSeen_ && sinceSharedMs_ >= kHostFirstMs) return Return(Reason::NoHost);
            return Action::None;
        }

        // The worker gave the card back: the file system mounted it (`mounted`), or not.
        Action Returned(bool mounted)
        {
            if (state_ != State::Returning) return Action::None;
            if (failAfterReturn_)
            {
                failAfterReturn_ = false;
                state_ = State::Failed;  // reason kept: why the lend failed
            }
            else if (!mounted && reason_ != Reason::NoCard)
            {
                // The share ended, but the file system could not mount the card (taken out, or broken).
                state_ = State::Failed;
                reason_ = Reason::CardError;
            }
            else
                state_ = State::Off;
            ejected_ = false;
            hostSeen_ = false;
            if (shareAfterReturn_)
            {
                shareAfterReturn_ = false;
                return Share(true, true, true);  // the worker checks USB again before it lends
            }
            return Action::None;
        }

      private:
        Action Fail(Reason why)
        {
            state_ = State::Failed;
            reason_ = why;
            return Action::None;
        }
        Action Return(Reason why)
        {
            state_ = State::Returning;
            reason_ = why;
            return Action::Return;
        }

        State state_ = State::Off;
        Reason reason_ = Reason::None;
        bool hostSeen_ = false;
        bool ejected_ = false;
        bool stopAfterStart_ = false;
        bool shareAfterReturn_ = false;
        bool failAfterReturn_ = false;
        uint32_t awayMs_ = 0;
        uint32_t sinceSharedMs_ = 0;
    };

    // Names for logs and screens.
    inline const char* StateName(State state)
    {
        switch (state)
        {
            case State::Off: return "off";
            case State::Starting: return "starting";
            case State::Shared: return "shared";
            case State::Returning: return "returning";
            case State::Failed: return "failed";
        }
        return "?";
    }

    inline const char* ReasonName(Reason reason)
    {
        switch (reason)
        {
            case Reason::None: return "none";
            case Reason::Stopped: return "stopped";
            case Reason::Ejected: return "ejected";
            case Reason::Unplugged: return "unplugged";
            case Reason::HostAway: return "host away";
            case Reason::NoHost: return "no host";
            case Reason::NotBuilt: return "not built";
            case Reason::NotStarted: return "USB not started";
            case Reason::NoRoom: return "no room on USB";
            case Reason::NoCard: return "no card";
            case Reason::CardError: return "card error";
        }
        return "?";
    }

    // SCSI READ(10) / WRITE(10) within the card: `count` sectors from `lba`, a whole number of 512-byte sectors.
    inline bool BlockRangeOk(uint32_t lba, uint32_t bytes, uint32_t offset, uint32_t sectors, uint32_t sectorSize = 512)
    {
        if (offset != 0 || bytes == 0 || bytes % sectorSize != 0) return false;
        const uint64_t count = bytes / sectorSize;
        return static_cast<uint64_t>(lba) + count <= sectors;
    }

}  // namespace CardenzaHAL::USB::DriveLogic
