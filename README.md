# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

[Website](https://203.io/products/cardenza-mini-synthsiser) · [Learn Cardenza](https://cardenza.203.io) · [203 Community](https://cardenza.203.io/community) · [Firmware developer guide](https://cardenza.203.io/community/develop) · [203-CLI](https://github.com/203-Systems/203-CLI) · [Example firmware: CardenzaSynth](https://github.com/203-Systems/CardenzaSynth)

CardenzaHAL is the ESP-IDF hardware layer for Cardenza, an ESP32-S3 device. Use it to read the 56-key keyboard, drive individual key LEDs, draw to the 240 × 135 display, play PCM audio, access an SD card, and add USB MIDI, audio, and HID to your firmware.

The HAL provides hardware initialization and native ESP-IDF handles. Your application owns rendering, key mappings, audio processing, file formats, and the main loop. This repository is a component, not a standalone firmware; its web target builds your firmware into a browser simulator as well ([Web simulator](#web-simulator)).

## Making a firmware on CardenzaHAL

Two things are required: **this HAL** (every firmware reaches the hardware only through it) and
**[203-CLI](https://github.com/203-Systems/203-CLI)**, `203c`, the 203 community's command line: it makes the
project, puts a build on your Cardenza, reads its log and is the only way to publish (by hand or from CI). It comes
with an Agent Skill for coding agents and every 203 device's guide (the Cardenza's:
[`products/cardenza`](https://github.com/203-Systems/203-CLI/blob/main/products/cardenza/README.md)). The rules for
every step, for people and agents alike: [`docs/FIRMWARE-PROJECT.md`](docs/FIRMWARE-PROJECT.md).

```sh
npm install -g 203c && 203c skill install                           # 203c and its Agent Skill (Node.js 18+, any OS)
203c new "My firmware" --summary "What it does"                      # the project: this HAL as a submodule, the template
cd my-firmware
python3 components/CardenzaHAL/tools/cardenza.py doctor   # what this computer still needs (ESP-IDF 5.3.5, Emscripten, 203c)
python3 components/CardenzaHAL/tools/cardenza.py build    # the preferred build: the checks, then idf.py build
203c flash --bin build/my_firmware.bin                    # onto your Cardenza over USB, through the 203 Launcher
203c log                                                  # what it prints
203c login                                                # once on a computer: the browser asks you to approve
203c publish --dry-run && 203c publish                    # the first version; then CI publishes (Releasing)
203c sync                                                 # later: .203/community.md, what people asked for and said
```

[`tools/cardenza.py`](tools/cardenza.py) is the preferred way to build: it checks ESP-IDF 5.3, that this HAL is a
clean submodule and that `203c` is installed (and signed in) before it builds, and exports ESP-IDF for the build when
the shell has not. **Never `idf.py flash` a Cardenza**: it erases the 203 Launcher and every firmware it installed;
`203c flash` installs through the Launcher.

## The hardware

What the Cardenza has, and where it is in the HAL. Pins: [`board/pins.h`](board/pins.h); schematic and PCB model:
[Cardenza-Hardware](https://github.com/203-Systems/Cardenza-Hardware).

| Part | What it is | In the HAL |
| --- | --- | --- |
| Chip | ESP32-S3, two cores at 240 MHz; 512 KB internal RAM, 2 MB quad PSRAM | ESP-IDF itself; [board defaults](#board-defaults-partitions-and-flashing), [`docs/developing.md`](docs/developing.md) (the two RAMs) |
| Flash | 16 MB, quad I/O at 120 MHz; the 203 Launcher keeps everything below 0x170000, an app gets at most 0x690000 bytes | [`board/partitions.csv`](board/partitions.csv), [`board/sdkconfig.defaults`](board/sdkconfig.defaults) |
| Keys | 56 keys, 4 rows × 14 columns (numbers 1..56); at most 4 held at once are read reliably | [`keyboard/keyboard.h`](keyboard/keyboard.h): `updateKeyList`, `keyList`, `isKeyPressing`; [Keyboard and key LEDs](#keyboard-and-key-leds) |
| Key lights | a single-colour LED under each key, 16 levels, and a global brightness | [`keyboard/keyboard.h`](keyboard/keyboard.h): `setLED`, `setLEDFrame`, `setLEDGlobalBrightness`, `enableLED`; [`keyboard/led_light.h`](keyboard/led_light.h) |
| Screen | 240 × 135 colour (RGB565), ST7789 on SPI, backlight on LEDC; a whole frame is 17 ms | [`display/display.h`](display/display.h): `Present`, `PresentChanged`, `SetBrightness`, `GetPanel`; [Display](#display) |
| Speaker | ES8156 codec, I2S, 44.1 kHz 16-bit, mono speaker (stereo slots available) | [`audio/audio.h`](audio/audio.h): `Init`, `GetTxChannel`, `SetVolume`; [Audio output](#audio-output) |
| Microphone | PDM, 44.1 kHz mono; its clock shares GPIO43 with the speaker | [`mic/mic.h`](mic/mic.h); [Microphone and shared audio pins](#microphone-and-shared-audio-pins) |
| SD card | microSD, 4-bit SDMMC, FAT at `/sdcard` | [`storage/storage.h`](storage/storage.h): `Init`, `IsMounted`, `CheckAfterIoError`; [SD card storage](#sd-card-storage) |
| HOME | the side key (GPIO0; held while plugging in USB: the ROM's download mode) | [`system/system.h`](system/system.h): `GetFuncButton`; [Func button, I2C, and IR](#func-button-i2c-and-ir) |
| I2C | the shared bus (GPIO1/2), the codec on it | [`system/system.h`](system/system.h): `GetI2CBus` |
| IR | a transmitter (GPIO44, NEC at 38 kHz) | [`ir/ir.h`](ir/ir.h): `Send` |
| USB | the chip's USB PHY: USB-Serial-JTAG (the log, esptool), or the HAL's device: MIDI, audio both ways, HID, the system channel (log, restart into the Launcher) | [`usb/usb.h`](usb/usb.h); [USB device usage](#usb-device-usage); on the computer: `203c log`, [`tools/cardenza_usb.py`](tools/cardenza_usb.py) |
| Wi-Fi, Bluetooth | the chip's radios (2.4 GHz Wi-Fi, Bluetooth 5 LE) | ESP-IDF's own drivers, not the HAL (the browser simulator has none: [Web simulator](#web-simulator)) |

## Releasing

Every firmware on the HAL, 203 Systems' own (Cardenza Synth, the 203 Launcher) and everyone else's, is released
the same way: to the 203 community, whose channels the 203 Launcher installs and updates from.

- Channels follow the Git ref, from CI with [`.github/actions/community-publish`](.github/actions/community-publish/action.yml): a push to `main` is **dev** (`<version>-dev.<run>`), a tag `vX.Y.Z` equal to `version.txt` is **stable** (its notes are `CHANGELOG.md`'s `## X.Y.Z`), a tag `vX.Y.Z-beta.N` is **beta**.
- Trusted publishing, no stored secret: the job (`permissions: id-token: write`) signs in with its GitHub OIDC token, which the community accepts for the repositories the firmware's owner linked on its page (自动发布 / Trusted publishing) or with `203c firmware publishers add`.
- The first version is published by hand (`203c publish`), which gives the firmware its code; [`examples/template`](examples/template) has the workflow. The details: [`docs/FIRMWARE-PROJECT.md`](docs/FIRMWARE-PROJECT.md#8-publish).
- Building from source and flashing your own board stay possible; they are not releases.

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

From the firmware root (ESP-IDF 5.3 found and exported by the tool):

```sh
python3 components/CardenzaHAL/tools/cardenza.py build   # or, in an activated ESP-IDF shell: idf.py build
203c flash --bin build/cardenza_hello.bin                # through the 203 Launcher (never idf.py flash: it erases the Launcher)
203c log                                                 # its log over USB
```

A board without the 203 Launcher (a bare chip): `idf.py -p <PORT> flash monitor`. The example shows a white strip on a black screen; held keys light up and turn off when released. A missing SD card is allowed. This example initializes the audio hardware but does not play sound.

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

Two optional helpers sit above the driver. Both are plain headers, so a simulator or a test on a computer can use them too:

- [`keyboard/led_light.h`](keyboard/led_light.h): per-key levels are linear duty, which the eye does not see as linear (level 7 looks far brighter than half). `Light::PerceivedToLevel(perceived)` maps a perceived brightness 0..15 to the level to send (gamma 2.6, judged on the board; every lit level stays lit), and `Light::LightPermille(perceived, global)` gives the light a key shows, for a simulator to draw.
- [`keyboard/key_settle.h`](keyboard/key_settle.h): `KeySettle<N>` filters the keys whose wrong reading does the most damage (modifiers, navigation). A state counts once it has held 12 ms, and a key that reads as held for over 15 s counts as released until it lets go (both times can be set). Feed it each key's reading once a frame; `Glitches()` and `Stuck()` count what it filtered out, for a log.

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

A whole frame is 17 ms of SPI, most of a 50 fps frame. **`PresentChanged(framebuffer)`** works out what changed by itself and sends only that: it hashes each row in 30-pixel segments, sends the changed rows of each 8-row band across the span of their changed segments, and sends one band whole every frame so the screen is refreshed completely about every 0.34 s. The framebuffer must be 4-byte aligned (`alignas(4)`); its 4.3 KB table of hashes is made on first use (PSRAM when there is some), and without room it sends whole frames. Use it or the other `Present` calls, not both. The calculation is [`display/changed_regions.h`](display/changed_regions.h), a plain header a host test or a firmware with its own presenting can use.

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

## Host tools

Command-line tools for the computer the board is plugged into (Python 3, no board needed to read them):

- [`tools/cardenza.py`](tools/cardenza.py): the preferred way to build a firmware. `doctor` says what the project and the computer lack (ESP-IDF 5.3, git, a clean HAL submodule, `203c` and its sign-in, Emscripten) and how to install it; `build` checks first, then runs `idf.py build` (exporting ESP-IDF when the shell has not) and says how much of the Launcher's room the image takes (`--web`: the browser version too); `web` builds the browser version; `flash` and `log` hand the build to `203c flash --bin` and `203c log`. Standard library only, Linux, macOS and Windows.

- [`tools/cardenza_usb.py`](tools/cardenza_usb.py): the host side of `usb/usb.h`. `list` shows the board's HID, MIDI and audio devices; `command identify|reboot|launcher|download` restarts it (into the Launcher, or into ROM download mode, the way to flash a firmware that runs USB and so has no serial port); `console <seconds>` prints its console and log; `app <text>` talks to the firmware's own app channel. A firmware's own tool can import it. Needs `pip install hidapi`; MIDI goes through macOS CoreMIDI.
- [`tools/cardenza_flash.py`](tools/cardenza_flash.py): flashes a build without erasing the Launcher (above).
- [`tools/check_submodule.py`](tools/check_submodule.py): for a firmware that has the HAL as a git submodule, fails when the HAL has local changes or sits on a commit upstream `main` does not have, and makes pushes from it impossible. Run it before every build, so HAL changes are made upstream and every firmware gets the same HAL.

## Version and build information

[`cmake/firmware_info.cmake`](cmake/firmware_info.cmake) gives a firmware its version and what each build is, so a log or a crash can be traced to the build that made it:

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/components/CardenzaHAL/cmake/firmware_info.cmake)
cardenza_read_version(${CMAKE_CURRENT_LIST_DIR}/version.txt PROJECT_VER)   # before project(): the version the Launcher shows
# in the component that prints it:
cardenza_target_build_info(${COMPONENT_LIB} ${PROJECT_DIR} HEADER MyBuildInfo.h NAMESPACE my::build)
```

`version.txt` holds `major.minor.patch`. The generated header has `kVersion`, `kHash` (the Git commit), `kDirty` (built with local changes, the HAL included) and `kTimeUtc`. Print them at every start with the ELF's hash (`esp_app_get_elf_sha256`), and keep the ELF of every build you give anyone: a backtrace is decoded with it (`xtensa-esp32s3-elf-addr2line -pfiaC -e <firmware>.elf <addresses>`).

## Developer notes, examples and publishing

- [`docs/developing.md`](docs/developing.md): what the chip and its toolchain do that is easy to miss (two RAMs, atomics with PSRAM, `memcpy` and division in hot code, what a flash write stops, reading files, USB and the serial port, debugging a hang), and how a firmware behaves under the 203 Launcher.
- [`docs/FIRMWARE-PROJECT.md`](docs/FIRMWARE-PROJECT.md): the rules for a firmware project on the HAL and for publishing it on the 203 community with `203c` (the community serves this file at `/develop.md`).
- [`examples/template`](examples/template): a firmware project to start from: the HAL as a submodule, the board's defaults, a partition table that keeps the Launcher, `version.txt`, `CHANGELOG.md` and `203.json`. It builds for the board and the browser.
- [`examples/psram_heap`](examples/psram_heap): a small firmware that adds the PSRAM to the heap only for code that asks for it, and replaces `new` so a failed allocation is a null pointer instead of a reboot. `idf.py -C examples/psram_heap build`.
- [`examples/hal_check`](examples/hal_check): the board through the HAL, key by key: every key's cell and LED, a tone per held key from an audio task, HOME. It builds for the board and for the browser alike.
- [`.github/actions/community-publish`](.github/actions/community-publish/action.yml): the standard release of every firmware, a GitHub Action that publishes a version to the 203 community with `203c` and trusted publishing (dev for a push to main, beta and stable for `vX.Y.Z-beta.N` and `vX.Y.Z` tags; [Releasing](#releasing)).
- [`.github/actions/launcher-publish`](.github/actions/launcher-publish/action.yml): the old Launcher hub, only for the transition: 203 Systems' official firmware mirror their releases there while 203 Launchers 0.2.0, which read only the hub's catalog, are still in use. New firmware do not use it.

## Web simulator

The HAL's web target ([`web/`](web)) builds a firmware written on CardenzaHAL, from the same source, into a WebAssembly simulator that runs at full speed in a browser: the screen, the 4 × 14 keys and their LEDs, HOME, the speaker, the SD card and USB MIDI. It is the version a firmware shows in the community player, and a quick way to try a change without a board.

### Build the simulator version

You need [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (`emsdk install latest`, `emsdk activate latest`) and CMake 3.20 or later; ESP-IDF is not needed. From the firmware project (the folder with `idf.py`'s top-level `CMakeLists.txt`):

```sh
python3 components/CardenzaHAL/tools/cardenza_web.py build .
python3 components/CardenzaHAL/tools/cardenza_web.py serve build-web/site   # http://localhost:8000/
```

`build` configures the project with the HAL's ESP-IDF build shim in place of ESP-IDF (`web/idf`): your `CMakeLists.txt` files, `main/`, `components/`, `EXTRA_COMPONENT_DIRS`, `managed_components/` and your `sdkconfig` (or `SDKCONFIG_DEFAULTS`) are used as they are. The folder it writes (default `build-web/site`, `--out` to choose) holds `index.html`, `<project>.js`, `<project>.wasm`, `cardenza-sim.js` and `sim.json` (target, firmware name and version, the HAL's version and commit, the key map, each file's size and SHA-256). Any static web server can host it. It needs no cross-origin isolation (no COOP / COEP: it uses no SharedArrayBuffer), only `Access-Control-Allow-Origin: *` when it is shown from another origin (always so inside `<iframe sandbox="allow-scripts">`, whose origin is opaque), and `.wasm` as `application/wasm` (it compiles from the downloaded bytes otherwise). In such a frame the browser gives it no storage (the card lives in memory) and no MIDI or microphone; opened as a page of its own it keeps the card and NVS in IndexedDB. `serve` sends those headers and also offers the site framed that way, from a second port, at `/__sandbox.html`. `--hal <dir>` builds against another CardenzaHAL; by default the firmware's own copy is used when it has `web/`, else the one the tool belongs to (with a warning), `--debug` builds with assertions.

Exit codes: `0` built; `1` your code does not build (the compiler's output follows); `2` usage, or not an ESP-IDF project; `3` the firmware does not follow the HAL (one line each on stderr: `unsupported: <function>`, `unsupported-header: <header>`, or `unsupported-source: <file>` for assembly and prebuilt libraries); `4` Emscripten or CMake is missing (with how to install it).

The page fills its window (the board, 7:4, on black), takes the computer keyboard by key position (the table is in `sim.json` and shown by the player) and touch or mouse on its own keys (a right click latches one), starts sound on the first click or key, and tells the page around it `{"cardenza_sim":"ready"}` once `app_main` runs, then `{"cardenza_sim":"keys","keys":[…]}`; also `restarted`, `crashed` and `stopped` (three crashes in 10 s), `ir` and `app_report`. The page around it can send the firmware's app channel `{"cardenza_sim":"app","data":[…]}`. URL parameters: `?sd=0` (no card), `?sdmb=<MB>` (its size, default 8192), `?log=1` (the console under the board). `esp_restart` and a crash start a new instance in the same page (sound, screen and MIDI stay).

A firmware's CMake can tell the web build by the variable `CARDENZA_HAL_WEB` (its code by the definition of the same name), for the rare place it must: a vendored library with a web build of its own that keys on `__EMSCRIPTEN__` (which stays defined: the C library needs it), say.

### What "follows the HAL" means

A firmware follows the HAL when it reaches the hardware only through the HAL's API and the native handles it gives (the display's panel and panel IO, the I2S channels, the I2C bus, the SD card's files), plus FreeRTOS and the ESP-IDF system services. The web target provides exactly that, with the device's behaviour:

| Area | On the web target |
| --- | --- |
| FreeRTOS | Tasks, priorities, two cores (`xTaskCreatePinnedToCore`), delays on the tick, queues, semaphores, mutexes with priority inheritance, queue sets, notifications, event groups, software timers, stream and message buffers, critical sections. pthreads, `std::thread`, `std::mutex`, `std::condition_variable` and sleeps run as tasks. |
| Time | `esp_timer` (task and ISR dispatch), `esp_timer_get_time`, `xTaskGetTickCount` follow real time. |
| Memory | Internal RAM (about 300 KB free at `app_main`) and the 2 MB PSRAM as separate heaps with the device's room; `malloc`, `new`, task stacks and DMA buffers come out of them as on the board; `heap_caps_*`, `CONFIG_SPIRAM_USE_MALLOC / CAPS_ALLOC / MEMMAP`, `esp_ptr_external_ram`. A firmware that runs out of internal RAM on the board runs out here at about the same point. |
| Display | `Display::*` is the HAL's own code; the panel and panel IO handles drive a model of the ST7789 (window, MADCTL, inversion, sleep, byte order) with the SPI bus's timing (17 ms for a whole frame) and `on_color_trans_done`. LEDC on GPIO38 is the backlight. |
| Keys, LEDs, HOME | `Keyboard::*` takes the page's keys through the HAL's debouncer and four-key guard; the LEDs show the levels the scan would drive and the global brightness. `System::GetFuncButton()` and GPIO0 are HOME. |
| Audio | `Audio::*` and the ES8156 codec's registers over I2C (startup time, volume); the TX channel's DMA (descriptors × frames) is drained at 44.1 kHz by the page's audio clock, so `i2s_channel_write` blocks like on the board; `on_sent` / `on_send_q_ovf`. The microphone (`Mic::*`, PDM RX) hears the computer's microphone where the page may use it, and GPIO43 belongs to whichever side configured it last. |
| Storage | `Storage::*` mounts `/sdcard` (none when the page says so, `?sd=0`); C and C++ files work; `esp_vfs_fat_info`; NVS kept with the page. |
| USB | `USB::*` MIDI to and from the computer's MIDI ports (Web MIDI, when allowed), the system SysEx commands, the app channel (also to the page around it); USB audio and HID have no host. |
| Also | GPIO, GPTimer, RMT for `IR::Send` (decoded and shown), ESP-IDF's log, `esp_restart` (the simulator restarts the firmware), `esp_reset_reason`, `esp_random`, `esp_app_get_description`, the task watchdog's calls. |

Anything else is not provided, and using it fails the build with a message saying so: other ESP-IDF drivers and components (Wi-Fi, Bluetooth, UART, ADC, SPI devices…), register access (`soc/*_reg.h`), the display's or codec's own setup (it belongs to the HAL), Xtensa and ROM internals, `esp_private/*`. So a firmware that builds for the web follows the HAL, and one that does not gets the list of what it reaches past it.

Differences that remain: one browser thread runs the tasks, switching at every call into FreeRTOS, ESP-IDF or the HAL (and between the two cores' tasks), so a task that loops without ever calling any of them holds everything up (the log names it after half a second); `thread_local` is shared by all tasks; file names are case-sensitive and an open file can be renamed; the browser's clock is coarser than the board's (0.1 to 1 ms); sound reaches the speakers about 60 ms after the DMA reads it.

The HAL's own tests of the web target: `tests/web` (the kernel and the models, on the computer), `tests/web/browser` (the page in headless Chrome), `tests/web_tool_test.py` (the build tool).

## Host tests

[`tests/`](tests) checks the HAL on a computer, without a board or ESP-IDF: the pure calculations (key debounce and the key-count guard, LED frames, the backlight curve), the driver sources against fake ESP-IDF drivers that fail on purpose, so every error path runs, and the web target's kernel and models (`tests/web`). CI runs them on every push and pull request.

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

Upload and registration tools with setup instructions are included in [tools/uploader/README.md](tools/uploader/README.md).

## Acknowledgements

- **[M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer)**: a wonderful project, and the inspiration and the reason we set out to make the Cardenza. Thank you, M5Stack. The Cardenza and this HAL are not compatible with M5Stack devices.
- **[ESP-IDF](https://github.com/espressif/esp-idf)** (Espressif) and **[TinyUSB](https://github.com/hathach/tinyusb)**: the framework every module here is built on, and the USB stack behind `usb/usb.h`.
- **[Emscripten](https://emscripten.org)**: the web target runs the same firmware source in a browser.
