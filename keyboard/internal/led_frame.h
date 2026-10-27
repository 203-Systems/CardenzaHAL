#pragma once

#include <algorithm>
#include <array>

#include "../types.h"

namespace CardenzaHAL::Keyboard::detail {

class LedFrameState
{
public:
    using ScanMasks = std::array<std::array<uint8_t, 8>, 4>;

    bool SetKey(Point2D_t key, uint8_t level)
    {
        if (key.x < 0 || key.y < 0 || key.x >= static_cast<int>(LED_COLUMNS) || key.y >= static_cast<int>(LED_ROWS))
            return false;
        auto& current = levels_[key.y * LED_COLUMNS + key.x];
        level = std::min(level, LED_BRIGHTNESS_MAX);
        if (current == level) return false;
        current = level;
        return true;
    }

    bool Assign(const LedFrame& frame)
    {
        bool changed = false;
        for (std::size_t index = 0; index < levels_.size(); ++index)
        {
            const uint8_t level = std::min(frame[index], LED_BRIGHTNESS_MAX);
            changed |= levels_[index] != level;
            levels_[index] = level;
        }
        return changed;
    }

    bool Fill(uint8_t level)
    {
        level = std::min(level, LED_BRIGHTNESS_MAX);
        bool changed = false;
        for (auto& current : levels_)
        {
            changed |= current != level;
            current = level;
        }
        return changed;
    }

    ScanMasks BuildScanMasks() const
    {
        ScanMasks masks{};
        for (std::size_t row = 0; row < 8; ++row)
        {
            const std::size_t y = row < 4 ? 3 - row : 7 - row;
            const std::size_t xBase = row < 4 ? 1 : 0;
            for (std::size_t column = 0; column < 7; ++column)
            {
                const uint8_t duty = levels_[y * LED_COLUMNS + xBase + column * 2];
                const auto bit = static_cast<uint8_t>(1u << (column + 1));
                for (std::size_t phase = 0; phase < masks.size(); ++phase)
                    if (duty & (1u << phase)) masks[phase][row] |= bit;
            }
        }
        return masks;
    }

private:
    LedFrame levels_{};
};

}  // namespace CardenzaHAL::Keyboard::detail
