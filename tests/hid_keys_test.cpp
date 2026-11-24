// usb/hid_keys.h: characters as keys on a US keyboard, the keys held (six and the modifiers), the gamepad's hat.
#include "usb/hid_keys.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <utility>

using namespace CardenzaHAL::USB::HidKeys;

namespace {

void AsciiTests()
{
    // Every printable character has a key, and the key types it back; no two share a key and shift.
    std::set<std::pair<int, bool>> seen;
    for (int c = 0x20; c < 0x7F; ++c)
    {
        const Stroke stroke = FromAscii(static_cast<char>(c));
        assert(stroke.usage != 0);
        assert(stroke.usage >= kUsageA && !IsModifier(stroke.usage));
        assert(ToAscii(stroke.usage, stroke.shift) == c);
        assert(seen.insert({stroke.usage, stroke.shift}).second);
    }
    // Upper case is the lower case key with shift.
    for (char c = 'a'; c <= 'z'; ++c)
    {
        const Stroke lower = FromAscii(c);
        const Stroke upper = FromAscii(static_cast<char>(c - 'a' + 'A'));
        assert(lower.usage == upper.usage && !lower.shift && upper.shift);
    }
    // Control characters: Enter, Tab; the rest have no key. "\r" too (Type skips it: "\r\n" is one Enter).
    assert(FromAscii('\n').usage == kUsageEnter && !FromAscii('\n').shift);
    assert(FromAscii('\t').usage == kUsageTab);
    assert(FromAscii('\r').usage == 0);
    assert(FromAscii('\0').usage == 0);
    assert(FromAscii(0x7F).usage == 0);
    assert(FromAscii(static_cast<char>(0xC3)).usage == 0);  // UTF-8's first byte of é
    // The number row and its shifted symbols share keys.
    assert(FromAscii('1').usage == FromAscii('!').usage && FromAscii('0').usage == FromAscii(')').usage);
    assert(FromAscii('2').usage == 0x1F && FromAscii('@').shift);
    assert(FromAscii(' ').usage == kUsageSpace && !FromAscii(' ').shift);
}

void KeySetTests()
{
    KeySet keys;
    uint8_t report[8];
    keys.Report(report);
    const uint8_t empty[8] = {};
    assert(std::memcmp(report, empty, 8) == 0 && keys.Empty());

    // Six keys at most; the seventh is not taken and the six stay.
    for (uint8_t usage = kUsageA; usage < kUsageA + 6; ++usage) assert(keys.Press(usage) == KeySet::Result::Changed);
    assert(keys.Press(kUsageA + 6) == KeySet::Result::Full);
    keys.Report(report);
    for (int index = 0; index < 6; ++index) assert(report[2 + index] == kUsageA + index);
    assert(!keys.Held(kUsageA + 6));
    // A key pressed twice changes nothing; a modifier still goes in beside six keys.
    assert(keys.Press(kUsageA) == KeySet::Result::Unchanged);
    assert(keys.Press(0xE1) == KeySet::Result::Changed);
    keys.Report(report);
    assert(report[0] == kModifierLeftShift && report[1] == 0);
    assert(keys.Press(0xE1) == KeySet::Result::Unchanged);
    // Released keys leave no gap; the others keep their order.
    assert(keys.Release(kUsageA + 2) == KeySet::Result::Changed);
    keys.Report(report);
    const uint8_t order[6] = {kUsageA, kUsageA + 1, kUsageA + 3, kUsageA + 4, kUsageA + 5, 0};
    assert(std::memcmp(report + 2, order, 6) == 0);
    // ... so a seventh fits again.
    assert(keys.Press(kUsageA + 6) == KeySet::Result::Changed);
    // Releasing what is not held changes nothing.
    assert(keys.Release(kUsageA + 20) == KeySet::Result::Unchanged);
    assert(keys.Release(0xE7) == KeySet::Result::Unchanged);
    // Every modifier is its own bit.
    KeySet modifiers;
    for (uint8_t usage = kUsageModifierFirst; usage <= kUsageModifierLast; ++usage) assert(modifiers.Press(usage) == KeySet::Result::Changed);
    modifiers.Report(report);
    assert(report[0] == 0xFF && report[2] == 0);
    assert(modifiers.Release(0xE0) == KeySet::Result::Changed && modifiers.Modifiers() == 0xFE);
    // No key and the error usages are not keys.
    for (uint8_t usage = 0; usage < kUsageA; ++usage)
    {
        assert(keys.Press(usage) == KeySet::Result::Invalid);
        assert(keys.Release(usage) == KeySet::Result::Invalid);
    }
    // Clear lets everything go.
    keys.Clear();
    keys.Report(report);
    assert(std::memcmp(report, empty, 8) == 0 && keys.Empty());
}

void HatTests()
{
    assert(HatFrom(false, false, false, false) == 0);
    assert(HatFrom(true, false, false, false) == 1);
    assert(HatFrom(true, false, false, true) == 2);
    assert(HatFrom(false, false, false, true) == 3);
    assert(HatFrom(false, true, false, true) == 4);
    assert(HatFrom(false, true, false, false) == 5);
    assert(HatFrom(false, true, true, false) == 6);
    assert(HatFrom(false, false, true, false) == 7);
    assert(HatFrom(true, false, true, false) == 8);
    // Opposites cancel.
    assert(HatFrom(true, true, false, false) == 0);
    assert(HatFrom(false, false, true, true) == 0);
    assert(HatFrom(true, true, true, false) == 7);
    assert(HatFrom(true, true, true, true) == 0);
}

}  // namespace

int main()
{
    AsciiTests();
    KeySetTests();
    HatTests();
    std::cout << "hid keys tests passed\n";
    return 0;
}
