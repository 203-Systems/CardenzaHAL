// display/frame_pacing.h: which ticks draw, and when a loop that keeps running late gives way.
#include "display/frame_pacing.h"

#include <cassert>
#include <iostream>

using CardenzaHAL::Display::FramePacing;

int main()
{
    // CardenzaSynth's loop: a tick every 10 ms, a frame every other tick (50 fps), three late ticks in a row
    // before it gives way.
    {
        FramePacing pacing(2, 3);
        assert(pacing.Tick() == 0 && pacing.DrawTick());
        assert(!pacing.Ended(true) && pacing.Tick() == 1 && !pacing.DrawTick());
        assert(!pacing.Ended(true) && pacing.DrawTick());
        // Late, late, on time: the run starts again, nothing given.
        assert(!pacing.Ended(false) && !pacing.Ended(false) && !pacing.Ended(true));
        assert(pacing.LateTicks() == 2 && pacing.Yields() == 0);
        // Three late in a row: give way, and the count starts again.
        assert(!pacing.Ended(false) && !pacing.Ended(false) && pacing.Ended(false));
        assert(!pacing.Ended(false) && !pacing.Ended(false) && pacing.Ended(false));
        assert(pacing.LateTicks() == 8 && pacing.Yields() == 2);
        assert(pacing.Tick() == 11);
    }
    // Every tick draws; never gives way.
    {
        FramePacing pacing(1, 0);
        for (int i = 0; i < 10; ++i)
        {
            assert(pacing.DrawTick());
            assert(!pacing.Ended(false));
        }
        assert(pacing.LateTicks() == 10 && pacing.Yields() == 0);
    }
    // Zero ticks a frame is one.
    {
        FramePacing pacing(0, 1);
        assert(pacing.TicksPerFrame() == 1 && pacing.Ended(false));
    }
    // A frame every third tick: ticks 0, 3, 6.
    {
        FramePacing pacing(3, 3);
        int draws = 0;
        for (int i = 0; i < 9; ++i)
        {
            draws += pacing.DrawTick() ? 1 : 0;
            pacing.Ended(true);
        }
        assert(draws == 3);
    }
    std::cout << "frame pacing ok\n";
    return 0;
}
