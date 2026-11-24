// mic/gain.h: a recording's gain, the firmware's (2026-11-23: not a board setting). The factor of a gain in
// decibels, held at its range, and samples times it: rounded, held at the int16 range, never wrapping, at the
// top of the range too (a 64-bit product: +36 dB times full scale does not fit 32 bits).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mic/gain.h"

using namespace CardenzaHAL::Mic;

int main()
{
    assert(kMinGainDb == -12 && kMaxGainDb == 36);
    assert(GainQ12(0) == 4096);
    assert(std::abs(GainQ12(6) - 8173) <= 1);    // 10^(6/20) = 1.995
    assert(std::abs(GainQ12(12) - 16306) <= 1);  // 3.981
    assert(std::abs(GainQ12(-12) - 1028) <= 1);
    assert(GainQ12(99) == GainQ12(kMaxGainDb) && GainQ12(-99) == GainQ12(kMinGainDb));

    const int16_t samples[] = {0, 100, -100, 20000, -20000, 32767, -32768};
    int16_t copy[7];
    std::memcpy(copy, samples, sizeof(copy));
    ApplyGainQ12(copy, 7, 4096);
    assert(std::memcmp(copy, samples, sizeof(copy)) == 0);  // 0 dB: untouched
    ApplyGainQ12(copy, 7, 8192);
    // Doubled, held at the int16 range: never wraps.
    assert(copy[0] == 0 && copy[1] == 200 && copy[2] == -200 && copy[3] == 32767 && copy[4] == -32768);
    assert(copy[5] == 32767 && copy[6] == -32768);
    std::memcpy(copy, samples, sizeof(copy));
    ApplyGainQ12(copy, 7, 2048);
    assert(copy[1] == 50 && copy[2] == -50 && copy[3] == 10000 && copy[6] == -16384);

    // +36 dB (about 63 times): full scale saturates instead of overflowing, small samples scale.
    std::memcpy(copy, samples, sizeof(copy));
    ApplyGainQ12(copy, 7, GainQ12(kMaxGainDb));
    assert(copy[0] == 0 && copy[1] == 6310 && copy[2] == -6310);  // +-6309.57, rounded
    assert(copy[3] == 32767 && copy[4] == -32768 && copy[5] == 32767 && copy[6] == -32768);

    ApplyGainQ12(nullptr, 7, 8192);  // nothing to do
    std::puts("mic gain: ok");
    return 0;
}
