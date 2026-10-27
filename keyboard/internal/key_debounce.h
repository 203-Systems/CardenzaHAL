#pragma once

#include <array>
#include <cstdint>

namespace CardenzaHAL::Keyboard::detail {

// Keep elapsed-time debounce independent of the LED bit-plane dwell times.
class KeyDebouncer
{
public:
    static constexpr uint32_t kDebounceUs = 3000;

    bool Update(const uint8_t (&raw)[8], uint32_t nowUs)
    {
        bool changed = false;
        for (std::size_t row = 0; row < stable_.size(); ++row)
        {
            const uint8_t value = raw[row];
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
                changed = true;
            }
        }
        return changed;
    }

    bool Pending() const { return candidate_ != stable_; }
    const std::array<uint8_t, 8>& Rows() const { return stable_; }

private:
    std::array<uint8_t, 8> stable_{};
    std::array<uint8_t, 8> candidate_{};
    std::array<uint32_t, 8> since_{};
};

}  // namespace CardenzaHAL::Keyboard::detail
