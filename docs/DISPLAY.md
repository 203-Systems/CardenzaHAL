# Fast, tear-free screens on the Cardenza

[简体中文](DISPLAY.zh-CN.md)

How to get the most out of the 240 × 135 screen with CardenzaHAL's presenter ([`display/presenter.h`](../display/presenter.h), [`display/display.h`](../display/display.h)): what it does for you, how to pace a loop, where to keep the framebuffer, what it costs, and what the panel can and cannot do about tearing. It is CardenzaSynth's screen path, made general, with the 203 Launcher's findings on this panel.

## The short version

```cpp
#include "cardenzaHAL.h"

namespace Display = CardenzaHAL::Display;

alignas(4) static uint16_t frame[Display::WIDTH * Display::HEIGHT];  // or Display::AllocateFramebuffer()

extern "C" void app_main()
{
    CardenzaHAL::Init();
    Display::FramePacer pacer;              // a tick every 10 ms, a frame every other tick: 50 fps
    for (;;)
    {
        Update();                           // keys, sound, state: every tick (10 ms of latency)
        if (pacer.DrawTick())
        {
            Draw(frame);                    // the whole picture, as a function of the time
            Display::PresentChanged(frame); // only what changed goes to the panel
        }
        pacer.Wait();
    }
}
```

Draw the whole picture every frame from your state and the time (animations as functions of time, no second framebuffer needed): `PresentChanged` finds what differs from the last frame and sends only that.

## What `PresentChanged` does

