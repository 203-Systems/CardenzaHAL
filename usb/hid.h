#pragma once

#include <cstdint>

// HID inputs for a firmware: the Cardenza as a computer's keyboard, media keys and gamepad. They are report IDs
// of the USB device's one HID interface (usb/usb.h: Config::hid with Config::keyboard, consumer, gamepad), next
// to the system channel (report 0xCB) and the firmware's raw reports (0xFF): no endpoint of their own.
//
// The HAL keeps what is held (keys, the media key, the gamepad's state) and the computer always ends up seeing
// that: a report that finds no room, or no computer (unplugged, asleep), goes as soon as the computer can take
// it, and a computer that comes back (resumes, is plugged in again) first gets the state as it is now. When the
// computer goes away, or the device's classes change (Reconfigure, the drive, the camera), everything held is
// let go: nothing stays pressed. Call these from the firmware's tasks, never from a USB callback.
//
// Each call returns true when its report is on its way to the computer now, false when it is not (no computer,
// no HID, that input not in the device's Config, or no room within `waitMs`); the state is kept either way and
// goes later. On macOS a HID keyboard makes the computer ask the person to identify it once (Keyboard Setup
// Assistant), and host tools may then open the HID interface only with the Input Monitoring permission (SysEx,
// WebUSB and the serial port need none): leave Config::keyboard off when the firmware types nothing.
namespace CardenzaHAL::USB::Hid {

    // ---- Keyboard (report ID 1, Config::keyboard): HID usage page 0x07 --------------------------------------
    inline constexpr uint8_t kKeyA = 0x04;  // ... kKeyA + 25 is Z
    inline constexpr uint8_t kKey1 = 0x1E;  // ... kKey1 + 8 is 9
    inline constexpr uint8_t kKey0 = 0x27;
    inline constexpr uint8_t kKeyEnter = 0x28;
    inline constexpr uint8_t kKeyEscape = 0x29;
    inline constexpr uint8_t kKeyBackspace = 0x2A;
    inline constexpr uint8_t kKeyTab = 0x2B;
    inline constexpr uint8_t kKeySpace = 0x2C;
    inline constexpr uint8_t kKeyCapsLock = 0x39;
    inline constexpr uint8_t kKeyF1 = 0x3A;  // ... kKeyF1 + 11 is F12
    inline constexpr uint8_t kKeyDelete = 0x4C;
    inline constexpr uint8_t kKeyRight = 0x4F;
    inline constexpr uint8_t kKeyLeft = 0x50;
    inline constexpr uint8_t kKeyDown = 0x51;
    inline constexpr uint8_t kKeyUp = 0x52;
    // Modifiers: pressed and released like keys.
    inline constexpr uint8_t kKeyLeftCtrl = 0xE0;
    inline constexpr uint8_t kKeyLeftShift = 0xE1;
    inline constexpr uint8_t kKeyLeftAlt = 0xE2;
    inline constexpr uint8_t kKeyLeftGui = 0xE3;  // Command on macOS, Windows key
    inline constexpr uint8_t kKeyRightCtrl = 0xE4;
    inline constexpr uint8_t kKeyRightShift = 0xE5;
    inline constexpr uint8_t kKeyRightAlt = 0xE6;
    inline constexpr uint8_t kKeyRightGui = 0xE7;

    // A computer has the device and it presents HID (Config::hid). Each input also needs its Config flag.
    bool Ready();

    // Holds a key down (a usage above, or any from page 0x07; modifiers 0xE0-0xE7) until Release. Six keys and the
    // modifiers at a time: a seventh is not taken (false, the six stay). Pressing a held key changes nothing.
    bool Press(uint8_t usage, uint32_t waitMs = 20);
    // Lets it go; a key not held changes nothing (true).
    bool Release(uint8_t usage, uint32_t waitMs = 20);
    bool ReleaseAll(uint32_t waitMs = 20);
    // Press and release.
    bool Tap(uint8_t usage, uint32_t waitMs = 20);
    // Types printable ASCII as a US keyboard does ("\n" Enter, "\t" Tab; "\r" is skipped), each character
    // pressed and released, waiting for room as the computer reads (about 500 characters a second). The keys
    // held go up while it types and come back after. False when no computer has the keyboard, when it took
    // longer than `timeoutMs`, or when a character has no key (that one is skipped).
    bool Type(const char* text, uint32_t timeoutMs = 2000);
    // What the computer set the keyboard's lights to (its output report: caps lock and so on). 0 without one.
    inline constexpr uint8_t kLedNumLock = 0x01;
    inline constexpr uint8_t kLedCapsLock = 0x02;
    inline constexpr uint8_t kLedScrollLock = 0x04;
    uint8_t KeyboardLeds();

    // ---- Media keys (report ID 3, Config::consumer): HID usage page 0x0C ----------------------------------
    inline constexpr uint16_t kMediaPlayPause = 0x00CD;
    inline constexpr uint16_t kMediaNext = 0x00B5;
    inline constexpr uint16_t kMediaPrevious = 0x00B6;
    inline constexpr uint16_t kMediaStop = 0x00B7;
    inline constexpr uint16_t kMediaMute = 0x00E2;
    inline constexpr uint16_t kMediaVolumeUp = 0x00E9;
    inline constexpr uint16_t kMediaVolumeDown = 0x00EA;
    inline constexpr uint16_t kMediaBrightnessUp = 0x006F;
    inline constexpr uint16_t kMediaBrightnessDown = 0x0070;
    // One media key at a time: pressing another replaces it.
    bool MediaPress(uint16_t usage, uint32_t waitMs = 20);
    bool MediaRelease(uint32_t waitMs = 20);
    // Press and release.
    bool Media(uint16_t usage, uint32_t waitMs = 20);

    // ---- Gamepad (report ID 4, Config::gamepad, off by default) -------------------------------------------
    // Six axes (-127..127), a hat and 32 buttons, as TinyUSB's hid_gamepad_report_t.
    enum Hat : uint8_t {
        kHatCentered = 0,
        kHatUp = 1,
        kHatUpRight = 2,
        kHatRight = 3,
        kHatDownRight = 4,
        kHatDown = 5,
        kHatDownLeft = 6,
        kHatLeft = 7,
        kHatUpLeft = 8,
    };
    struct Gamepad {
        int8_t x = 0, y = 0, z = 0, rz = 0, rx = 0, ry = 0;
        uint8_t hat = kHatCentered;
        uint32_t buttons = 0;  // bit n: button n + 1
    };
    // The whole state; sent when it differs from the last one.
    bool SetGamepad(const Gamepad& state, uint32_t waitMs = 20);
    Gamepad GamepadState();
    // The hat from four directions (up and down together cancel, as do left and right).
    uint8_t HatFrom(bool up, bool down, bool left, bool right);

}  // namespace CardenzaHAL::USB::Hid
