# Cardenza HAL

Hardware abstraction layer for the ESP32-S3-based Cardenza, built with ESP-IDF.

Provides drivers for audio output (I2S + ES8156), display, 14 × 4 keyboard and per-key LEDs, SD storage, PDM microphone, IR, HOME button, and the shared I2C bus.

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

## Usage Notes

- Requires ESP-IDF; desktop builds are not supported.
- Keyboard events use `{x, y}` matrix coordinates. Applications define their own key mappings.
- Audio defaults to 44.1 kHz, 16-bit mono PCM in the left I2S slot, with 32-bit slots. Stereo output requires application-side configuration and interleaved left/right samples.
