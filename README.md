# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

ESP-IDF hardware drivers for Cardenza (ESP32-S3): audio, display, keyboard and per-key LEDs, SD storage, PDM microphone, IR, Func button, I2C, and USB (MIDI, audio, HID).

## Integration

The HAL is an ESP-IDF component. Add its directory to `EXTRA_COMPONENT_DIRS` and require it by its directory name; it brings its own dependencies (TinyUSB, from `idf_component.yml`):

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/cardenza")   # in the project CMakeLists.txt
idf_component_register(... REQUIRES cardenza)                     # in your component
```

Include headers relative to the HAL root (`"cardenzaHAL.h"`, `"usb/usb.h"`, ...). Outside ESP-IDF, `add_subdirectory` and `target_link_libraries(your_target PRIVATE CardenzaHAL)` still work for the drivers without USB.

## USB

`CardenzaHAL::USB::Init()` (`usb/usb.h`) turns the USB port into one composite device: USB MIDI 1.0, USB Audio 2.0 (stereo 16-bit, 44.1 kHz, both ways, asynchronous with feedback: the host follows the device's clock) and HID (keyboard, consumer keys, a system channel and the firmware's own raw reports). VID 0x0203 (203 Systems). Report IDs and command bytes follow Matrix OS: report 0xCB carries system commands (0x40 Launcher / update mode, 0x41 reboot, 0x42 ROM download mode, 0x43 console, 0x44 identify; answers come back as command | 0x80), and the same commands work as SysEx `F0 23 <command> F7`.

- It takes the chip's only USB PHY from USB-Serial-JTAG: the console moves to the system channel and esptool's port is gone until a restart into the Launcher or the ROM download mode. Holding Func while `Init` runs keeps USB-Serial-JTAG.
- The ESP32-S3 has four IN endpoints besides EP0, and the device uses all of them (MIDI, audio in, audio feedback, HID). There is no room for CDC.
- TinyUSB is configured by `usb/config/tusb_config.h` alone. Do not add `esp_tinyusb` (or anything else that configures TinyUSB) to the same firmware.
- RAM: about 8.5 KB static (the audio FIFOs are 2 KB each way), plus a 4 KB task stack once started.
- With `keyboard = true`, macOS asks the user to identify the keyboard once, and a host tool may open the HID interface (system channel included) only with the Input Monitoring permission. SysEx needs no permission.
- Tested on macOS (2026-11-02): MIDI both ways, system commands over HID and SysEx, the console, audio both ways at 44.1 kHz. On full speed the feedback goes out as 10.14 in 3 bytes (macOS stops playing after 128 ms on TinyUSB's 16.16); a host that asks for the Microsoft OS string gets 16.16 (Windows, not tested). TinyUSB's DWC2 driver does not restart an isochronous OUT transfer that missed its frame; the HAL restarts the playback endpoint after 20 ms without data.

## Launcher Apps

See the [203-launcher application registration guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow) for metadata and registration commands.
