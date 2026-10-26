# Cardenza HAL

[English](README.md) | [简体中文](README.zh-CN.md)

ESP-IDF hardware drivers for Cardenza (ESP32-S3): audio, display, keyboard and per-key LEDs, SD storage, PDM microphone, IR, Func button, and I2C.

## Integration

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

Include headers relative to the HAL root. ESP-IDF components compiling the sources directly should add this root to `INCLUDE_DIRS`.

## Audio

```cpp
#include "cardenzaHAL.h"

// After CardenzaHAL::Init(), select the mode before enabling TX:
CardenzaHAL::Audio::Init(I2S_SLOT_MODE_STEREO);
auto tx = CardenzaHAL::Audio::GetTxChannel();
```

Check initialization succeeds, then enable TX with `i2s_channel_enable(tx)`. The default mode is mono: one `int16_t` per frame, copied to both outputs. Stereo takes interleaved `L0, R0, L1, R1, ...` samples. Both use 44.1 kHz, 16-bit PCM and 32-bit I2S slots. Disable TX before switching modes. Applications own rendering and PCM conversion.

## Keyboard LEDs

After keyboard initialization:

```cpp
namespace Keys = CardenzaHAL::Keyboard;
Keys::setAllLEDs(8);
Keys::setLED({0, 0}, 15);
Keys::setLEDGlobalBrightness(10);
Keys::enableLED(true);
```

Per-key and global brightness use **0–15**. Per-key duty has exactly **16 levels**. Global brightness uses independent LED_EN hardware PWM with fixed gamma-2 compensation; it does not modify per-key values. Updates apply automatically, from one control task.

LED pins are **EN47 / CLK48 / DAT21**. Keyboard PWM reserves LEDC timer 1 / channel 6; display backlight uses timer 0 / channel 7. Both use APB. Keyboard events are `{x, y}` coordinates on a 14 × 4 matrix; applications define key mappings.

## Launcher Apps

See the [203-launcher application registration guide](https://github.com/203-Systems/203-launcher/blob/main/docs/REGISTERED_SOFTWARE.md#developer-workflow) for metadata and registration commands.
