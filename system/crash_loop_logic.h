#pragma once

#include <cstdint>

// A firmware that crashes right after it starts, again and again, goes back to the 203 Launcher's home screen
// instead (Kconfig CONFIG_CARDENZA_HAL_CRASH_LOOP_LAUNCHER): the person never needs the BOOT key for it. Pure
// (no ESP-IDF): system/crash_loop.cpp keeps the count in RTC memory, tests/crash_loop_test.cpp checks it.
namespace CardenzaHAL::System::CrashLoop {

    inline constexpr uint32_t kMagic = 0x32304352u;  // "203C"

    // Kept in RTC memory across restarts (not across a power cycle).
    struct State {
        uint32_t magic;
        uint32_t crashes;  // early crashes in a row before this start
    };

    // Why the chip restarted, as far as this needs it.
    enum class Reset : uint8_t { PowerOn, Crash, Other };

    // At a start: the count of early crashes in a row, this start's reason included. A crash counts only when
    // the run before it did not last (the timer that clears the count did not fire, so the count is still
    // there); anything but a crash starts again from 0.
    inline uint32_t Count(const State& state, Reset reset)
    {
        if (reset != Reset::Crash || state.magic != kMagic) return reset == Reset::Crash ? 1 : 0;
        return state.crashes + 1;
    }

    // True: do not run the firmware again, go to the Launcher.
    inline bool GiveUp(uint32_t crashes, uint32_t limit) { return limit != 0 && crashes >= limit; }

}  // namespace CardenzaHAL::System::CrashLoop
