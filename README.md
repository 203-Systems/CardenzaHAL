# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

CardenzaHAL is the ESP-IDF hardware layer for Cardenza, an ESP32-S3 device. Use it to read the 56-key keyboard, drive individual key LEDs, draw to the 240 × 135 display, play PCM audio, access an SD card, and add USB MIDI, audio, and HID to your firmware.

The HAL provides hardware initialization and native ESP-IDF handles. Your application owns rendering, key mappings, audio processing, file formats, and the main loop. This repository is a component, not a standalone firmware or a desktop simulator.

## Requirements

- The current Cardenza board; pin assignments are in [`board/pins.h`](board/pins.h).
- ESP-IDF **5.3 or later**, with its environment activated and the ESP32-S3 tools installed. The manifest declares this minimum; it is not a compatibility test of every later release.
- C++17 or later. Use `.cpp` for code that includes the HAL headers.
- ESP-IDF Component Manager enabled. [`idf_component.yml`](idf_component.yml) pulls in `espressif/tinyusb` `~0.21.0`.

The drivers require ESP-IDF. The alternate CMake target `CardenzaHAL` also needs an embedded ESP-IDF environment; it does not enable host builds.

## Quick start

This example lights the display and illuminates each key while it is held. USB stays on USB-Serial-JTAG until you explicitly initialize the USB device module.

### 1. Add the component

From your firmware project root:

```sh
mkdir -p components
git submodule add https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL
```

After cloning a firmware that already contains this submodule, run `git submodule update --init --recursive` instead. If the firmware is not a Git repository, use `git clone https://github.com/203-Systems/CardenzaHAL.git components/CardenzaHAL`.

Use this project layout:

```text
my-firmware/
├── CMakeLists.txt
├── components/
│   └── CardenzaHAL/
└── main/
    ├── CMakeLists.txt
    └── main.cpp
```

