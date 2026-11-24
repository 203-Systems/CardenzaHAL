#pragma once

#include <cstddef>
#include <cstdint>

// USB device on the ESP32-S3's OTG port, through TinyUSB: one composite device. Its classes, each the firmware's
// choice (Config; which ones are built: Kconfig, CardenzaHAL > USB):
//   MIDI (USB-MIDI 1.0), audio (USB Audio 2.0, stereo 16-bit at kSampleRate, both ways), HID (keyboard, media
//   keys, gamepad: usb/hid.h; the system channel and the firmware's own reports), WebUSB (no endpoint: the
//   system channel for web pages), a serial port (CDC: the console, the system channel as lines, esptool's
//   reset), a USB drive (the SD card: usb/drive.h) and a camera (the screen as a lossless stream on the
//   WebUSB interface: usb/camera.h).
// Report IDs, command bytes and the SysEx form follow Matrix OS (203 Systems), so the same host tools can talk
// to both.
//
// Every 203 device can be recognised and sent into flashing over whatever it has, with nobody pressing a key:
// MIDI (SysEx), HID (report 0xCB), WebUSB and the serial port all carry the same system channel (identify, the
// development channel's info, partitions, status and settings, restart into the Launcher or into the ROM's
// download mode). A device always keeps one: without MIDI, HID and WebUSB the serial port is added. Audio, the
// drive and the camera carry none.
//
// The ESP32-S3 has four IN endpoints besides EP0. What each class takes: MIDI 1, HID 1, audio 1 and its
// feedback endpoint 1 (without it the playback is adaptive), the serial port 2, the drive 1, the camera 1 (on the
// WebUSB interface), WebUSB none. The default (MIDI, audio with feedback, HID, WebUSB) takes all four; the build stops with what to
// turn off when the classes on by default do not fit (usb/config/tusb_config.h). When a firmware lends the card
// or shows the camera, its endpoint comes from what gives way, in this order: the feedback endpoint (the
// playback becomes adaptive), HID, the serial port, audio, MIDI, each only while another command channel stays
// (usb/usb_logic.h, Fit). Init and Reconfigure log what gave way.
//
// The device's name is the same in every USB mode of the board: manufacturer "203 Systems", product the
// board's ("Cardenza"), serial number the chip's MAC as USB-Serial-JTAG gives it ("70:AF:09:BC:EB:30"), which
// is also how the ROM's download mode and the 203 Launcher show up: a computer matches the same chip across
// them by it. Interfaces say what they are ("Cardenza MIDI", "Cardenza Audio", "Cardenza HID", "Cardenza
// Serial", "Cardenza WebUSB", "Cardenza Storage"). The firmware's own name and version are in
// the development channel's INFO, not in the USB names.
//
// Starting it takes the chip's only USB PHY away from USB-Serial-JTAG: the console and esptool's port go with
// it. The system channel replaces both: the host reads the console there (and on the serial port, when there is
// one), and asks the HAL to restart into the Launcher (which brings USB-Serial-JTAG back and takes firmware
// over it) or into the ROM download mode (esptool).
//
// CardenzaHAL::Init starts it with the default Config (Kconfig CONFIG_CARDENZA_HAL_USB_AUTOSTART, on by
// default), and connects a second later: a firmware's own USB::Init(config) in that second is what the computer
// sees first; a later one makes the computer find the device again with the new classes.
namespace CardenzaHAL::USB {

    inline constexpr uint32_t kSampleRate = 44100;

