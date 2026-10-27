#pragma once

#include <array>
#include <cstdint>

#include "../types.h"

namespace CardenzaHAL::Keyboard::detail {

// Treat excessive sampled keys as interference. Keep confirmed keys and their
// releases; quarantine rejected presses until a debounced physical release.
class KeyCountGuard
{
public:
    using Rows = std::array<uint8_t, 8>;
    static constexpr uint32_t kReleaseDebounceUs = 3000;

    void Filter(const uint8_t (&raw)[8], uint32_t nowUs, uint8_t (&filtered)[8])
    {
        Rows observed{};
        for (std::size_t row = 0; row < observed.size(); ++row) observed[row] = raw[row] & 0x7f;
        const bool excessive = Count(observed) > KEY_PRESS_LIMIT;
        for (std::size_t row = 0; row < observed.size(); ++row)
        {
            const uint8_t released = blocked_[row] & ~observed[row];
            if (released != releaseCandidate_[row])
            {
                releaseCandidate_[row] = released;
                releasedSince_[row] = nowUs;
            }
            else if (released && static_cast<uint32_t>(nowUs - releasedSince_[row]) >= kReleaseDebounceUs)
            {
                blocked_[row] &= ~released;
                releaseCandidate_[row] = 0;
            }
            if (excessive) blocked_[row] |= observed[row] & ~accepted_[row];
            filtered[row] = observed[row] & ~blocked_[row];
        }
    }

    bool Publish(const Rows& debounced)
    {
        Rows next{};
        // Independent row debounce can briefly combine old releases with new
        // presses. Apply the same limit to the published snapshot as well.
        const bool excessive = Count(debounced) > KEY_PRESS_LIMIT;
        for (std::size_t row = 0; row < next.size(); ++row)
        {
            if (excessive) blocked_[row] |= debounced[row] & ~accepted_[row];
            next[row] = debounced[row] & ~blocked_[row] & 0x7f;
        }
        if (next == accepted_) return false;
        accepted_ = next;
        return true;
    }

    bool Pending(const Rows& debounced) const
    {
        for (std::size_t row = 0; row < accepted_.size(); ++row)
            if (releaseCandidate_[row] || (debounced[row] & blocked_[row])) return true;
        return false;
    }

    bool Active() const
    {
        for (const auto row : blocked_) if (row) return true;
        return false;
    }

    const Rows& Accepted() const { return accepted_; }

private:
    static std::size_t Count(const Rows& rows)
    {
        std::size_t count = 0;
        for (uint8_t bits : rows)
            while (bits) { bits &= bits - 1; ++count; }
        return count;
    }

    Rows accepted_{};
    Rows blocked_{};
    Rows releaseCandidate_{};
    std::array<uint32_t, 8> releasedSince_{};
};

}  // namespace CardenzaHAL::Keyboard::detail