Root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cardenza_hello)
```

`main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES CardenzaHAL)
```

ESP-IDF discovers `components/` automatically. If you keep the HAL elsewhere, set `EXTRA_COMPONENT_DIRS` **before** including `project.cmake`. The `REQUIRES` name must match the HAL directory name: a directory named `cardenza` requires `REQUIRES cardenza`. Include paths remain relative to the HAL root, such as `"cardenzaHAL.h"`.

### 2. Initialize and read keys

`main/main.cpp`:

```cpp
#include "cardenzaHAL.h"
#include <cstdint>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void app_main()
{
    if (!CardenzaHAL::Init()) {
        const auto& r = CardenzaHAL::LastInitReport();
        ESP_LOGE("app", "HAL init failed: system=%d keyboard=%d audio=%d display=%d",
                 r.system, r.keyboard, r.audio, r.display);
        return;
    }

    namespace Display = CardenzaHAL::Display;
    namespace Keyboard = CardenzaHAL::Keyboard;

    // Static storage keeps this 64,800-byte buffer off the task stack.
    static uint16_t framebuffer[Display::WIDTH * Display::HEIGHT] = {};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < Display::WIDTH; ++x)
            framebuffer[y * Display::WIDTH + x] = 0xFFFF;

    if (!Display::SetBrightness(160) || !Display::Present(framebuffer)) {
        ESP_LOGE("app", "Display submission failed");
        return;
    }

    Keyboard::setLEDGlobalBrightness(8);
    Keyboard::enableLED(true);  // Key LEDs start disabled.
    while (true) {
        Keyboard::updateKeyList();
        Keyboard::LedFrame leds{};
        for (const auto& key : Keyboard::keyList())
            leds[key.y * Keyboard::LED_COLUMNS + key.x] = 15;
        Keyboard::setLEDFrame(leds);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

### 3. Build and run your firmware

In an activated ESP-IDF shell, from the firmware root:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with your device port. The example shows a white strip on a black screen; held keys light up and turn off when released. A missing SD card is allowed. This example initializes the audio hardware but does not play sound.

These are commands for your consuming firmware. Launcher-distributed applications also need the appropriate partition layout and metadata; see [Launcher applications](#launcher-applications).

## Initialization and errors

[`CardenzaHAL::Init()`](cardenzaHAL.cpp) initializes the modules in this order:

| Module | Required for `Init()` to return `true` | Notes |
| --- | --- | --- |
| System | Yes | Shared I2C bus and Func button; failure stops initialization |
| Keyboard | Yes | Starts background key/LED scanning |
| Audio | Yes | Configures mono TX and starts the ES8156 codec; a new TX channel is left disabled |
| Display | Yes | Prepares the panel; present a frame to reveal it |
| Storage | No | Attempts to mount `/sdcard` |
| IR | No | Prepares the transmitter |

`LastInitReport()` exposes these six results and `overall`. A successful aggregate initialization is cached; calling it again does not remount a missing card. Retry `Storage::Init()` separately. Microphone and USB initialization are **not** included, and `cardenzaHAL.h` does not include `usb/usb.h`.

You can initialize individual modules for a smaller application. Call `System::Init()` before using the shared I2C bus or Func button. Check each module's result. Codec failures can be retried through `Audio::Init()`; retry attempts are rate limited to about once per second.

## Keyboard and key LEDs

Public API: [`keyboard/keyboard.h`](keyboard/keyboard.h), [`keyboard/types.h`](keyboard/types.h).

- Coordinates are `x = 0..13`, `y = 0..3`. Key numbers are **1..56**, calculated as `y * 14 + x + 1`. Character mappings belong in your application.
- Call `updateKeyList()` before reading `keyList()`, `isPressed()` (held-key count), or `isKeyPressing(number)`. Treat the returned list as a read-only snapshot; copy it if you need to retain it.
- `getKey()` refreshes the snapshot and returns the first held coordinate, or `{-1, -1}`. `getKeyNum()` returns `0` for negative coordinates; pass only valid coordinates otherwise.
- `isChanged()` compares the **key count**, not the complete set. For press/release events, compare successive snapshots; replacing one key with another can leave the count unchanged.
- `KEY_PRESS_LIMIT` is **4**, including modifiers. Sampling more than four keys blocks new presses until their stable release; confirmed keys can still release. Do not assume unrestricted rollover.

All LED brightness values are **0..15** and are clamped to that range. Enable output with `enableLED(true)`. Use `setLED({x, y}, level)`, `setAllLEDs(level)`, or `clearAllLEDs()`. `enableLED(false)` preserves stored levels; its return value is the resulting enabled state, so `false` also means a successful disable.

For animation, fill a `LedFrame` at index `y * LED_COLUMNS + x`, then call `setLEDFrame()` to publish once. Global brightness is independent of the stored per-key levels: `setLEDGlobalBrightness(0)` darkens all keys, and `15` is maximum. Per-key output uses four bit planes; global output uses a fixed gamma-2 PWM curve. Call key snapshot and LED control APIs from one application control task.

## Display

Public API: [`display/display.h`](display/display.h).

- Landscape size: **240 × 135**, RGB565, `WIDTH * HEIGHT` 16-bit pixels (**64,800 bytes**), row stride `WIDTH`. The panel is configured for little-endian RGB data.
- `Present(framebuffer)` submits a full frame synchronously. The buffer can be in PSRAM; the HAL copies through its own internal DMA band. The caller can reuse the framebuffer when the call returns, including on failure.
- To update a region, modify the full framebuffer, then submit rectangles into that buffer:

```cpp
const CardenzaHAL::Display::Region dirty{0, 0, 40, 16};
bool ok = CardenzaHAL::Display::Present(framebuffer, &dirty, 1);
```

Rectangles must have positive dimensions and stay within the screen. The first submission and recovery after a failed submission always send the full screen. A zero rectangle count means unchanged pixels after that initial full frame.

`SetBrightness(0..255)` takes a perceived brightness level; the HAL applies gamma 2.2. The request is staged until the first successful `Present()`. Cold boots reset the panel; software restarts preserve its previous pixels until presentation.

For a native renderer, use `GetPanel()` / `GetPanelIO()`, then the no-argument `Present()` to drain queued draws and reveal the frame. Do not combine a custom panel-IO callback with the framebuffer `Present()` path. Keep display submission and control on one task.

## Audio output

Public API: [`audio/audio.h`](audio/audio.h).

The ES8156 output runs at **44,100 Hz**, using signed 16-bit PCM in **32-bit I2S wire slots**. `Audio::Init()` defaults to mono: one sample per frame is copied to both wire slots. `Audio::Init(I2S_SLOT_MODE_STEREO)` consumes interleaved left/right samples. Disable the native TX channel before changing modes.

After initialization, enable and write the native channel yourself. This function plays about one second of a quiet 440 Hz tone; call it after successful HAL initialization:

```cpp
#include "audio/audio.h"
#include <cmath>
#include <cstdint>
#include <esp_err.h>

void PlayTone()
{
    namespace Audio = CardenzaHAL::Audio;
    if (!Audio::Init(I2S_SLOT_MODE_STEREO) || !Audio::SetVolume(30)) return;
    auto tx = Audio::GetTxChannel();
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    int16_t pcm[128 * 2];
    int sampleIndex = 0;
    for (int block = 0; block < 345; ++block) {
        for (int frame = 0; frame < 128; ++frame, ++sampleIndex) {
            const auto sample = static_cast<int16_t>(
                2000.0 * std::sin(6.283185307179586 * 440.0 * sampleIndex / 44100.0));
            pcm[frame * 2] = sample;
            pcm[frame * 2 + 1] = sample;
        }
        size_t written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(tx, pcm, sizeof(pcm), &written, 1000));
        if (written != sizeof(pcm)) break;
    }
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
}
```

`i2s_channel_write()` takes **bytes** and a timeout in **milliseconds**. Production audio should handle partial writes and continuously feed blocks from its audio task. The HAL leaves sample generation, mixing, PCM conversion, and scheduling to the application.

`SetVolume(0..100)` caches the requested volume and applies it only if the codec is ready. It returns `false` when the codec is unavailable or the I2C write fails; it does not initialize the codec. `GetVolume()` returns the requested level. Use `IsCodecReady()` to check codec state.

## Microphone and shared audio pins

Public API: [`mic/mic.h`](mic/mic.h).

The PDM input produces **44,100 Hz, mono, signed 16-bit PCM** on I2S0. Output uses I2S1, but microphone CLK and speaker WS share **GPIO43**. Coordinate ownership in the application; these interfaces cannot independently run at the same time on that pin.

To switch from playback to recording:

1. Stop the audio writer and disable its TX channel with `i2s_channel_disable()`.
2. Call `Audio::StandbyCodec()` and check success.
3. Call `Mic::Init()` and obtain `Mic::InputChannel()`; check both results.
4. With RX disabled, call `Mic::RestoreInputPins()` (also needed when returning to an existing microphone channel).
5. Enable RX with `i2s_channel_enable()`, then use `i2s_channel_read()`.

To resume playback, stop the reader, disable RX, call `Audio::RestoreOutputPins()`, then `Audio::Init()` with the desired slot mode, and enable TX. Check each result before continuing. `InputChannel()` may initialize RX, but does not enable it. The HAL does not provide recording start/read/stop wrappers or automatic pin arbitration.

## SD card storage

Public API: [`storage/storage.h`](storage/storage.h).

`Storage::Init()` mounts a FAT filesystem at **`/sdcard`** using 4-bit SDMMC, with up to five open files. A failed mount does **not** format the card. Use standard C/C++ file I/O after checking `IsMounted()`:

```cpp
#include "storage/storage.h"
#include <cstdio>

void ReadSettings()
{
    if (!CardenzaHAL::Storage::Init()) return;
    FILE* file = std::fopen("/sdcard/settings.txt", "r");
    if (!file) return;
    char line[128];
    while (std::fgets(line, sizeof(line), file)) {
        // Parse a line in your application.
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) CardenzaHAL::Storage::CheckAfterIoError();
}
```

`GetCapacity()` reports total card capacity in **bytes**, not free space. `IsMounted()` is cached state, not a card-detect signal. After an I/O failure, close affected files before `CheckAfterIoError()`: a healthy card stays mounted (for example, if full); an unresponsive card is marked unavailable and retired on the next `Init()`. Coordinate retries with all file users; automatic hot-plug management is not provided.

## Func button, I2C, and IR

- [`System::GetFuncButton()`](system/system.h) returns `true` while the active-low **Func button on GPIO0** is held. It is separate from the matrix keyboard and its Fn key.
- `System::GetI2CBus()` returns the native ESP-IDF master-bus handle after `System::Init()`. Reuse it with `i2c_master_bus_add_device()` rather than creating another bus on the same pins (SCL GPIO1, SDA GPIO2).
- Despite its name, `System::EnterDeepSleep()` currently **only deletes the shared I2C bus**. It does not enter ESP32 deep sleep or configure wake sources. Coordinate codec and other bus users before calling it.
- [`IR::Send(addr, cmd)`](ir/ir.h) sends an 8-bit NEC address/command frame with their inverted bytes, using a 38 kHz carrier on GPIO44. It initializes the transmitter if needed and waits for transmission to finish. There is no receive API or transmission status return.

## USB device usage

Include `usb/usb.h` separately and start USB after system initialization. The default configuration exposes MIDI, audio, HID keyboard, consumer keys, and the system channel. For a MIDI-only application interface:

```cpp
#include "usb/usb.h"

CardenzaHAL::USB::Config config;
config.product = "My Cardenza App";
config.audio = false;
config.keyboard = false;
config.consumer = false;
// config.fallbackSeconds = 30;  // Optional development recovery; leave 0 in products.
bool started = CardenzaHAL::USB::Init(config);
```

The system HID channel remains enabled. `Init()` returns `false` if USB is already running, a development fallback skips this boot, or startup fails. Check `Started()` and `Mounted()` separately; mounted means configured by a host and not suspended.

| API | Data and application responsibility |
| --- | --- |
| `MidiRead(packet)` / `MidiWrite(packet)` | Four-byte USB-MIDI 1.0 event packets on cable 0, not a raw three-byte MIDI stream. For example, `{0x09, 0x90, 60, 100}` is channel-1 Note On. Check the bool result. |
| `AudioWrite(buffer, frames)` | Interleaved stereo `int16_t` at 44.1 kHz, sent to the host for recording |
| `AudioRead(buffer, frames)` | The host's playback stream; returns the number of available stereo frames read |
| `KeyboardReport(modifiers, keys)` | Six HID key codes plus modifier bits; send a zero report to release keys |
| `ConsumerReport(usage)` | One consumer usage; `0` releases it |
| `SetAppReportHandler(handler)` / `SendAppReport(data, length)` | Application reports, up to `kReportSize` (63) bytes; keep handlers short because they run in the USB task |

USB audio counts are **frames**, each containing two samples (four bytes). Service both directions every audio block and handle short transfers. If the host is not recording, `AudioWrite()` returns the requested count and discards the data; check `AudioInputActive()` when you need to distinguish that case. `AudioOutputActive()` reports host playback.

USB audio does not automatically route to the speaker or microphone. Bridge it in your application, including mono/stereo conversion and shared-pin management. Apply `HostMuted()` and `HostVolume()` yourself if needed; volume is in 1/256 dB, with `0` meaning full level. `AudioStats()` exposes endpoint counters and FIFO fill in bytes.

### USB protocol and platform notes

`CardenzaHAL::USB::Init()` (`usb/usb.h`) turns the USB port into one composite device: USB MIDI 1.0, USB Audio 2.0 (stereo 16-bit, 44.1 kHz, both ways, asynchronous with feedback: the host follows the device's clock) and HID (keyboard, consumer keys, a system channel and the firmware's own raw reports). VID 0x0203 (203 Systems). Report IDs and command bytes follow Matrix OS: report 0xCB carries system commands (0x40 Launcher / update mode, 0x41 reboot, 0x42 ROM download mode, 0x43 console, 0x44 identify; answers come back as command | 0x80), and the same commands work as SysEx `F0 23 <command> F7`. The firmware's own reports (0xFF) also come as SysEx `F0 23 7F <bytes> F7` from a host without HID access (a web page with Web MIDI), and `SendAppReport` answers the way the last one came (`F0 24 7F <bytes> F7`, 7-bit). MIDI packets from the firmware and the HAL's SysEx are written one sender at a time, so a SysEx reaches the host whole.

- It takes the chip's only USB PHY from USB-Serial-JTAG: the console moves to the system channel and esptool's port is gone until a restart into the Launcher or the ROM download mode.
- The ESP32-S3 has four IN endpoints besides EP0, and the device uses all of them (MIDI, audio in, audio feedback, HID). There is no room for CDC.
- TinyUSB is configured by `usb/config/tusb_config.h` alone. Do not add `esp_tinyusb` (or anything else that configures TinyUSB) to the same firmware.
- RAM: about 8.5 KB static (the audio FIFOs are 2 KB each way), plus a 4 KB task stack once started.
- With `keyboard = true`, macOS asks the user to identify the keyboard once, and a host tool may open the HID interface (system channel included) only with the Input Monitoring permission. SysEx needs no permission.
- Tested on macOS (2026-11-02): MIDI both ways, system commands over HID and SysEx, the console, audio both ways at 44.1 kHz. On full speed the feedback goes out as 10.14 in 3 bytes (macOS stops playing after 128 ms on TinyUSB's 16.16); a host that asks for the Microsoft OS string gets 16.16 (Windows, not tested). TinyUSB's DWC2 driver does not restart an isochronous OUT transfer that missed its frame; the HAL restarts the playback endpoint after 20 ms without data.

## Hardware resource ownership

Use [`board/pins.h`](board/pins.h) as the source for GPIO assignments. Avoid reconfiguring resources owned by the HAL:

| Resource | HAL owner |
| --- | --- |
| I2C0, GPIO1/2 | System / audio codec / application I2C devices |
| I2S1, GPIO41/43/42 | Audio output |
| I2S0, GPIO43/46 | PDM microphone; GPIO43 shared with output |
| LEDC timer 0 / channel 7 | Display backlight, APB clock |
| LEDC timer 1 / channel 6 | Key LED global brightness, APB clock |
| GPIO47/48/21 | Key LED enable / clock / data |
| GPIO0 | Func button and ROM boot selection |
| Internal USB PHY | USB device or USB-Serial-JTAG |

## Board defaults, partitions and flashing

The HAL ships what the board is, as defaults a firmware can take (none is enforced):

- [`board/sdkconfig.defaults`](board/sdkconfig.defaults): 16 MB flash in quad I/O at 120 MHz (as the 203 Launcher's bootloader runs it), the 2 MB PSRAM at 120 MHz, a 1000 Hz FreeRTOS tick (`Display::Present` waits per band; at 100 Hz a screen takes 170 ms), FAT with 512-byte sectors and long names. List it first in `SDKCONFIG_DEFAULTS`, your own defaults after it:
  ```cmake
  set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL/board/sdkconfig.defaults;sdkconfig.defaults")
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  ```
- [`board/sdkconfig.debug`](board/sdkconfig.debug): for debug builds, a hung task restarts the board with a backtrace (task watchdog panic, idle tasks watched, run-time statistics).
- [`board/partitions.csv`](board/partitions.csv): the default partition table. **The device's partition table belongs to the 203 Launcher**: a firmware installed through the Launcher goes into a slot the Launcher makes. This table only matters for a board flashed directly, and then it keeps the Launcher's regions exactly where the Launcher has them (NVS, otadata, the Launcher's own slot reserved, coredump) and puts the firmware after them in `ota_0` (0x170000, 4 MB). Name it with `CONFIG_PARTITION_TABLE_CUSTOM=y` and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="<path to the HAL>/board/partitions.csv"`. Never put anything else below 0x170000.
- [`tools/cardenza_flash.py`](tools/cardenza_flash.py): flashes a build the safe way. It reads the board's partition table first and **refuses when the board has the Launcher** (a full flash would erase the Launcher and its list of installed software); install through the Launcher instead, or, while developing, write only the app image into one of the Launcher's app partitions with `--app-into <label>` (the Launcher's developer workflow: then start or register it in the Launcher). `--replace-launcher` erases the Launcher on purpose. It also warns when the build's own table would overlap the Launcher.
  ```sh
  python3 components/CardenzaHAL/tools/cardenza_flash.py -p /dev/ttyACM0 build
  ```

## Host tests

[`tests/`](tests) checks the HAL on a computer, without a board or ESP-IDF: the pure calculations (key debounce and the key-count guard, LED frames, the backlight curve) and the driver sources against fake ESP-IDF drivers that fail on purpose, so every error path runs. CI runs them on every push and pull request.

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Component not found | Match `REQUIRES` to the HAL directory name; put external paths in `EXTRA_COMPONENT_DIRS` before `project.cmake`. |
| TinyUSB configuration/build conflicts | Let this component supply `usb/config/tusb_config.h`; remove competing `esp_tinyusb` configuration. |
| `Init()` returns `false` | Inspect `LastInitReport()` and module logs. SD/IR failure alone does not fail aggregate initialization. |
| Display stays dark | Submit a successful first frame with `Present()` and request a nonzero brightness. |
| Key LEDs stay dark | Enable LEDs, set nonzero per-key levels, and set nonzero global brightness. |
| A key swap is missed | Compare complete key snapshots; `isChanged()` only detects count changes. |
| No speaker audio | Check codec readiness, volume, slot mode, TX enable, and continuous PCM writes; restore output pins after microphone use. |
| SD unavailable | Check filesystem/card and `Storage::Init()`; the HAL never formats on mount failure. |
| Serial port disappears after USB starts | Expected PHY handover. Use a system command to enter Launcher/ROM download mode. |
| USB enumerates but produces no audio | Enable a host stream and feed/read USB audio in the app; no automatic hardware routing is installed. |

## Launcher applications

See the [203-launcher application registration guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow) for metadata and registration commands.
