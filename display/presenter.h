#pragma once

// The frame presenter: fast screens with a few calls (docs/DISPLAY.md has the guide and the numbers).
//
//   alignas(4) static uint16_t frame[Display::WIDTH * Display::HEIGHT];   // or AllocateFramebuffer()
//   Display::FramePacer pacer;                     // a tick every 10 ms, a frame every other tick (50 fps)
//   for (;;) {
//       Update();                                  // keys, sound, state: every tick
//       if (pacer.DrawTick()) {
//           Draw(frame);                           // draw the whole picture, as a function of the time
//           Display::PresentChanged(frame);        // only what changed goes to the panel
//       }
//       pacer.Wait();
//   }
//
// PresentChanged (display/display.h) finds what changed by hashing every row in segments, sends it in the
// panel's scan order through the HAL's DMA blocks, and refreshes one band of the screen whole every frame.
// This header tunes it (PresentOptions), says what it cost (FrameStats, PresentTotals, a FrameCallback),
// paces a loop (FramePacer), allocates framebuffers (AllocateFramebuffer) and presents on a task of its own
// while the next frame is drawn (DoubleBuffer).

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "frame_pacing.h"
#include "region.h"

namespace CardenzaHAL::Display {

// ---- How PresentChanged works -------------------------------------------------------------------------

struct PresentOptions {
    // The width of the segments every row is hashed in (display/changed_regions.h ValidSegment: 8, 10, 12,
    // 16, 20, 24, 30, 40, 48, 60, 80, 120 or 240 pixels). Its table is HEIGHT * WIDTH / segmentPixels words
    // (30: 4.3 KB, 16: 8.1 KB), made at the next frame, in PSRAM when there is some.
    int segmentPixels = 30;
    // Bands of eight rows sent whole every frame, changed or not, a different one each time (0 to 17). One
    // refreshes the whole screen every 17 frames (0.34 s at 50 fps) for 3.8 KB a frame, so nothing a hash
    // missed or the panel lost stays on screen. 0: none.
    int refreshBands = 1;
    // Two DMA blocks: the next window is copied while the panel takes the one before. The second block (4.3 KB
    // of internal RAM) is taken from the heap at the next frame; without room, one block (copy, then send).
    bool pipeline = true;
};

// False (and nothing changes) for invalid options. The next PresentChanged sends the whole screen.
bool SetPresentOptions(const PresentOptions& options);
PresentOptions GetPresentOptions();

// The next framebuffer Present* sends the whole screen, in one pass from left to right as fast as the bus
// goes: after a new page, or when you know everything moved (one pass instead of the pieces that changed;
// PresentChanged still hashes the frame, for the frames after it).
void RequestWholeFrame();

// ---- What it costs ------------------------------------------------------------------------------------

// One framebuffer Present* (Present(framebuffer), Present(framebuffer, regions, count), PresentChanged).
struct FrameStats {
    uint32_t number = 0;     // frames presented since the start, this one included
    uint32_t bytes = 0;      // pixels sent to the panel, in bytes
    uint16_t regions = 0;    // rectangles that changed (or that were given)
    uint16_t windows = 0;    // panel windows they went in (one DMA transfer each)
    uint32_t findUs = 0;     // finding what changed (PresentChanged: hashing the frame)
    uint32_t sendUs = 0;     // copying and sending, until the last window was on the panel
    uint32_t presentUs = 0;  // the whole call
    bool whole = false;      // the whole screen went: the first frame, after a failure or the system menu, RequestWholeFrame
    bool held = false;       // the system menu holds the screen: nothing was sent (the frame after it goes whole)
    bool ok = false;         // what the call returned
};

struct PresentTotals {
    uint32_t frames = 0;
    uint32_t wholeFrames = 0;
    uint32_t heldFrames = 0;
    uint32_t failures = 0;
    uint64_t bytes = 0;
    uint64_t findUs = 0;     // sums: divide by frames for the average
    uint64_t presentUs = 0;
    uint32_t maxPresentUs = 0;
};

FrameStats LastFrame();
PresentTotals Totals();
void ResetTotals();

// Called after every framebuffer Present* (held ones and failures too) on the task that presented, outside
// the display's lock, so it may call the Display functions; keep it short. nullptr removes it.
using FrameCallback = void (*)(const FrameStats& frame, void* context);
void SetFrameCallback(FrameCallback callback, void* context = nullptr);

// ---- Framebuffers -------------------------------------------------------------------------------------

// Where a framebuffer lives. The panel's DMA never reads it (the HAL copies what goes out into its own
// internal blocks), so either RAM works:
// - Psram: costs no internal RAM (the scarce one: Wi-Fi, audio, DMA, task stacks). Hashing and copying it
//   read 64.8 KB a frame through the cache, slower than internal RAM: watch FrameStats::findUs.
// - Internal: fastest to draw, hash and copy (a frame's hashes took 0.4 ms in CardenzaSynth), for 64.8 KB of
//   the board's ~300 KB of internal heap.
// - Any: PSRAM when the board has some in its heap, else internal.
enum class FrameMemory : uint8_t { Any, Psram, Internal };

// WIDTH * HEIGHT pixels (64,800 bytes), zeroed, 16-byte aligned (PresentChanged needs 4). Null: no room.
uint16_t* AllocateFramebuffer(FrameMemory memory = FrameMemory::Any);
void FreeFramebuffer(uint16_t* frame);

// ---- Pacing ---------------------------------------------------------------------------------------------

struct PacerOptions {
    uint32_t tickMs = 10;          // how often the loop runs: keys, sound control, the app's state
    uint32_t ticksPerFrame = 2;    // a frame is drawn every this many ticks (2 at 10 ms: 50 fps)
    uint32_t yieldAfterLate = 3;   // late ticks in a row before the loop sleeps one RTOS tick anyway (0: never)
};

// A loop that ticks at a steady rate and draws every few ticks (display/frame_pacing.h has the rules).
// Construct it just before the loop; call Wait() at the end of every tick. Periods are whole RTOS ticks
// (at least one): with CONFIG_FREERTOS_HZ=1000 (the board's default) any number of milliseconds.
class FramePacer {
public:
    explicit FramePacer(const PacerOptions& options = {});

