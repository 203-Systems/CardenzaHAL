#pragma once

#include <array>
#include <cstdint>

#include "key_count_guard.h"

namespace CardenzaHAL::Keyboard::detail {

// Keep elapsed-time debounce independent of the LED bit-plane dwell times.
class KeyDebouncer
{
public:
    static constexpr uint32_t kDebounceUs = KeyCountGuard::kReleaseDebounceUs;

    bool Update(const uint8_t (&raw)[8], uint32_t nowUs)
    {
        uint8_t filtered[8];
        guard_.Filter(raw, nowUs, filtered);
        for (std::size_t row = 0; row < stable_.size(); ++row)
        {
            const uint8_t value = filtered[row];
            if (value == stable_[row])
            {
                candidate_[row] = value;
                since_[row] = nowUs;
            }
            else if (value != candidate_[row])
            {
                candidate_[row] = value;
                since_[row] = nowUs;
            }
            else if (static_cast<uint32_t>(nowUs - since_[row]) >= kDebounceUs)
            {
                stable_[row] = value;
                since_[row] = nowUs;
            }
        }
        return guard_.Publish(stable_);
    }

    bool Pending() const { return candidate_ != stable_ || guard_.Pending(stable_); }
    bool InterferenceActive() const { return guard_.Active(); }
    const std::array<uint8_t, 8>& Rows() const { return guard_.Accepted(); }

private:
    std::array<uint8_t, 8> stable_{};
    std::array<uint8_t, 8> candidate_{};
    std::array<uint32_t, 8> since_{};
    KeyCountGuard guard_;
};

}  // namespace CardenzaHAL::Keyboard::detail