    struct Config {
        uint16_t vendorId = 0x0203;   // 203 Systems
        uint16_t productId = 0x10C0;  // Cardenza (Matrix OS: 0x1040 Mystrix 1, 0x1080 Mystrix 2)
        uint16_t deviceVersion = 0x0200;  // 2.00: from the BOS descriptor (WebUSB) on
        // Not shown any more: the USB manufacturer is always "203 Systems". Kept for source compatibility.
        const char* manufacturer = "203 Systems";
        // The firmware's own name, as IDENTIFY and the development channel's INFO ("product") answer it. The
        // USB product string is the board's ("Cardenza"), whatever this says.
        const char* product = "Cardenza";
        // The classes. Each is there only when the build has it (Kconfig) and it fits (above); a device
        // keeps a command channel whatever these say.
        bool midi = true;
        bool audio = true;
        // With audio: the computer plays to the device too (false: it only records the device; no stream
        // from the computer, more bus time for the camera).
        bool audioPlayback = true;
        // The playback on the device's clock through a feedback endpoint; false (or no endpoint left):
        // adaptive, the HAL matching the rate.
        bool audioFeedback = true;
        // The HID interface (the system channel as report 0xCB, the firmware's raw reports, and the inputs
        // below).
        bool hid = true;
        // HID inputs (usb/hid.h). A HID keyboard makes macOS ask the user to identify it once, and macOS
        // then lets a host tool open the HID interface (system channel included) only with the Input
        // Monitoring permission; SysEx and WebUSB still work without it.
        bool keyboard = true;
        bool consumer = true;
        bool gamepad = false;
        // WebUSB: the system channel for a web page, on the control endpoint (no IN endpoint).
        bool webusb = true;
        // The serial port (CDC), when the build has it (CONFIG_CARDENZA_HAL_USB_SERIAL, or added because the
        // build has no other command channel).
        bool serial = true;
        // The USB drive and the camera: usually not here but asked for at run time (Drive::Share,
        // Camera::Enable), which find them an endpoint.
        bool drive = false;
        bool camera = false;
        // The console (stdout, stderr and ESP_LOG) goes to the system channel (and to the serial port).
        bool console = true;
        // Development safety: when no host has sent a system command this many seconds after Init, the
        // device restarts once without USB, so USB-Serial-JTAG (esptool, the console) is back even if no
        // host tool can reach the device. 0: never. Not for products.
        uint16_t fallbackSeconds = 0;
    };

    // Starts the device with this configuration. After CardenzaHAL::Init started it (Kconfig
    // CONFIG_CARDENZA_HAL_USB_AUTOSTART), the firmware's first Init takes it over with its own
    // configuration and returns true. False if the firmware already called it, a development fallback
    // skips this boot, or startup fails.
    bool Init(const Config& config = Config());
    bool Started();
    // Configured by a host and not suspended.
    bool Mounted();
    // Changes the classes while running: the computer sees the device leave and come back with them (MIDI
    // ports, audio streams and open files of the host start again). What does not fit gives way (above);
    // false when the device is not started.
    bool Reconfigure(const Config& config);
    // The configuration the device runs with (what the firmware asked for), and what it presents.
    Config CurrentConfig();
    struct Presented {
        bool midi = false, audio = false, audioPlayback = false, audioFeedback = false, hid = false, webusb = false, serial = false,
             drive = false, camera = false;
    };
    Presented Classes();

    // MIDI: USB-MIDI 1.0 event packets on virtual cable 0 (code index number, then three MIDI bytes).
    // System commands sent as SysEx (F0 23 <command> ... F7) are answered by the HAL and never read here.
    bool MidiRead(uint8_t packet[4]);
    bool MidiWrite(const uint8_t packet[4]);

    // Audio, interleaved stereo int16 frames at kSampleRate. Write: to the host (it records them).
    // Read: what the host plays. Both return the frames they took or gave; buffers are small (about
    // 20 ms), so call them every audio block. The recording follows the device's clock; so does the
    // playback with its feedback endpoint, and without it (adaptive) Read drops or repeats a frame now and
    // then to keep up with the host's clock: no resampling either way.
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
    // (0 releases it). False while the host is not ready for them, or without HID. usb/hid.h has
    // more (keys pressed and released, text typed, media keys, a gamepad).
    bool KeyboardReport(uint8_t modifiers, const uint8_t keys[6]);
    bool ConsumerReport(uint16_t usage);

