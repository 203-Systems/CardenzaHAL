#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// A recording's gain. Pure (no ESP-IDF): host tests and simulators use it too. The chip's PDM receiver has no
// gain of its own, so a firmware that records multiplies what it reads (Mic::ApplyGain does it with these).
// The gain is the firmware's, not the board's (the user, 2026-11-23): the firmware keeps it with its own
// settings and passes it in.
namespace CardenzaHAL::Mic {

    inline constexpr int kMinGainDb = -12;
    inline constexpr int kMaxGainDb = 36;
    // Suggested initial setting for voice recording on Cardenza.
    inline constexpr int kRecommendedVoiceGainDb = 9;

    // The gain of `db` (held at kMinGainDb..kMaxGainDb) as a factor in 1/4096 (Q12): 4096 at 0 dB.
    inline int32_t GainQ12(int db)
    {
        db = std::clamp(db, kMinGainDb, kMaxGainDb);
        return static_cast<int32_t>(std::lround(4096.0 * std::pow(10.0, db / 20.0)));
    }

    // Each sample times gainQ12 / 4096, rounded, held at the int16 range (saturates, never wraps). The product
    // is 64-bit: above about +24 dB a full-scale sample times the factor no longer fits 32 bits.
    inline void ApplyGainQ12(int16_t* samples, std::size_t count, int32_t gainQ12)
    {
        if (samples == nullptr || gainQ12 == 4096) return;
        for (std::size_t i = 0; i < count; ++i)
        {
            const int64_t scaled = (static_cast<int64_t>(samples[i]) * gainQ12 + 2048) >> 12;
            samples[i] = static_cast<int16_t>(std::clamp<int64_t>(scaled, INT16_MIN, INT16_MAX));
        }
    }

}  // namespace CardenzaHAL::Mic
