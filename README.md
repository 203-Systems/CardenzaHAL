# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

Hardware abstraction layer for the ESP32-S3-based Cardenza, built with ESP-IDF.

Provides drivers for audio output (I2S + ES8156), display, 14 × 4 keyboard and per-key LEDs, SD storage, PDM microphone, IR, Func button, and the shared I2C bus.

## Integration

Add this repository as a submodule in an ESP-IDF project, then link the HAL target:

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

Include headers relative to the HAL root:

```cpp
#include "cardenzaHAL.h"
#include "audio/audio.h"
```

Projects that compile the sources directly into an ESP-IDF component should add the HAL root to `INCLUDE_DIRS`.

## Audio Configuration

After `CardenzaHAL::Init()` (if used), select the mode before enabling TX:

```cpp
CardenzaHAL::Audio::Init();                     // Mono (default)
// Or: CardenzaHAL::Audio::Init(I2S_SLOT_MODE_STEREO);
auto tx = CardenzaHAL::Audio::GetTxChannel();
```

Check `Init()` succeeds before enabling the channel with `i2s_channel_enable(tx)`. Mono accepts one `int16_t` per frame and copies it to both outputs. Stereo accepts interleaved `int16_t` samples (`L0, R0, L1, R1, ...`); a mono synth using stereo mode must duplicate its samples. `GetTxChannel()` preserves the selected mode.

Repeated initialization in the same mode succeeds without reconfiguring TX. To switch modes, disable TX first; switching while enabled returns `false`. Call initialization from a single setup/control task.

## Keyboard LED Brightness

Per-key and global brightness both use **0–15** (off to maximum). After keyboard initialization:

```cpp
namespace Keys = CardenzaHAL::Keyboard;
Keys::setAllLEDs(8);
Keys::setLED({0, 0}, 15);
Keys::setLEDGlobalBrightness(10);
Keys::enableLED(true);
```

- Per-key brightness is **4-bit, exactly 16 electrical duty levels**, using four binary-weighted scan planes.
- Global brightness uses independent active-low LED_EN hardware PWM. It does not clamp or scale the per-key buffer. Its 16 settings use gamma-2 compensation by default; `setLEDGlobalPerceptualBrightness(false)` selects a linear global curve. `ledGlobalBrightness()` and `ledGlobalPerceptualBrightness()` return the current settings.
- Make lighting updates from one control task. The nominal brightness cycle is 1.536 ms (~651 Hz), before ISR and blanking overhead.

The global PWM uses LEDC timer 1 / channel 6 at 78,125 Hz with 10-bit duty; the app still selects only 16 levels. Backlight uses timer 0 / channel 7 at 1 kHz. Both use APB because ESP32-S3 low-speed LEDC timers share their clock selection. Row/column transitions force LED_EN high for blanking. These controls change duty, not analog LED current.

The default pins are EN21 / CLK48 / DAT47. For newer EN47 / CLK48 / DAT21 boards, define `CARDENZA_KEYBOARD_LED_EN_GPIO=47`, `CARDENZA_KEYBOARD_LED_CLK_GPIO=48`, and `CARDENZA_KEYBOARD_LED_DAT_GPIO=21` for all HAL translation units.

The ESP-IDF v5.3.5 test app built, flashed with hash verification, and passed startup/serial-command checks on the newer board. Optical calibration, waveform measurements, flicker, and behavior under synthesizer load remain unverified. Test apps and build artifacts are maintained outside this repository.

## 203-launcher Apps

To add a Cardenza application to [203-launcher](https://github.com/203-Systems/203-launcher), follow the [application registration guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow) for metadata and registration commands. Bare BIN imports appear as Unknown App until explicitly registered.

## Usage Notes

- Requires ESP-IDF; desktop builds are not supported.
- Keyboard events use `{x, y}` matrix coordinates. Applications define their own key mappings.
- Audio defaults to 44.1 kHz, 16-bit mono PCM with 32-bit I2S slots (64fs). Slot width and channel count are separate settings. Both modes use both wire slots. Changing slot width through native ESP-IDF APIs also requires matching ES8156 clock settings.