    // HID reports of kReportSize bytes both ways (usage page 0xFF00) on two report IDs:
    //   kReportSystem: commands for the HAL. Byte 0 is the command, the answer comes back on the same
    //     report ID with the command | 0x80. The same commands work as SysEx F0 23 <command> ... F7,
    //     answered with F0 24 <command | 0x80> ... F7.
    //   kReportApp: the firmware's own; the HAL hands them to the handler (in the USB task; keep it short).
    //     A host without HID access (a web page with Web MIDI) sends them as SysEx F0 23 7F <bytes> F7.
    //     SendAppReport answers the way the last app report came: a HID report, or SysEx
    //     F0 24 7F <bytes> F7 (keep the bytes 7-bit for that). False when it did not go: USB not started,
    //     the report queue full, or over SysEx the host not reading MIDI (the answer did not arrive whole).
    //     A host that is unplugged forgets the way: the next one's first app report sets it again.
    //   The console (kCommandConsole) stops by itself when the host stops reading its SysEx, and at unplug.
    // The same two reports go over WebUSB and the serial port (docs/usb-dev.md, "Transport"):
    //   WebUSB: control transfers to the "Cardenza WebUSB" interface (vendor class, no endpoint; the page
    //     claims it): OUT, bRequest kWebUsbSend, wIndex the interface number, data [report ID, its bytes] (at
    //     most 64); IN, bRequest kWebUsbReceive, wLength 64: [report ID, 63 bytes], or no data while no answer
    //     waits (ask again).
    //   The serial port: a line "sys <hex>" is a report 0xCB, "app <hex>" a report 0xFF; answers come back as
    //     lines of the same words with the 63 bytes in hex. It also takes plain words (version, info, ...).
    // An answer goes back the way its command came; SendAppReport, the way the last app report came.
    inline constexpr uint8_t kWebUsbSend = 0x10;
    inline constexpr uint8_t kWebUsbReceive = 0x11;
    inline constexpr uint8_t kReportSystem = 0xCB;
    inline constexpr uint8_t kReportApp = 0xFF;
    inline constexpr std::size_t kReportSize = 63;
    // 2: the development channel (kCommandDev). 3: the board's settings on it (docs/usb-dev.md). 4: the
    // serial port and WebUSB carry it too, INFO is always answered, the USB names are the board's.
    inline constexpr uint8_t kProtocolVersion = 4;
    enum Command : uint8_t {
        kCommandBootloader = 0x40,  // Matrix OS: the update mode; here the Launcher, else the ROM download mode
        kCommandReboot = 0x41,      // restarts the firmware
        kCommandDownload = 0x42,    // restarts into the ROM download mode (esptool)
        kCommandConsole = 0x43,     // byte 1: 1 starts sending the console, 0 stops it; console text comes
                                    // as 0xC3, byte 1 its length, then the text (over SysEx when the
                                    // command came as SysEx: F0 24 43 <text, 7-bit> F7)
        kCommandIdentify = 0x44,    // answers protocol version, then the product name
        kCommandDev = 0x45,         // the development channel: a computer's questions about the firmware
                                    // (info, partitions, memory, card files, screen, keys; docs/usb-dev.md)
        kCommandApp = 0x7F,         // SysEx only: F0 23 7F <bytes> F7 is an app report of <bytes> (below)
    };
    using ReportHandler = void (*)(const uint8_t* report, std::size_t length);
    void SetAppReportHandler(ReportHandler handler);
    bool SendAppReport(const uint8_t* data, std::size_t length);

    // Restarts into the ROM download mode: USB-Serial-JTAG is back and esptool can write the flash.
    [[noreturn]] void RestartIntoDownloadMode();
    // Restarts into the Launcher's home screen, also when its "Resume after reset" option is on (the
    // Launcher's restart ticket, system/launcher.h). It brings USB-Serial-JTAG back. Without a Launcher
    // this simply restarts the firmware.
    [[noreturn]] void RestartIntoLauncher();

}  // namespace CardenzaHAL::USB
