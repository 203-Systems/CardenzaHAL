#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace CardenzaHAL::Keyboard {

// A second opinion on keys whose wrong reading does the most damage (a firmware's modifier or
// navigation keys, say). Pure: usable on a computer too.
//
// The HAL debounces for 3 ms, which is right for a contact but not for a signal that is off for
// longer: a key that chatters would be a row of taps, and one that reads as held for no reason (a
// ghost of the key matrix) would hijack every other key. Here a key's state changes only once the
// reading has held for settleUs, and a key that reads as held for longer than longestHoldUs counts as
// released until it really lets go. Poll it with each key's reading (isKeyPressing) once a frame, from
// one task; Glitches and Stuck count what it filtered out, for a log.
template <std::size_t kKeys>
class KeySettle {
public:
    static constexpr int64_t kDefaultSettleUs = 12000;
    static constexpr int64_t kDefaultLongestHoldUs = 15000000;

    explicit KeySettle(int64_t settleUs = kDefaultSettleUs, int64_t longestHoldUs = kDefaultLongestHoldUs)
        : settleUs_(settleUs), longestHoldUs_(longestHoldUs)
    {
    }

    // The reading of key (0..kKeys-1) at nowUs (microseconds, e.g. esp_timer_get_time()); returns
    // whether the key counts as held.
    bool Update(std::size_t key, bool reading, int64_t nowUs)
    {
        if (key >= kKeys) return false;
        State& state = states_[key];
        if (state.ignored)
        {
            // Stays released until the reading has been off for the settle time.
            if (reading) state.offSince = nowUs;
            else if (nowUs - state.offSince >= settleUs_) state.ignored = false;
            return false;
        }
        if (reading == state.shown)
        {
            if (state.pending) ++glitches_;
            state.pending = false;
        }
        else if (!state.pending || reading != state.candidate)
        {
            if (state.pending) ++glitches_;
            state.pending = true;
            state.candidate = reading;
            state.since = nowUs;
        }
        else if (nowUs - state.since >= settleUs_)
        {
            state.pending = false;
            state.shown = reading;
            state.heldSince = nowUs;
        }
        if (state.shown && nowUs - state.heldSince >= longestHoldUs_)
        {
            state = State{};
            state.ignored = true;
            state.offSince = nowUs;
            ++stuck_;
        }
        return state.shown;
    }

    // Readings that changed and changed back before they settled.
    uint32_t Glitches() const { return glitches_; }
    // Keys let go because they read as held for longer than longestHoldUs.
    uint32_t Stuck() const { return stuck_; }
    int64_t SettleUs() const { return settleUs_; }
    int64_t LongestHoldUs() const { return longestHoldUs_; }

private:
    struct State {
        bool shown = false;
        bool pending = false;
        bool candidate = false;
        bool ignored = false;
        int64_t since = 0;
        int64_t heldSince = 0;
        int64_t offSince = 0;
    };
    int64_t settleUs_;
    int64_t longestHoldUs_;
    std::array<State, kKeys> states_{};
    uint32_t glitches_ = 0;
    uint32_t stuck_ = 0;
};

}  // namespace CardenzaHAL::Keyboard
