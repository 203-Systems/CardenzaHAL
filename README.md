# Cardenza HAL

ESP-IDF hardware layer for Cardenza (ESP32-S3): audio output (I2S + ES8156), display, 4×14 matrix keyboard with per-key LEDs, SD storage, PDM microphone, IR, the HOME button and the shared I2C bus.

Consumers pull this repository in as a git submodule:

| Consumer | Submodule path |
| --- | --- |
| CardenzaSynth | `Devices/Cardenza/cardenza` |
| Cardenza-MPE | `hal/cardenza` |

## Design Goals

- Keep public headers and internal includes rooted in this directory.
- Allow `add_subdirectory(cardenza)` from another CMake project.
- Avoid depending on the consumer's repository root include path.
- Expose native ESP-IDF handles rather than app-level wrappers (see [AGENT.md](AGENT.md)).

## Public Include Root

Consumers should include headers from the module root exposed by the `CardenzaHAL` target.

```cpp
#include "cardenzaHAL.h"
#include "audio/audio.h"
```

Avoid hardcoding the submodule path (for example `Devices/Cardenza/cardenza/...`) in consumers.

## CMake Usage

As a subdirectory:

```cmake
add_subdirectory(path/to/cardenza)
target_link_libraries(your_target PRIVATE CardenzaHAL)
```

ESP-IDF consumers that compile the sources into their own component (as CardenzaSynth does) list the `.cpp` files directly and add this directory to `INCLUDE_DIRS`.

## ESP-IDF Only

Every driver source needs ESP-IDF. Configuring the CMake target outside an ESP-IDF build stops with an error unless `CARDENZA_HAL_ENABLE_EMBEDDED_DRIVERS` is forced on. Do not add `ESP_PLATFORM` guards or host stub `.cpp` files; desktop builds use a separate device layer instead (for example CardenzaSim).

## Keyboard

The keyboard API is positional: `updateKeyList()` / `keyList()` return pressed keys as `{x, y}` matrix coordinates (14 columns × 4 rows), plus the LED controls. There is no text or HID key mapping; consumers that need key labels define their own table.

## Audio Output

The I2S transport uses 16-bit PCM in 32-bit slots. Philips WS spans 32 BCLK cycles per slot (50% duty across the 64-cycle frame); changing `slot_bit_width` alone leaves the ESP-IDF helper's WS width at 16 and produces mismatched timing.

The default TX channel accepts mono PCM in the left slot. Stereo consumers must configure stereo slots and supply interleaved left/right samples. A mono synth that drives both headphone channels duplicates its samples at the consumer's output boundary; synthesis and mixing remain mono.

The TX channel runs at 44.1 kHz with a DMA queue of 3 × 128 frames (about 8.7 ms). A consumer that renders larger blocks must finish each block within the remaining queue time or the output underruns.
