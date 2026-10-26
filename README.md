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

`setLED(pos, level)`, `setAllLEDs(level)`, and `setLEDGlobalBrightness(level)` use **0–15** (off to maximum). All 16 levels are distinct. Per-key values use a gamma-2 approximation over six binary-weighted scan planes (64 electrical duties); `setLEDPerceptualBrightness(false)` selects linear duty for comparison. App updates should come from one control task.

Global brightness drives active-low LED_EN with independent 10-bit LEDC PWM at 78,125 Hz (16 app levels mapped nonlinearly into 1,024 hardware codes). It does not clamp or scale the per-key buffer. Timer 1 / channel 6 are reserved; display backlight uses timer 0 / channel 7. Row/column changes disconnect PWM and force LED_EN high to blank the LEDs.

The nominal full brightness cycle is 6.45 ms (~155 Hz), before ISR and blanking overhead. Timing and visual quality need hardware validation under application load. These are time-duty controls, not analog current regulation. The default pin mapping remains EN21 / CLK48 / DAT47; newer boards can define `CARDENZA_KEYBOARD_LED_EN_GPIO=47`, `CARDENZA_KEYBOARD_LED_CLK_GPIO=48`, and `CARDENZA_KEYBOARD_LED_DAT_GPIO=21` for all HAL translation units.

## 203-launcher Apps

To add a Cardenza application to [203-launcher](https://github.com/203-Systems/203-launcher), follow the [application registration guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow) for metadata and registration commands. Bare BIN imports appear as Unknown App until explicitly registered.

## Usage Notes

- Requires ESP-IDF; desktop builds are not supported.
- Keyboard events use `{x, y}` matrix coordinates. Applications define their own key mappings.
- Audio defaults to 44.1 kHz, 16-bit mono PCM with 32-bit I2S slots (64fs). Slot width and channel count are separate settings. Both modes use both wire slots. Changing slot width through native ESP-IDF APIs also requires matching ES8156 clock settings.