    uint32_t Tick() const { return pacing_.Tick(); }
    bool DrawTick() const { return pacing_.DrawTick(); }
    // Sleeps until the next tick is due. False: this tick ran late (no sleep); after yieldAfterLate of those
    // in a row it sleeps one RTOS tick anyway, so the tasks below this one on its core run, and starts the
    // schedule again from then.
    bool Wait();
    uint32_t LateTicks() const { return pacing_.LateTicks(); }
    uint32_t Yields() const { return pacing_.Yields(); }
    TickType_t Period() const { return period_; }

private:
    FramePacing pacing_;
    TickType_t period_;
    TickType_t lastWake_;
};

// ---- Double buffering -----------------------------------------------------------------------------------

// Two framebuffers and a task of the HAL's that presents one (PresentChanged) while you draw the next into
// the other, so a frame costs the longer of drawing and sending instead of both. It does not stop tearing:
// the panel's own memory is what it shows, and it has no TE line (docs/DISPLAY.md). One per firmware.
struct DoubleBufferOptions {
    FrameMemory memory = FrameMemory::Any;
    UBaseType_t priority = 0;           // 0: the priority of the task that calls Begin
    BaseType_t core = tskNO_AFFINITY;   // the presenting task's core
    uint32_t stackBytes = 4096;         // internal RAM; the FrameCallback runs on it too
};

class DoubleBuffer {
public:
    using Options = DoubleBufferOptions;

    DoubleBuffer() = default;
    ~DoubleBuffer() { End(); }
    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    // Two framebuffers (129.6 KB) and the task. False: no room (nothing kept).
    bool Begin(const Options& options = {});
    // Waits for the frame being sent, then gives everything back.
    void End();

    // The frame to draw now, never the one being sent. It holds the frame before the last one presented:
    // draw the whole picture (or copy what you keep).
    uint16_t* Back() const { return frames_[back_]; }
    // Hands Back() to the presenting task and returns at once (it waits only while the frame before is still
    // being sent); Back() is then the other frame. False: not begun.
    bool Present();
    // Until the frame handed over has been presented (or the timeout): what PresentChanged returned.
    bool Wait(TickType_t timeout = portMAX_DELAY);

private:
    static void Run(void* self);

    uint16_t* frames_[2] = {nullptr, nullptr};
    int back_ = 0;
    int front_ = 1;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t handed_ = nullptr;  // a frame is waiting for the task
    SemaphoreHandle_t idle_ = nullptr;    // the task has no frame (given while idle)
    bool result_ = true;
};

}  // namespace CardenzaHAL::Display