1. **Finds what changed.** Every row is hashed in segments of 30 pixels (a word at a time: 0.4 ms a frame on the board with the frame in internal RAM) and compared with the last frame. Each band of 8 rows gives one rectangle over its changed rows and the span of its changed segments; bands under each other with the same span join. Its table is 4.3 KB (PSRAM when there is some).
2. **Refreshes one band whole every frame**, a different one each time: the whole screen every 17 frames (0.34 s at 50 fps) for 3.8 KB a frame, so nothing a hash missed or the panel lost stays on screen.
3. **Sends in the panel's scan order.** The rectangles are cut into windows of at most 2,160 pixels and sent from left to right ([`display/scan_order.h`](../display/scan_order.h), below: tearing): a tall rectangle in bands of 16 columns, a short wide one as one window.
4. **Through two DMA blocks of internal RAM.** The framebuffer can be anywhere: the HAL copies each window into a block (the panel's DMA cannot read PSRAM) and copies the next into the other block while the first is on the wire. The task waiting for a block wakes as soon as the block is out: the HAL asks for the switch from the DMA interrupt, which ESP-IDF's SPI panel driver does not do.
5. **At 80 MHz** (`CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ`), the clock the 203 Launcher runs this panel at on every Cardenza.
6. **Stays out of the way of the system menu**: while it is open nothing is sent (`FrameStats::held`), and the first frame after it goes whole. The development channel's screenshot (`203cli board screen`) is taken from the framebuffer you present.

The first frame and the one after a failed send go whole. `Present(framebuffer)` sends a whole frame the same way; `Present(framebuffer, regions, count)` sends rectangles you know changed (use one of the two styles, not both: the other `Present` calls do not update `PresentChanged`'s table).

## Pacing a loop

`FramePacer` runs a loop at a steady tick and draws every few ticks (`PacerOptions`: `tickMs = 10`, `ticksPerFrame = 2`). Keys and sound control keep the tick's latency while the screen runs at 50 fps. A tick that runs late does not sleep (the next ones catch up); after three late ticks in a row the loop sleeps one RTOS tick anyway, so the tasks below it on its core (a card writer, the idle task and its watchdog) get to run: CardenzaSynth's UI once kept a core at 93% with nothing left for them and the watchdog restarted the board. `LateTicks()` and `Yields()` count both.

Periods are whole RTOS ticks. The board's defaults run FreeRTOS at 1000 Hz, where any millisecond works; at ESP-IDF's default of 100 Hz the shortest tick is 10 ms. The display itself no longer depends on the tick rate (before 2026-11-23 each band waited for the next tick: a screen took 17 ms at 1000 Hz and 170 ms at 100 Hz).

## Where the framebuffer lives

The panel's DMA never reads your framebuffer, so either RAM works:

| | PSRAM | Internal RAM |
| --- | --- | --- |
| Internal RAM it takes | none | 64.8 KB of about 300 KB |
| Finding what changed | reads 64.8 KB a frame through the cache: slower (measure `FrameStats::findUs`) | 0.4 ms a frame (CardenzaSynth) |
| Copying what goes out | about 40 MB/s (an estimate) | about 400 MB/s |

`AllocateFramebuffer(FrameMemory::Any)` gives a zeroed, 32-byte aligned frame in PSRAM when the board's heap has PSRAM, else in internal RAM (`Psram` or `Internal` to choose). A static `alignas(4)` array is internal RAM unless you place it in PSRAM (`EXT_RAM_BSS_ATTR` with `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`).

The HAL's own memory: one 4.3 KB DMA block (static), a second one from the internal heap at the first frame (`PresentOptions::pipeline`, on by default; without room, one block), the 4.3 KB hash table (PSRAM when there is some).

## What it costs, frame by frame

- `LastFrame()`: the last frame's `bytes`, `regions`, `windows`, `findUs` (hashing), `sendUs` (copying and sending until the last window is on the panel), `presentUs`, and whether it went `whole`, was `held` by the system menu, or failed.
- `Totals()` / `ResetTotals()`: frames, whole frames, held frames, failures, bytes, the sum and the maximum of the present time.
- `SetFrameCallback(callback, context)`: called after every framebuffer present, on the presenting task, outside the display's lock (it may call the display). For a log line now and then or an on-screen meter; keep it short.

The template ([`examples/template`](../examples/template)) logs a line a minute: `screen: 3000 frames, 1523 us a frame (max 26851), 3832 bytes a frame, 15 late ticks` (the web simulator's).

## Drawing while the last frame goes out

`DoubleBuffer` keeps two framebuffers (129.6 KB, PSRAM when there is some) and a task of the HAL's that presents one while you draw the next into the other: a frame then costs the longer of drawing and sending, not both. `Back()` is the frame to draw (it holds the frame before last: draw the whole picture), `Present()` hands it over and returns. It costs a task (4 KB of stack) and does nothing for tearing: the panel's own memory is what it shows.

## Tearing

The panel (ST7789) has no tearing-effect line wired to the chip, and the HAL does not read its scan line back (the board has no MISO line), so the HAL cannot know where the panel is in its refresh. About 60 times a second it scans its memory out to the glass, one of the screen's columns after another from x = 0 (in this landscape orientation the panel's rows are the screen's columns; the 203 Launcher measured it). Whatever is written while the scan passes over it can show for one refresh (16.7 ms) partly old and partly new.

What the presenter does about it:

- **Left to right, along the scan.** Whole rows written top to bottom cross the scan: every change that covered the screen met it somewhere, split along a diagonal. Column bands written left to right run along it: a refresh that meets the write splits on one straight vertical line, and less often.
- **Fast.** At 80 MHz a whole frame takes about 7.6 ms, well inside a refresh; at 40 MHz it took about as long as the scan, and the two ran side by side and tore in steps (the 203 Launcher's review of 2026-11-04).
- **Only what changed, in one burst.** A small change is on the panel in a fraction of a millisecond.

What cannot be fixed without a TE line: a change written at the moment the scan crosses its columns is split there for one refresh. The chance is about (its width in columns × 48 us + the time to write it) / 16.7 ms: a 24-pixel sprite about 17%, a bar across the whole width about 70% of the frames it moves in, whatever the order. So design motion for the panel:

- Move in small steps. A split shows the difference between two frames: a bar that slides 2-3 pixels a frame splits by 2-3 pixels, which is hard to see; a jump of a whole row is not.
- Change a page in one frame (`RequestWholeFrame()` when you know everything moved). It meets the scan in about 30% of the changes, for one refresh. A slide or wipe over several frames is a whole-screen change in every one of them.
- Keep what moves narrow when you can: the chance grows with the width of the change.
- Double buffering in RAM does not help (see above), and neither does drawing faster: the split comes from when the panel scans, not from your frame.

The numbers above are the model in [`tests/display_bench.cpp`](../tests/display_bench.cpp) (below) with the ST7789's default timing; the panel's real line time has not been measured.

## Tuning

`SetPresentOptions(PresentOptions)`; the next frame goes whole:

- `segmentPixels` (30): the hash segment's width, an even divisor of 240 with at most 32 segments a row (8 … 240). 16 lines the rectangles up with the scan bands for a 8.1 KB table.
- `refreshBands` (1): bands of 8 rows resent whole every frame (0 to 17). 0 sends nothing for an unchanged frame.
- `pipeline` (true): the second DMA block.

`CONFIG_CARDENZA_HAL_DISPLAY_SPI_MHZ` (80) is the bus clock (the chip makes 80, 40, 26.7 MHz …). Lower it only to rule the clock out on a board whose screen misbehaves.

## The numbers

`tests/display_bench.cpp` runs the HAL's own change finding and scan order over typical screens and models the board's SPI time (ESP-IDF's figures for an SPI transaction on the S3; the model gives the 17 ms and 170 ms measured before). Build the host tests (`cmake -S tests -B build-tests && cmake --build build-tests`) and run `build-tests/CardenzaDisplayBench`. Per frame, from a framebuffer in PSRAM, the refresh band included:

| Screen | Bytes | Before (40 MHz, bands waited for the tick) | Now (80 MHz, scan order, two blocks) | One block | Tear chance before / now |
| --- | --- | --- | --- | --- | --- |
| nothing moves | 3.8 KB | 1.5 ms | 0.54 ms | 0.54 ms | 0 / 0 |
| a 60 × 14 number changes | 5.0 KB | 2.4 ms | 0.73 ms | 0.76 ms | 21% / 19% |
| a 24 × 24 sprite moves 2 px a frame | 6.3 KB | 4.0 ms | 0.86 ms | 0.92 ms | 25% / 17% |
| a 240 × 18 bar slides 3 px a frame | 6.8 KB | 3.0 ms | 0.94 ms | 1.04 ms | 74% / 71% |
| the whole screen scrolls | 64.8 KB | 17.5 ms | 7.6 ms | 9.1 ms | 100% / 30% |

These are a model: `FrameStats` measures the real ones on a board.
