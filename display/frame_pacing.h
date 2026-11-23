#pragma once

#include <cstdint>

namespace CardenzaHAL::Display {

// What a frame loop decides from tick to tick: which ticks draw, and when a loop that keeps running late
// gives the tasks below it a turn. Pure (no FreeRTOS): Display::FramePacer (display/presenter.h) runs it,
// host tests check it.
//
// A loop ticks at its own rate (keys, sound control, the app's state: CardenzaSynth every 10 ms) and draws
// every few ticks (every other one: 50 fps). A late tick does not sleep (FreeRTOS's xTaskDelayUntil), so a
// loop whose ticks keep running long never blocks, and the tasks of lower priority on its core (a card
// writer, the idle task and its watchdog) never run: after `yieldAfterLate` late ticks in a row the loop
// sleeps one RTOS tick anyway and starts its schedule again from then (CardenzaSynth, 2026-11-04: a restored
// project with 10 ms frames kept core 0 at 93% and the stress build's watchdog restarted the board).
class FramePacing {
public:
    // ticksPerFrame: a frame is drawn every this many ticks (at least 1). yieldAfterLate: late ticks in a row
    // before the loop gives way (0: never).
    explicit FramePacing(uint32_t ticksPerFrame = 2, uint32_t yieldAfterLate = 3)
        : ticksPerFrame_(ticksPerFrame == 0 ? 1 : ticksPerFrame), yieldAfterLate_(yieldAfterLate)
    {
    }

    // The tick about to run (0, 1, 2 ...), and whether it draws: ticks 0, ticksPerFrame, 2 * ticksPerFrame ...
    uint32_t Tick() const { return tick_; }
    bool DrawTick() const { return tick_ % ticksPerFrame_ == 0; }
    uint32_t TicksPerFrame() const { return ticksPerFrame_; }

    // This tick has ended; `onTime`: the wait for the next one slept (it did not run late). True: give way now
    // (sleep one RTOS tick and restart the schedule from the time after it).
    bool Ended(bool onTime)
    {
        ++tick_;
        if (onTime)
        {
            lateRun_ = 0;
            return false;
        }
        ++late_;
        if (yieldAfterLate_ == 0 || ++lateRun_ < yieldAfterLate_) return false;
        lateRun_ = 0;
        ++yields_;
        return true;
    }

    // Ticks that ran late, and times the loop gave way, since it started.
    uint32_t LateTicks() const { return late_; }
    uint32_t Yields() const { return yields_; }

private:
    uint32_t ticksPerFrame_;
    uint32_t yieldAfterLate_;
    uint32_t tick_ = 0;
    uint32_t lateRun_ = 0;
    uint32_t late_ = 0;
    uint32_t yields_ = 0;
};

}  // namespace CardenzaHAL::Display
