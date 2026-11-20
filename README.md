# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

[Website](https://203.io/products/cardenza-mini-synthsiser) · [Learn Cardenza](https://cardenza.203.io) · [203 Community](https://cardenza.203.io/community) · [Firmware developer guide](https://cardenza.203.io/community/develop) · [203-CLI](https://github.com/203-Systems/203-CLI) · [Example firmware: CardenzaSynth](https://github.com/203-Systems/CardenzaSynth)

**Making a firmware for the Cardenza? Install 203c first: it is required.** `npm install -g 203c && 203c skill install`
(Node.js 18 or later; macOS, Linux and Windows alike). Every step below uses it, and a firmware on this HAL does not
build without it. Cloning the 203-CLI repository does not install it.

CardenzaHAL is the ESP-IDF hardware layer for Cardenza, an ESP32-S3 device. Use it to read the 56-key keyboard, drive individual key LEDs, draw to the 240 × 135 display, play PCM audio, access an SD card, add USB MIDI, audio, and HID to your firmware, and reach the internet over the Wi-Fi the 203 Launcher set up.

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
the shell has not. Every other way checks for `203c` too: `idf.py build` and the web target stop without it and say
how to install it ([`cmake/require_203c.cmake`](cmake/require_203c.cmake); CI builds skip that check, their publish
step installs 203c). **Never `idf.py flash` a Cardenza**: it erases the 203 Launcher and every firmware it installed;
`203c flash` installs through the Launcher.

## The hardware

What the Cardenza has, and where it is in the HAL. Pins: [`board/pins.h`](board/pins.h); schematic and PCB model:
[Cardenza-Hardware](https://github.com/203-Systems/Cardenza-Hardware).

| Part | What it is | In the HAL |
| --- | --- | --- |
| Chip | ESP32-S3, two cores at 240 MHz; 512 KB internal RAM, 2 MB quad PSRAM | ESP-IDF itself; [board defaults](#board-defaults-partitions-and-flashing), [`docs/developing.md`](docs/developing.md) (the two RAMs) |
| Flash | 16 MB, quad I/O at 120 MHz; the 203 Launcher keeps everything below 0x170000, an app gets at most 0x690000 bytes | [`board/partitions.csv`](board/partitions.csv), [`board/sdkconfig.defaults`](board/sdkconfig.defaults) |
| Keys | 56 keys, 4 rows × 14 columns (numbers 1..56); at most 4 held at once are read reliably; the caps print a character and a shifted one, the arrows are on `;` `.` `,` `/` | [`keyboard/keyboard.h`](keyboard/keyboard.h): `updateKeyList`, `keyList`, `isKeyPressing`; the caps: [`keyboard/legends.h`](keyboard/legends.h); [Keyboard and key LEDs](#keyboard-and-key-leds) |
| Key lights | a single-colour LED under each key, 16 levels, and a global brightness | [`keyboard/keyboard.h`](keyboard/keyboard.h): `setLED`, `setLEDFrame`, `setLEDGlobalBrightness`, `enableLED`; [`keyboard/led_light.h`](keyboard/led_light.h) |
| Screen | 240 × 135 colour (RGB565), ST7789 on SPI, backlight on LEDC; a whole frame is 17 ms | [`display/display.h`](display/display.h): `Present`, `PresentChanged`, `SetBrightness`, `GetPanel`; [Display](#display) |
| Speaker | ES8156 codec, I2S, 44.1 kHz 16-bit, mono speaker (stereo slots available) | [`audio/audio.h`](audio/audio.h): `Init`, `GetTxChannel`, `SetVolume`; [Audio output](#audio-output) |
| Microphone | PDM, 44.1 kHz mono; its clock shares GPIO43 with the speaker | [`mic/mic.h`](mic/mic.h); [Microphone and shared audio pins](#microphone-and-shared-audio-pins) |
| SD card | microSD, 4-bit SDMMC, FAT at `/sdcard` | [`storage/storage.h`](storage/storage.h): `Init`, `IsMounted`, `CheckAfterIoError`; [SD card storage](#sd-card-storage) |
| HOME | the side key (GPIO0; a press opens the [system menu](#system-menu-press-home-in-any-firmware); held while plugging in USB or at a reset: the ROM's download mode) | [`system/system.h`](system/system.h): `GetFuncButton`; [Func button, I2C, and IR](#func-button-i2c-and-ir) |
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
| System | Yes | Shared I2C bus and HOME button; failure stops initialization |
| Keyboard | Yes | Starts background key/LED scanning |
| Audio | Yes | Configures mono TX and starts the ES8156 codec; a new TX channel is left disabled |
| Display | Yes | Prepares the panel; present a frame to reveal it |
| Storage | No | Attempts to mount `/sdcard` |
| IR | No | Prepares the transmitter |

`LastInitReport()` exposes these six results and `overall`. A successful aggregate initialization is cached; calling it again does not remount a missing card. Retry `Storage::Init()` separately. Microphone, USB and the network ([Net](#network-wi-fi-and-https)) are **not** included, and `cardenzaHAL.h` includes neither `usb/usb.h` nor `net/net.h`. With `CONFIG_CARDENZA_HAL_PRESENCE` on (the default), `Init()` also starts the [community presence service](#community-presence-reachable-while-any-firmware-runs) once, whatever the modules' results.

You can initialize individual modules for a smaller application. Call `System::Init()` before using the shared I2C bus or HOME button. Check each module's result. Codec failures can be retried through `Audio::Init()`; retry attempts are rate limited to about once per second.

## Keyboard and key LEDs

Public API: [`keyboard/keyboard.h`](keyboard/keyboard.h), [`keyboard/types.h`](keyboard/types.h); what the key caps say: [`keyboard/legends.h`](keyboard/legends.h).

The key caps, row by row from the top (`y`), with their key numbers. Each key's own character or name is printed in black, its shifted character in yellow (written second here), and four keys carry a yellow arrow (up, down, left, right):

```text
y   keys   x=0   1      2    3    4    5    6    7    8    9    10        11        12         13
0   1..14  esc   1 !    2 @  3 #  4 $  5 %  6 ^  7 &  8 *  9 (  0 )       - _       = +        del
1  15..28  tab   q      w    e    r    t    y    u    i    o    p         [ {       ] }        \ |
2  29..42  fn    shift  a    s    d    f    g    h    j    k    l         ; : up    ' "        ok
3  43..56  ctrl  opt    alt  z    x    c    v    b    n    m    , < left  . > down  / ? right  space
```

del is printed ← (backspace), shift ⇧, space ␣; ok is enter. The left column is esc, tab, fn, ctrl from the top: fn is the third key (29), the bottom-left key is ctrl (43). HOME, the side key left of the screen, is not in the matrix ([HOME button](#home-button-i2c-and-ir)).

Use these legends; never invent a layout of your own:

- **The arrows are `;` up, `.` down, `,` left, `/` right**, the yellow triangles; not HJKL, IJKL or WASD.
- **Text input**: every key types its character, with shift its yellow one; the arrows are then fn + `;` `.` `,` `/`. del erases the character before the cursor, ok ends the input.
- A screen that types nothing (a list, a menu, a game) may take `;` `.` `,` `/` alone as the arrows, as the 203 Launcher and the system menu do.
- **ok confirms, esc (or del) goes back.** HOME is the system menu's.

[`keyboard/legends.h`](keyboard/legends.h) (a plain header, for a simulator or a test too) has them by key number: `Legend(key)` or `Legend({x, y})` gives the key's `name` (`"esc"`, `"q"`, `";"`, `"ok"`, `"space"`), `character` and `shifted` (printable ASCII; `0` for the keys that type nothing: esc, del, tab, fn, shift, ok, ctrl, opt, alt) and `arrow`; `Typed(key, shift)` is the character it types; `KeyNumber({x, y})` and `KeyPosition(key)` convert; `kKeyUp`, `kKeyDown`, `kKeyLeft`, `kKeyRight`, `kKeyEsc`, `kKeyOk`, `kKeyDel`, `kKeyTab`, `kKeyFn`, `kKeyShift`, `kKeyCtrl`, `kKeyOpt`, `kKeyAlt`, `kKeySpace` name the keys:

```cpp
#include "keyboard/legends.h"
namespace Keyboard = CardenzaHAL::Keyboard;

// A key that went down (a Point2D_t from keyList() that was not there the frame before), on a list:
switch (Keyboard::KeyNumber(key)) {
    case Keyboard::kKeyUp: MoveUp(); break;      // ;
    case Keyboard::kKeyDown: MoveDown(); break;  // .
    case Keyboard::kKeyOk: Open(); break;
    case Keyboard::kKeyEsc: Back(); break;
}
// In a text field: 0 for esc, del, tab, ok and the modifiers.
const char typed = Keyboard::Typed(key, Keyboard::isKeyPressing(Keyboard::kKeyShift));
```

- Coordinates are `x = 0..13`, `y = 0..3`. Key numbers are **1..56**, calculated as `y * 14 + x + 1`.
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

## HOME button, I2C, and IR

- [`System::GetHomeButton()`](system/system.h) returns `true` while the active-low **HOME button on GPIO0** (the side key) is held. It is separate from the matrix keyboard and its Fn key. A **press of it opens the system menu** (below): the press that opens or closes the menu reads as released.
- `System::GetI2CBus()` returns the native ESP-IDF master-bus handle after `System::Init()`. Reuse it with `i2c_master_bus_add_device()` rather than creating another bus on the same pins (SCL GPIO1, SDA GPIO2).
- Despite its name, `System::EnterDeepSleep()` currently **only deletes the shared I2C bus**. It does not enter ESP32 deep sleep or configure wake sources. Coordinate codec and other bus users before calling it.
- [`IR::Send(addr, cmd)`](ir/ir.h) sends an 8-bit NEC address/command frame with their inverted bytes, using a 38 kHz carrier on GPIO44. It initializes the transmitter if needed and waits for transmission to finish. There is no receive API or transmission status return.

## System menu (press HOME in any firmware)

A press of HOME (the side key, GPIO0) opens the [system menu](system/menu.h) in every firmware on the HAL. It is the 203 Launcher's list, drawn by the Launcher's own code and in the Launcher's language, so the two look and move as one thing:

| Item | What it does |
| --- | --- |
| Continue | back to the firmware (also ESC or del) |
| Back to Launcher | the Launcher's home screen, whatever its settings (its restart ticket: no "Boot last app" countdown) |
| Restart | this firmware again (under the Launcher: its boot ticket, no countdown) |
| Turn off | deep sleep: screen, backlight and key lights off; HOME turns the board on again and the firmware starts |
| Download mode | the ROM's download mode, to flash over USB (esptool) |

Without the menu, the Launcher opens when FN (the keyboard's bottom-left key) is held while the board starts (power-on or a reset); not HOME: held at a reset, HOME starts the ROM's download mode.

Keys are the Launcher's: `;` and `,` up, `.` and `/` down, ENTER or a tap on HOME choose, ESC or del go back; they are lit while the menu is open.

The same on the glass as the Launcher: the same greys (black, white and grey only: every pixel the menu shows has equal red, green and blue on the panel, its backdrop included) and the same brightness (the Launcher's backlight setting, its NVS `launcher/bright`, on its duty curve and its 256 Hz PWM while the menu shows, whatever your firmware's brightness; yours comes back when it closes).

How it comes and moves (the panel has no tearing-effect line, so any frame may be written across its scan: what moves changes a few pixels a frame, and what covers the screen comes pixel by pixel):

- **A press opens it.** HOME down for 30 ms (past a contact's bounce) is a press; a HOME already down when your firmware starts, or still down when the menu closes, opens nothing until it has been let go. Your picture (its last frame) goes dark and grey in one frame: its luma at two fifths, through a 4×4 ordered dither to five dark greys. 70 ms later the menu comes through it in the dither's order, a quarter of the pixels a frame, while the backlight goes to the Launcher's: each pixel is the backdrop's or the menu's, never a mix, so nothing has a colour and a frame written across the panel's scan shows no seam.
- **In the menu** the dark selection bar slides by its two edges, as in the Launcher's lists (its `ListSlide`): the edge on the side it goes leads (the Launcher's ease-out, 150 ms), the other follows from rest (230 ms), so the bar stretches toward the new row and settles back on it; a press on the way goes on from where the edges are. Text turns white or dark pixel by pixel where an edge passes, and a row's description opens as the bar covers it. Past the last row the rows change at once and the bar slides on from where it is on screen. While it moves a frame follows the last one a tick later (PSRAM: the frame is drawn once into it; without, every 10 ms), and each sends only what changed in each band of eight rows, the rows and 30-pixel segments the edges and text swept: about 1-3 KB a frame on average and 16 KB at most, where whole bands were 3-9 KB and up to 35 KB (`tests/system_menu_test.cpp` measures every slide of `--compare`).
- **Continue** shows the dark, grey backdrop of your latest frame for 60 ms, then your picture; your own frames go on from there. **The others** dim the screen as the Launcher's power-off does (600 ms) and then leave.

- `CardenzaHAL::Init` starts it when the display is up (`CONFIG_CARDENZA_HAL_SYSTEM_MENU`, on by default): a 5 KB task that reads GPIO0 every 10 ms; about 30 KB of flash. While the menu is open it borrows 2 KB of RAM and, when the firmware has PSRAM, a 64 KB copy of your latest frame there (the backdrop is made from it; without PSRAM, or when your firmware presents no frame within 60 ms of the press, the backdrop is a dark dithered field) and a 32 KB picture of the menu (drawn once per frame; without PSRAM it is drawn band by band); while it is open, a 4 KB table of what it sent (PSRAM when there is some).
- While the menu is open your firmware keeps running (its tasks and sound), but the HAL holds back what would disturb it: `Display::Present*` send nothing and return `true` (the next frame after it closes goes whole, so your screen comes back by itself), `Keyboard::updateKeyList` reports no key (keys still down when it closes stay hidden until released), and the key lights show the menu's keys (your `setLED*` levels are kept and come back). A firmware that draws through the panel handles itself should pause while `SystemMenu::IsOpen()`.
- `SystemMenu::SetListener` tells you when it opens and closes; `SystemMenu::SetLeaveHook` runs just before the board restarts, sleeps or enters the download mode (save what must survive). With the menu on, HOME is the menu's: a firmware that needs HOME for itself calls `SystemMenu::Enable(false)` (or builds without `CONFIG_CARDENZA_HAL_SYSTEM_MENU`).
- The Launcher's code is in [system/menu/launcher/](system/menu/launcher/), copied unchanged by `tools/launcher_ui.py --launcher <203-launcher checkout>` (MIT, [licenses/203-launcher-LICENSE.txt](licenses/203-launcher-LICENSE.txt)): its text and glyph code, canvas drawing, easing and the selection bar's slide; [system/menu/frame.cpp](system/menu/frame.cpp) is its `menuFrame` and status rail on the Cardenza, ported line by line. `--check` tells whether the copy is current, `--compare` draws every frame with the Launcher's real `menuFrame` too, the still ones and the slides frame by frame (a move, key repeat, a reversal, a scroll each way, every 4 ms until the bar settles: 1,910 frames in five languages), and fails on any pixel that differs (the host tests keep those frames' hashes). The words are `system/menu/strings.json`.
- Its Chinese, Japanese and Vietnamese text uses the 203 Launcher's bitmaps of **HarmonyOS Sans** (12 px), read out of the Launcher's tables by the same tool for the menu's words only; the bitmaps are a work made with the font, which its license lets us distribute ([licenses/HarmonyOS-Sans-LICENSE.txt](licenses/HarmonyOS-Sans-LICENSE.txt)); no font file is included. The 5x7 font is NullPerator's (BSD-3-Clause, notice in [system/menu/launcher/font5x7.h](system/menu/launcher/font5x7.h)).

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

## Network (Wi-Fi and HTTPS)

Public API: [`net/net.h`](net/net.h); plain headers (no ESP-IDF, usable on a computer) [`net/launcher_settings.h`](net/launcher_settings.h), [`net/http_request.h`](net/http_request.h) and [`net/connect_order.h`](net/connect_order.h).

For a firmware that reaches the internet now and then (share a recording, check something) over the Wi-Fi networks and the community account the 203 Launcher set up. Nothing runs until asked (the [presence service](#community-presence-reachable-while-any-firmware-runs), when it is on, uses Net on its own): `ReadLauncherSettings` reads what the Launcher saved, `Connect` brings Wi-Fi up, `Send` makes HTTP(S) requests, `Disconnect` takes everything down again and gives the memory back.

```cpp
#include "net/net.h"
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace Net = CardenzaHAL::Net;

// A task of its own: every call blocks for seconds.
void WhoAmITask(void*)
{
    static Net::LauncherSettings settings;  // 1.6 KB: off the stack
    Net::ConnectReport report;
    if (Net::ReadLauncherSettings(settings) == Net::SettingsResult::Ok && settings.account.Bound() &&
        Net::Connect(settings, report) == Net::ConnectStatus::Connected)
    {
        settings.Forget();  // the Wi-Fi passwords are not needed any more
        char url[160], auth[96];
        std::snprintf(url, sizeof(url), "%s/me", settings.account.api);
        std::snprintf(auth, sizeof(auth), "Bearer %s", settings.account.token);
        const Net::Header headers[] = {{"Authorization", auth}, {"Accept", "application/json"}};
        Net::Request request;
        request.url = url;
        request.headers = headers;
        request.headerCount = 2;
        static char body[2048];
        Net::Response response;
        response.body = body;
        response.capacity = sizeof(body);
        if (Net::Send(request, response) == Net::SendStatus::Ok && response.status == 200) {
            // body holds the JSON answer (response.truncated: it was longer)
        }
        Net::Disconnect();
    }
    vTaskDelete(nullptr);
}
// xTaskCreatePinnedToCore(WhoAmITask, "net", 8192, nullptr, 2, nullptr, 0);
```

A file is sent from its descriptor, a block at a time (`kUploadBlockBytes`, 4 KB, read into internal RAM because the SD card's DMA cannot reach PSRAM), with progress after each block:

```cpp
const int fd = open("/sdcard/take.wav", O_RDONLY);
struct stat info;
fstat(fd, &info);
const Net::Header headers[] = {{"Content-Type", "audio/wav"}};
Net::Request request;
request.method = "PUT";
request.url = signedUploadUrl;
request.headers = headers;
request.headerCount = 1;
request.bodyFd = fd;
request.bodyFileLength = info.st_size;
Net::Response response;  // no buffer: only the status is wanted
Net::Send(request, response, [](void*, uint64_t sent, uint64_t total) {
    // show sent / total; return false to stop
    return true;
});
close(fd);
```

| Call | Does |
| --- | --- |
| `ReadLauncherSettings(settings)` | The Launcher's account, saved networks (passwords decrypted) and last network. `Ok`, `NoPartition` (not a Launcher's partition table) or `Unreadable` (see below). No Wi-Fi needed. |
| `Connect(settings, report, timeoutMs = 20000, cancelled, context)` | Joins the network the Launcher joined last, on its channel without a scan; otherwise scans (about 1.5 s) and tries the saved networks in range, strongest first (each try at most 15 s, two reconnects within it). `Connected`, `NoNetworks`, `NotFound`, `AuthFailed` (a password refused, or a handshake that timed out), `Failed`, `NoMemory`, `Cancelled`; `report` has the network, its signal, the time and the error. Everything is taken down again when it fails. |
| `Connected()` | Has an IP address. A dropped connection is not rejoined: call `Connect` again. |
| `Disconnect()` | Wi-Fi stopped and deinitialized, its netif and (if Net made it) the default event loop deleted. While the presence service holds the connection, it stays up for it (the firmware only lets go; power save returns to modem sleep). |
| `Send(request, response, progress, cancelled, context)` | One request over a fresh connection. `https` is checked against ESP-IDF's certificate bundle; redirects are not followed. The answer's body goes into the caller's buffer (zero-terminated, `truncated` when longer). `Ok` means an answer came, whatever its HTTP status. |
| `RunningImageDigest(hex)` | The running image's appended SHA-256 as the community knows a build (64 hex digits; what `esp_partition_get_sha256` gives). Found from the image's headers (a dozen small flash reads on the first call; the result is kept): the bootloader checked the image when it started it. |

`ValidRequest` (`net/http_request.h`) is what `Send` checks first: a method of GET, POST, PUT, DELETE, HEAD or PATCH; an `http://` or `https://` URL without user info or fragment, its path and query in printable ASCII; header names that are tokens and values without control characters (no CR or LF); no `Host`, `Content-Length`, `Transfer-Encoding` or `Connection` header (Net sets them); a body from memory or from a file, not both, of at most `INT_MAX` bytes.

**Threading.** Call Net from one task of your own, one call at a time, never from the UI or the audio task. Give it a stack of 6-8 KB (a TLS handshake runs on it; [`examples/launcher_net`](examples/launcher_net) logs what it used) and a priority below the UI. `cancelled` is polled about every 100 ms while connecting and between upload blocks; one network operation (connecting, one read or write) can still take up to `Request::timeoutMs`. Wi-Fi's task (priority 23), lwIP's (18) and the event loop's (20, always core 0) run with the options below on core 0. Net refuses to start Wi-Fi the firmware already runs itself.

**The Launcher's settings** live in the NVS partition every firmware on the device shares (`nvs`, 0x9000). Net reads three of the Launcher's namespaces and **never writes anything** to them (the only write Net ever makes to the shared NVS is the presence service's handover, `l_pending`, below):

| Namespace | Keys | Net's view |
| --- | --- | --- |
| `l_account` | `token` (`ccd_` + 64 hex), `api` (the community's API base, `https://community.203.io/api`), `user_id`, `user_name`, `device_id` (the chip's 12-hex MAC) | `LauncherAccount`; as for the Launcher, a record without a valid token or an api is no binding (`Bound()` false); a name over 63 bytes is cut at a character |
| `l_wifi` | per saved network, by the CRC-32 of its SSID: `s_<CRC>` the SSID, `p_<CRC>` the password (AES-128-CBC with PKCS#7 under a key and IV every Launcher shares, base64: obfuscated, not encrypted; empty for an open network), `b_<CRC>` = 1 | `networks[]`, at most 16; an entry missing a part or not decrypting is left out, as the Launcher does |
| `launcher` | `wifi_last` (the SSID it joined last), `wifi_ch` (its channel), `remote_install` (Settings > Account > Receive firmware; 0 off, missing on) | `lastSsid`, `lastChannel`, `receiveFirmware` |

**None of it is secret from firmware on the board.** Any firmware the device runs can read the device token, and the Wi-Fi passwords are only obfuscated: the key and IV are the same in every Launcher (and in `net/launcher_settings.h`), so anyone with a firmware on the board or a dump of its flash has the passwords. The community treats the device token as least-privilege for that reason (it reaches the device's own endpoints and public pages, never the account's settings, other credentials or personal data). Real protection would take a per-device key (an eFuse HMAC key, or flash encryption with NVS encryption), which the Launcher does not use.

The partition is opened through a copy of its partition entry marked read-only: every write or erase NVS could make goes through `esp_partition_write` / `write_raw` / `erase_range`, which refuse a read-only entry (`ESP_ERR_NOT_ALLOWED`) before the flash is touched, and NVS refuses read-write handles on it. ESP-IDF's NVS writes while it opens a partition in a few states: a blank partition (it starts a page), a page move cut by a power loss (it finishes the move), an entry half written or a value found twice after an interrupted write, blob parts that do not match. On the read-only copy the open then fails or (for some of them) drops the affected items in RAM only; Net compares NVS's count of written entries with the pages on the flash and reports any difference as `Unreadable`. It never repairs: the Launcher repairs its partition the next time it starts. A partition written by a newer NVS format, or without a free page, is `Unreadable` too. When the firmware has opened `nvs` itself, Net reads through it and leaves it open. Nothing else in Net touches NVS: Wi-Fi starts with `nvs_enable = 0` and `WIFI_STORAGE_RAM`, and the firmware turns the PHY's calibration storage off (below).

**Memory.**

- Linked (as soon as a firmware calls any Net function): about 36 KB of static internal RAM (Wi-Fi's, the PHY's and lwIP's data, and 13 KB of Wi-Fi, PHY and system code that must stay in IRAM even with the IRAM options off) and about 610 KB of flash, measured on a firmware that already runs the HAL and USB with the options below. A firmware that never calls Net is unchanged: every memory section keeps its size. With `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` (below) about 14 KB of it, the `.bss` of Wi-Fi (`net80211`, `pp`), lwIP and the supplicant, moves to PSRAM: CardenzaSynth's static internal RAM grew by 21.3 KB instead of 34 KB (2026-11-17).
- While connected: Wi-Fi's receive buffers (6 static ones of 1.6 KB from `esp_wifi_init` to `esp_wifi_deinit`, dynamic ones while frames come), the Wi-Fi, lwIP and event tasks: some 40-60 KB of internal RAM, measured by the example on the board.
- During a request: esp_http_client's buffers (1 KB, and the request line), the upload block (4 KB, internal, file bodies only) and TCP's send buffer (up to 16 KB while uploading), internal; TLS 30-50 KB (a 16 KB receive and a 4 KB send record buffer, the server's certificates, the handshake) in PSRAM with `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`.
- After `Disconnect`: lwIP's own task stays (ESP-IDF 5.3 cannot deinitialize lwIP: about 4 KB with its 3 KB stack), and NVS keeps its small manager object; ESP-IDF also leaves 8 bytes per `ReadLauncherSettings` (the partition object `nvs_flash_init_partition_ptr` makes).
- PSRAM: with the PSRAM added to the heap for those who ask ([`examples/psram_heap`](examples/psram_heap), `CONFIG_SPIRAM_USE_MEMMAP`), Wi-Fi and lwIP stay internal: ESP-IDF's `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` needs `SPIRAM_USE_CAPS_ALLOC` or `SPIRAM_USE_MALLOC` and asks for `MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM`, which such a region does not have, and `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` needs the same. So mbedTLS goes to PSRAM through Net's own allocator: with `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`, Net installs it (`mbedtls_platform_set_calloc_free`) before its first connection, PSRAM first and internal RAM when there is no PSRAM; ESP-IDF itself leaves mbedTLS on plain `calloc` in that mode. The ESP32-S3's hardware AES and SHA take PSRAM buffers: their DMA reaches PSRAM, and ESP-IDF's drivers write the cache back around it or bounce through small internal blocks. A firmware that puts PSRAM in malloc (`SPIRAM_USE_CAPS_ALLOC` / `SPIRAM_USE_MALLOC`) may use ESP-IDF's options instead.

**sdkconfig.** [`examples/launcher_net/sdkconfig.defaults`](examples/launcher_net/sdkconfig.defaults) is the reference set, after the board's defaults:

| Option | Why |
| --- | --- |
| `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n` | On, the PHY reads its calibration from NVS namespace `phy` when Wi-Fi starts and writes it after a full calibration: into the Launcher's partition if the firmware has NVS open read-write, an error line at every start if not. Off, it calibrates fully each time. |
| `CONFIG_ESP_WIFI_NVS_ENABLED=n` | Wi-Fi's own default for keeping its settings in NVS (Net turns it off at init anyway). |
| `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC=y` | TLS memory in PSRAM through Net's allocator (above). Without it, mbedTLS takes 30-50 KB of internal RAM per connection. |
| `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y` | The common half of ESP-IDF's bundle (42 roots, 17 KB) has what the community needs: ISRG Root X1 (Let's Encrypt, which Vercel uses for its own domains such as `cardenza.203.io`), GTS Root R1 and GlobalSign Root CA (`*.vercel.app`), GTS Root R4 (`*.supabase.co`, the community's storage). |
| `CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`, `CONFIG_LWIP_IRAM_OPTIMIZATION=n` | IRAM is the firmware's hot code (audio). Wi-Fi and lwIP run from flash, at some throughput. |
| `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n`, `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n` | A station on a network with a password: less code. |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4`, `DYNAMIC_RX_BUFFER_NUM=16`, `DYNAMIC_TX_BUFFER_NUM=16`, `RX_BA_WIN=6` | Wi-Fi's buffers are internal RAM (1.6 KB each; 4 static ones, as the 203 Launcher). The block-ack window may not exceed the dynamic receive buffers or twice the static ones. |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384`, `CONFIG_LWIP_TCP_WND_DEFAULT=5760` | Uploads: what TCP sends before an acknowledgement sets the speed over a long round trip (16 KB at 200 ms is about 80 KB/s, ESP-IDF's 5.7 KB under 30 KB/s). Answers are small. |
| `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`, `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`, `CONFIG_LWIP_TCPIP_TASK_PRIO=4` | Network tasks on core 0, so that core 1 stays the firmware's; lwIP's task below a firmware's UI and audio (ESP-IDF's 18 preempts them; Wi-Fi's own task stays at 23). |
| `CONFIG_LWIP_IPV6=n` | IPv4 only, as the 203 Launcher: less code and no IPv6 tables. |
| `CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS=y` | The presence service's TLS session is resumed from one request to the next: no certificate chain on the air and no key exchange or certificate check on the CPU after the first. |
| `CONFIG_SPIRAM_USE_MEMMAP=y` | PSRAM only for those who ask (examples/psram_heap). |
| `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` | Wi-Fi's, lwIP's and the supplicant's `.bss` (about 14 KB) in PSRAM, used only while connected. It works with `SPIRAM_USE_MEMMAP`, but ESP-IDF puts it at the start of the PSRAM: a firmware that adds the PSRAM to the heap itself must begin the region after `_ext_ram_bss_end` (both examples do), or the heap hands out those variables' memory. ESP-IDF then drops `SPIRAM_IGNORE_NOTFOUND`: the board no longer boots without its PSRAM. |

Certificate dates are not checked (`CONFIG_MBEDTLS_HAVE_TIME_DATE` stays off: the device has no clock).

**Limits.** 2.4 GHz networks with a password (WPA, WPA2, WPA3-Personal) or open; no enterprise networks; a hidden network is reached only as the last network (a scan cannot name it). No redirects, no keep-alive (one connection per request), no proxies, no HTTP/2. Trust comes from ESP-IDF 5.3's bundle only: Let's Encrypt's newer roots (YR, YE) are not in it, and sites under them work while they send the chain cross-signed by ISRG Root X1 (as they do in 2026). The response body is only what fits the caller's buffer.

## Community presence (reachable while any firmware runs)

Public API: [`net/presence.h`](net/presence.h); its decisions, as plain functions tested on a computer: [`net/presence_logic.h`](net/presence_logic.h).

On the 203 community's website, a firmware's page can send that firmware to a signed-in person's bound device, to install it or to install and start it, and 我的设备 can update the device's Launcher. The 203 Launcher takes such commands in its menus; the presence service takes them while **any firmware on the HAL** runs, so the device is reachable (and the community sees it online, in a firmware) whatever it is doing. With `CONFIG_CARDENZA_HAL_PRESENCE` on (menuconfig › CardenzaHAL; **on by default**), `CardenzaHAL::Init()` starts it, and a firmware that does nothing else is online:

1. `CONFIG_CARDENZA_HAL_PRESENCE_START_DELAY` seconds (10) after `Init()`, a task (priority 1, core 0, 8 KB of stack) reads the Launcher's settings. It goes on only when the Launcher bound the device to the community (`l_account`, for one of the community's own hosts), saved a Wi-Fi network and has **Settings › Account › Receive firmware** on; otherwise it ends (`State::Idle`) and costs nothing more in this run.
2. It joins Wi-Fi through [Net](#network-wi-fi-and-https) (the firmware can use the same connection: `Net::Connect` joins in, `Net::Disconnect` lets go without taking it from the service) and keeps it, in modem sleep between beacons, as long as the firmware lets it (below).
3. Every 15 seconds (the answer's `poll`) it asks `GET <api>/launcher/inbox?from=firmware` with the device token, and with `X-Firmware-Name`, `X-Firmware-Version` (the app description's project name and version) and `X-Image-Digest` (the image's appended SHA-256, `Net::RunningImageDigest`), so the community knows what runs. That request is the device's presence: the website offers to send only to a device that asked within the last 40 seconds, and says "device off or not online" otherwise.
4. A command that comes is **handed over**: the firmware's hook runs (`SetHandoverHook`: save what must not be lost, or refuse); the command (its id, its action `install`, `install_start` or `update_launcher`, the unlock token of a private firmware when the community sent one, and the community's address) is written to the shared NVS namespace `l_pending`; the community is told (`POST <api>/launcher/commands/<id>` `{"status":"handover"}`); and the device restarts into the Launcher (`System::RestartIntoLauncher`, below). The Launcher clears `l_pending` first, asks the community for the command (`GET <api>/launcher/commands/<id>`, whose Item carries the build's signed statement), and carries it out like a command from its own inbox, reporting each step. The format is the Launcher's: 203-launcher `docs/SHARED_SETTINGS.md`. Any other command in the same answer is refused at once (`failed`, "Unsupported command" or "Device busy"), so the website does not wait for it.

| Call | Does |
| --- | --- |
| `Start()` | Starts the service once (`Init()` calls it with the option on). |
| `Enable(bool)` | On by default. Off stops asking and lets go of Wi-Fi (down unless the firmware holds it through Net); it returns once the service has stopped, so call it from a task that may wait. Before `Init()` it keeps the service from starting. Use it before a firmware runs Wi-Fi itself (Wi-Fi has one owner: with the service's up, the firmware's `esp_wifi_init` fails). |
| `Planned()` | Whether the service goes online in this run (on, bound to an allowed community, a saved network, Receive firmware on). Reads the settings once (read-only, a few ms); a firmware may ask at its start to keep room for Wi-Fi. |
| `SetActivity(Activity)` | What the firmware is doing; returns at once, any task. `Idle` (the default): Wi-Fi may come up and requests go on. `RealTime` (sound playing): Wi-Fi does not come up (its first start in a run calibrates the radio, which keeps core 0 busy for a while), a connection that is up keeps asking. `Quiet`: the service lets go of Wi-Fi (its RAM comes back, a request under way is cancelled) and asks nothing until the activity changes. While the activity keeps Wi-Fi down (`Quiet`, or `RealTime` before it came up) the service's task ends and its 8 KB stack returns to the firmware; the next change starts it again. |
| `SetRamFloor(bytes)` | The internal RAM the firmware keeps for itself (24 KB by default): Wi-Fi comes up only while that much stays free beside what Wi-Fi takes (measured when it first comes up; until then 40 KB, 30 KB when its own allocations go to PSRAM) and a request, and is let go when free internal RAM falls below it (then not again for 60 s, doubling to 10 minutes while the firmware keeps needing it). |
| `GetState()` | `Off`, `Idle` (nothing to do in this run), `Connecting`, `Online`, `Waiting` (after a failure, or giving way), `SignedOut` (the community refused the token), `HandingOver`. |
| `GetReport()` | For diagnostics: the state, why it waits (`Activity`, `AudioLate`, `Ram`, `FirmwareRequest`, `Failure`), requests answered and failed, Wi-Fi's starts and let-goes, the internal RAM Wi-Fi took (`wifiBytes`), the last request's time, the free internal RAM it saw. |
| `SetHandoverHook(hook, context)` | `bool hook(const Handover&, void*)` runs on the service's task before the restart: the command's id and action, whether it starts or updates the Launcher, the firmware's name and version for a message. `false` refuses it (the website shows "Refused by the running firmware"). |

**It gives way** (`Logic::Decide`, in this order). `Quiet` lets go of Wi-Fi. Below its RAM floor the firmware gets Wi-Fi's RAM back. While the speaker's DMA runs late (`Audio::GetTxCounters()`: blocks sent with nothing new written), nothing starts. Without Wi-Fi, `RealTime` waits, and Wi-Fi comes up only when the floor, Wi-Fi's cost and a request fit (and a block for its task): Wi-Fi up with no room to ask would only hold the RAM. With Wi-Fi, the firmware's own Net request waits, and a request starts only when the floor and the request's RAM fit (12 KB and an 8 KB block with TLS in PSRAM, 10 KB with Wi-Fi's own allocations in PSRAM too, 48 KB and a 20 KB block without). A request during which the audio ran dry pauses requests for 60 s, doubling up to 5 minutes; after a failure it waits 5 s, doubling up to 5 minutes; a request `cardenza.203.io` does not answer is asked of its twin `apps.203.io` (the same server; 203-launcher 0.3.3 does the same) 2 s later, and the next join starts from the binding's API again. It does **not** stop because the speaker plays: a firmware that writes audio all the time (a synthesizer renders silence too) would never be reachable; a firmware says `RealTime` or `Quiet` instead. The task runs at priority 1 on core 0, so it only gets time nothing else wants; with the reference sdkconfig Wi-Fi's task (23) and lwIP's (4, below a firmware's UI and audio) are on core 0 too, and Wi-Fi and lwIP run from flash, so IRAM stays the firmware's. There are no flash writes and no long critical sections in the service (the handover's one write aside).

**What a firmware with real-time audio does.** Say `RealTime` while sound plays (Wi-Fi then never starts in the middle of it, and stays up if it is), `Quiet` for stretches that need every byte or cycle (a recording, a big load), and `Idle` otherwise; set a RAM floor that covers what it allocates while running (CardenzaSynth keeps 24 KB: its effects and engines check for 16-20 KB free, and it says `Quiet` while its voices want the RAM Wi-Fi holds). Firmware that never call these are `Idle` with a 24 KB floor.

**Where the token goes.** Every firmware on the board can read the device token (see Net), so it is treated as public-ish: the service sends it only over https, to the hosts in `presence_logic.h` (`community.203.io`, `cardenza.203.io`, and `apps.203.io`, the live community's twin behind a CDN; port 443), and asks nothing at all for a binding that names another address (an SD card or a test setting cannot redirect it). It reaches only the inbox and the command reports (the community's device credential is least-privilege). `CONFIG_CARDENZA_HAL_PRESENCE_LOCAL_HTTP` (debug builds only, off by default) adds http:// to a loopback or private IPv4 address for a test community. The log never shows the token, an unlock token or a URL's path. A 401 `signin_required` (or 403 `banned`) stops the service until the next start, after reading `l_account` once more in case the Launcher replaced the token (it rotates it daily, but never while a firmware runs); `l_account` itself is never touched. The binding is read whenever Wi-Fi comes up.

**Back to the Launcher.** [`system/launcher.h`](system/launcher.h) `System::RestartIntoLauncher()` writes the Launcher's restart ticket (two words in RTC fast memory that the Launcher's bootloader reads and clears: its home screen, also with "Resume after reset" on and without the "Boot last app" countdown), hands the USB PHY back to USB-Serial-JTAG and restarts. `USB::RestartIntoLauncher()` (the 0x40 command) now does the same after telling the USB host. Without the Launcher it restarts the firmware.

**The one NVS write.** The handover writes `l_pending` (and nothing else) into the shared NVS partition, once, right before the restart: the firmware's own open partition if it has one, else NVS opened read-write as the Launcher opens it at its start (it repairs what a cut write left, which the Launcher would do a moment later anyway). A partition that only an erase would open is left alone, and the handover fails ("Could not hand over to the Launcher"). A flash write stops the cache for a few milliseconds: key LEDs may flicker once before the restart.

**Cost** (ESP-IDF 5.3.5, 2026-11-17, the reference sdkconfig of [`examples/launcher_net`](examples/launcher_net/sdkconfig.defaults)).

| | Measured | How |
| --- | --- | --- |
| Static internal RAM | **+22.6 KB** (IRAM code +13.1 KB: Wi-Fi's, the PHY's and the system's that must stay in IRAM; .data +8.1 KB; .bss +1.4 KB); about 12 KB more of Wi-Fi's and lwIP's .bss goes to PSRAM | `examples/community_presence` with the option on against off (`esp_idf_size --diff`); CardenzaSynth: +21.3 KB |
| Flash | **+610 KB** (Wi-Fi, lwIP, the supplicant, mbedTLS, the certificate bundle) | the same builds |
| Internal RAM while Wi-Fi is up | measured on the board: `GetReport().wifiBytes`, and the log's `Wi-Fi up: it took … of internal RAM`. CardenzaSynth (2026-11-18): **30.1 KB** with Wi-Fi's own allocations in PSRAM (below), 38.6 KB without; what stays internal: Wi-Fi's task (6.6 KB) and queues, its 4 static receive buffers (6.4 KB, DMA), lwIP's and the event loop's tasks (3.9 and 3.2 KB), the driver's internal blocks; plus this task's 8 KB stack | heap trace by caller on the board |
| Per request | resumed TLS session: about 2 KB on the air and a few milliseconds of CPU in lwIP's and Wi-Fi's tasks; the first request of a connection a full handshake (about 6 KB, ECDHE on P-256 in software: tens of milliseconds at priority 1, and 30-50 KB of TLS memory for a moment, in PSRAM). The 16 KB answer and its commands in PSRAM; a kept client of 2 KB internal between requests. Internal RAM went down by 9.5 KB during a full handshake and 6 KB during a resumed one (CardenzaSynth, Wi-Fi's allocations in PSRAM): a request waits for 10 KB beside the firmware's floor then, 12 KB otherwise | the log's first request line, `GetReport().lastAskMs` |
| Between requests | modem sleep: Wi-Fi wakes for the AP's beacons (a fraction of a millisecond on core 0, a few times a second) | |

Successful requests are logged at debug level only, and so are the routine steps of Wi-Fi coming up again after a firmware let it go: the service adds a line to a firmware's log only when something happens that would explain a problem (the first time online, why it waits for RAM, a let-go for RAM, failures, a refused token, a handover).

**Wi-Fi's own allocations in PSRAM.** The Wi-Fi driver asks for internal RAM where it needs it (its DMA receive buffers, queues, task) and for the rest through the adapter table's `_wifi_malloc` family: its configuration (3.5 KB), the station, the management frames (5 × 600 B) and the frames it copies for lwIP while they come in. ESP-IDF's `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` sends exactly these to PSRAM first, but only with `SPIRAM_USE_CAPS_ALLOC` or `SPIRAM_USE_MALLOC`. A firmware that adds its PSRAM to the heap itself for `MALLOC_CAP_SPIRAM` alone (`CONFIG_SPIRAM_USE_MEMMAP`, [`examples/psram_heap`](examples/psram_heap)) gets the same from Net: `esp_wifi_init` gets a copy of the driver's table with those four pointing to PSRAM-first allocators (`net.cpp`, `WifiFuncs`), when the heap has 64 KB of PSRAM free; lwIP stays internal. Without PSRAM in the heap, or with ESP-IDF's option on, the driver's own table is used.

**Off.** `CONFIG_CARDENZA_HAL_PRESENCE=n`: nothing of the service or of Net is linked, and the API does nothing (`GetState()` is `Off`, `Planned()` false). The web target has no network: there the API is present and the service absent.

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
| GPIO0 | HOME button (the side key; a press opens the system menu) and ROM boot selection |
| Internal USB PHY | USB device or USB-Serial-JTAG |
| Wi-Fi radio | Net: the firmware between `Connect` and `Disconnect`, and the presence service while it runs (`Presence::Enable(false)` before running Wi-Fi yourself) |

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
- [`examples/launcher_net`](examples/launcher_net): reads the Launcher's settings, joins its Wi-Fi, makes one HTTPS request and takes everything down, logging time and RAM at each step; its `sdkconfig.defaults` is the reference set of options for a firmware using Net. `idf.py -C examples/launcher_net build`.
- [`examples/community_presence`](examples/community_presence): `CardenzaHAL::Init()` with the presence service, a handover hook, the service's state in the log. `idf.py -C examples/community_presence build`.
- [`examples/hal_check`](examples/hal_check): the board through the HAL, key by key: every key's cell and LED, a tone per held key from an audio task, HOME. It builds for the board and for the browser alike.
- [`.github/actions/community-publish`](.github/actions/community-publish/action.yml): the standard release of every firmware, a GitHub Action that publishes a version to the 203 community with `203c` and trusted publishing (dev for a push to main, beta and stable for `vX.Y.Z-beta.N` and `vX.Y.Z` tags; [Releasing](#releasing)).
- [`.github/actions/launcher-publish`](.github/actions/launcher-publish/action.yml): the old Launcher hub, only for the transition: 203 Systems' official firmware mirror their releases there while 203 Launchers 0.2.0, which read only the hub's catalog, are still in use. New firmware do not use it.
- **AI agents and the 203 community**: for publishing a firmware's versions, reading its feature requests, check reports or sending it to a device, install the 203 community skill once: `curl -fsSL https://cardenza.203.io/community/skill/install.sh | sh` (into `~/.claude/skills/203-community`; details on https://community.203.io/agents).

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

[`tests/`](tests) checks the HAL on a computer, without a board or ESP-IDF: the pure calculations (key debounce and the key-count guard, LED frames, the backlight curve, and Net's: the Launcher's saved-network format against vectors made by the Launcher's own code, request checks, the order networks are tried in; the presence service's decisions and the Launcher's restart ticket), the driver sources against fake ESP-IDF drivers that fail on purpose, so every error path runs, and the web target's kernel and models (`tests/web`). CI runs them on every push and pull request.

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
- **[203 Launcher](https://github.com/203-Systems/203-launcher)** (MIT, from [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)): the system menu runs its drawing, text and motion code; license in [licenses/203-launcher-LICENSE.txt](licenses/203-launcher-LICENSE.txt).
- **HarmonyOS Sans** (Huawei Device Co., Ltd.): the system menu's Chinese, Japanese and Vietnamese text is drawn with bitmaps made from it (via the 203 Launcher); license in [licenses/HarmonyOS-Sans-LICENSE.txt](licenses/HarmonyOS-Sans-LICENSE.txt).
