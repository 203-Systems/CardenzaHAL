#pragma once

// A simulated clock for the kernel tests: time moves only when the test moves it, or by a microsecond
// a read when a test wants busy loops to take time (as CPU time would).
#include <cstdint>

#include "web/src/kernel/kernel.h"

namespace sim {

inline uint64_t g_now = 0;
inline bool g_ticking = false;

inline uint64_t Now()
{
    if (g_ticking) ++g_now;
    return g_now;
}

inline void Start()
{
    cardenza_web::kernel::SetClockForTest(Now);
}

// Runs the scheduler, jumping the clock from event to event, until `until` (microseconds).
inline void RunUntil(uint64_t until)
{
    namespace k = cardenza_web::kernel;
    for (int guard = 0; guard < 1000000; ++guard)
    {
        k::RunFor(1000000000ull);
        const int64_t delay = k::NextDelayUs();
        if (delay < 0 || g_now + static_cast<uint64_t>(delay) > until)
        {
            if (g_now < until) g_now = until;
            k::RunFor(1000000000ull);
            return;
        }
        g_now += static_cast<uint64_t>(delay);
    }
}

}  // namespace sim
