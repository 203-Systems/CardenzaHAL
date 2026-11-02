#pragma once

#include <cstddef>
#include <cstdint>

// USB device on the ESP32-S3's OTG port, through TinyUSB: one composite device with USB MIDI 1.0,
// USB Audio 2.0 (stereo 16-bit at kSampleRate, both directions) and HID (keyboard, consumer keys, the
// system command channel and the firmware's own raw reports). Report IDs, command bytes and the SysEx
// form follow Matrix OS (203 Systems), so the same host tools can talk to both.
//
// Starting it takes the chip's only USB PHY away from USB-Serial-JTAG: the console and esptool's port
// go with it. The system channel replaces both: the host reads the console there, and asks the HAL to
// restart into the Launcher (which brings USB-Serial-JTAG back and takes firmware over it) or into the
// ROM download mode (esptool). Holding the Func button while Init runs keeps USB-Serial-JTAG instead.
//
// The ESP32-S3 has four IN endpoints besides EP0, and this device uses all of them: MIDI, audio to
// the host, the audio feedback endpoint and HID. Nothing else fits next to it (no CDC).
namespace CardenzaHAL::USB {

    inline constexpr uint32_t kSampleRate = 44100;

    struct Config {
        uint16_t vendorId = 0x0203;   // 203 Systems
        uint16_t productId = 0x10C0;  // Cardenza (Matrix OS: 0x1040 Mystrix 1, 0x1080 Mystrix 2)
        uint16_t deviceVersion = 0x0100;
        const char* manufacturer = "203 Systems";
        const char* product = "Cardenza";
        // Each class can be left out; the system channel is always there.
        bool midi = true;
        bool audio = true;
        // A HID keyboard makes macOS ask the user to identify it once, and macOS then lets a host tool
        // open the HID interface (system channel included) only with the Input Monitoring permission;
        // SysEx still works without it.
        bool keyboard = true;
        bool consumer = true;
        // The console (stdout, stderr and ESP_LOG) goes to the system channel.
        bool console = true;
        // Development safety: when no host has sent a system command (HID or SysEx) this many seconds
        // after Init, the device restarts once without USB, so USB-Serial-JTAG (esptool, the console) is
        // back even if no host tool can reach the device. 0: never. Not for products.
        uint16_t fallbackSeconds = 0;
    };

    // Starts the device. False if it is already running, the Func button is held, or the stack could
    // not start (USB-Serial-JTAG then stays as it was).
    bool Init(const Config& config = Config());
    bool Started();
    // Configured by a host and not suspended.
    bool Mounted();

    // MIDI: USB-MIDI 1.0 event packets on virtual cable 0 (code index number, then three MIDI bytes).
    // System commands sent as SysEx (F0 23 <command> ... F7) are answered by the HAL and never read here.
    bool MidiRead(uint8_t packet[4]);
    bool MidiWrite(const uint8_t packet[4]);

    // Audio, interleaved stereo int16 frames at kSampleRate. Write: to the host (it records them).
    // Read: what the host plays. Both return the frames they took or gave; buffers are small (about
    // 20 ms), so call them every audio block. The host's clock follows the device (asynchronous
    // endpoints with feedback): no resampling either way.
    std::size_t AudioWrite(const int16_t* frames, std::size_t count);
    std::size_t AudioRead(int16_t* frames, std::size_t count);
    // The host is recording from the device / playing to it.
    bool AudioInputActive();
    bool AudioOutputActive();
    // What has crossed the USB audio endpoints so far, and how full the FIFOs are now (bytes).
    struct AudioCounters {
        uint32_t packetsFromHost = 0, bytesFromHost = 0;
        uint32_t packetsToHost = 0, bytesToHost = 0, emptyPacketsToHost = 0;
        uint32_t fifoFromHost = 0, fifoToHost = 0;
        uint32_t playbackRestarts = 0;  // the host's stream stalled and was restarted (see usb.cpp)
    };
    AudioCounters AudioStats();
    // The host's own volume control for what it plays: mute, and the level in 1/256 dB (0 = full).
    bool HostMuted();
    int16_t HostVolume();

    // HID: a boot-style keyboard report (modifier bits, six key codes) and one consumer usage
    // (0 releases it). False while the host is not ready for them.
    bool KeyboardReport(uint8_t modifiers, const uint8_t keys[6]);
    bool ConsumerReport(uint16_t usage);

    // HID reports of kReportSize bytes both ways (usage page 0xFF00) on two report IDs:
    //   kReportSystem: commands for the HAL. Byte 0 is the command, the answer comes back on the same
    //     report ID with the command | 0x80. The same commands work as SysEx F0 23 <command> ... F7,
    //     answered with F0 24 <command | 0x80> ... F7.
    //   kReportApp: the firmware's own; the HAL hands them to the handler (in the USB task; keep it short).
    inline constexpr uint8_t kReportSystem = 0xCB;
    inline constexpr uint8_t kReportApp = 0xFF;
    inline constexpr std::size_t kReportSize = 63;
    inline constexpr uint8_t kProtocolVersion = 1;
    enum Command : uint8_t {
        kCommandBootloader = 0x40,  // Matrix OS: the update mode; here the Launcher, else the ROM download mode
        kCommandReboot = 0x41,      // restarts the firmware
        kCommandDownload = 0x42,    // restarts into the ROM download mode (esptool)
        kCommandConsole = 0x43,     // byte 1: 1 starts sending the console, 0 stops it; console text comes
                                    // as 0xC3, byte 1 its length, then the text (over SysEx when the
                                    // command came as SysEx: F0 24 43 <text, 7-bit> F7)
        kCommandIdentify = 0x44,    // answers protocol version, then the product name
    };
    using ReportHandler = void (*)(const uint8_t* report, std::size_t length);
    void SetAppReportHandler(ReportHandler handler);
    bool SendAppReport(const uint8_t* data, std::size_t length);

    // Restarts into the ROM download mode: USB-Serial-JTAG is back and esptool can write the flash.
    [[noreturn]] void RestartIntoDownloadMode();
    // Restarts into the Launcher: a plain restart, which the Launcher's bootloader routes to the
    // Launcher (unless its "resume after reset" option is on). It brings USB-Serial-JTAG back. Without
    // a Launcher this simply restarts the firmware.
    [[noreturn]] void RestartIntoLauncher();

}  // namespace CardenzaHAL::USB
