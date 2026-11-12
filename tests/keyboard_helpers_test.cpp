#include "keyboard/key_settle.h"
#include "keyboard/led_light.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

void KeySettleTests()
{
    using Settle = CardenzaHAL::Keyboard::KeySettle<4>;
    constexpr int64_t kFrame = 16000;
    // A press and a release each count one frame later, a real tap gets through.
    {
        Settle settle;
        int64_t now = 0;
        assert(!settle.Update(1, true, now));
        assert(settle.Update(1, true, now += kFrame));
        assert(settle.Update(1, false, now += kFrame));
        assert(!settle.Update(1, false, now += kFrame));
        assert(settle.Glitches() == 0 && settle.Stuck() == 0);
    }
    // A one-frame spike never shows, and neither does a dip while held.
    {
        Settle settle;
        int64_t now = 0;
        assert(!settle.Update(1, true, now));
        assert(!settle.Update(1, false, now += kFrame));
        assert(settle.Glitches() == 1);
        assert(!settle.Update(1, true, now += kFrame));
        assert(settle.Update(1, true, now += kFrame));
        assert(settle.Update(1, false, now += kFrame));
        assert(settle.Update(1, true, now += kFrame));
        assert(settle.Update(1, true, now += kFrame));
        assert(settle.Glitches() == 2);
    }
    // A key that chatters frame by frame: no row of taps, whichever state it started in.
    {
        Settle settle;
        int64_t now = 0;
        for (int frame = 0; frame < 400; ++frame) assert(!settle.Update(2, frame % 2 == 0, now += kFrame));
        assert(settle.Glitches() > 100);
    }
    // Reads as held for ever: released after the longest hold, and it stays so until a real release.
    {
        Settle settle;
        int64_t now = 0;
        bool shown = false;
        for (int64_t elapsed = 0; elapsed < Settle::kDefaultLongestHoldUs + kFrame * 4; elapsed += kFrame) shown = settle.Update(3, true, now += kFrame);
        assert(!shown && settle.Stuck() == 1);
        for (int frame = 0; frame < 100; ++frame) assert(!settle.Update(3, true, now += kFrame));
        assert(!settle.Update(3, false, now += kFrame));
        assert(!settle.Update(3, false, now += kFrame));
        assert(!settle.Update(3, true, now += kFrame));
        assert(settle.Update(3, true, now += kFrame));
        assert(settle.Stuck() == 1);
        // The other keys are not affected.
        assert(!settle.Update(0, true, now));
        assert(settle.Update(0, true, now += kFrame));
    }
    // Times of its own: a shorter settle lets a press through sooner, a short longest hold lets go early.
    {
        Settle settle(4000, 100000);
        int64_t now = 0;
        assert(!settle.Update(0, true, now));
        assert(settle.Update(0, true, now += 5000));
        for (int frame = 0; frame < 10; ++frame) settle.Update(0, true, now += kFrame);
        assert(!settle.Update(0, true, now) && settle.Stuck() == 1);
        assert(settle.SettleUs() == 4000 && settle.LongestHoldUs() == 100000);
    }
    // A key outside the range is never held.
    {
        Settle settle;
        assert(!settle.Update(4, true, 0) && !settle.Update(4, true, kFrame));
    }
}

void LightTests()
{
    using namespace CardenzaHAL::Keyboard::Light;
    // Perceived levels go through the gamma table to the linear duty; a lit level never goes dark, the
    // table only climbs and keeps both ends.
    static_assert(PerceivedToLevel(0) == 0 && PerceivedToLevel(15) == 15 && PerceivedToLevel(255) == 15);
    for (unsigned level = 1; level <= 15; ++level)
        assert(PerceivedToLevel(level) >= 1 && PerceivedToLevel(level) >= PerceivedToLevel(level - 1));
    // The table is the gamma: round(15 * (p / 15) ^ 2.6), at least 1 for a lit level.
    for (unsigned p = 1; p <= 15; ++p)
    {
        const long expected = std::lround(15.0 * std::pow(p / 15.0, static_cast<double>(kPerceivedGamma)));
        assert(PerceivedToLevel(p) == (expected < 1 ? 1 : expected));
    }
    // The light: duty of the mapped level times the global level (gamma 2).
    for (unsigned global = 0; global <= 15; ++global)
    {
        unsigned lastLight = 0;
        for (unsigned level = 0; level <= 15; ++level)
        {
            const unsigned light = LightPermille(level, global);
            assert(light >= lastLight);
            lastLight = light;
            if (level == 0 || global == 0) assert(light == 0);
            else assert(LevelDuty(PerceivedToLevel(level)) * GlobalDuty(global) > 0.0f);
        }
    }
    assert(LightPermille(15, 15) == 1000);
    assert(LightPermille(6, 15) == 67);   // perceived 6: the dimmest duty, 1/15
    assert(LightPermille(15, 8) == 284);  // the global level dims every key, with gamma 2
}

}  // namespace

int main()
{
    KeySettleTests();
    LightTests();
    std::cout << "keyboard helpers ok\n";
    return 0;
}
