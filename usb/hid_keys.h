#pragma once

#include <cstdint>

// HID inputs' pure parts (usb/hid.h): what a character is on a US keyboard, and the keys held (six and the
// modifiers, as a boot keyboard report carries them). No ESP-IDF and no TinyUSB: usb/hid.cpp uses them on the
// board, tests/hid_keys_test.cpp on the host.
namespace CardenzaHAL::USB::HidKeys {

// Keyboard usages (HID usage page 0x07) this needs.
inline constexpr uint8_t kUsageA = 0x04;
inline constexpr uint8_t kUsage1 = 0x1E;
inline constexpr uint8_t kUsage0 = 0x27;
inline constexpr uint8_t kUsageEnter = 0x28;
inline constexpr uint8_t kUsageTab = 0x2B;
inline constexpr uint8_t kUsageSpace = 0x2C;
inline constexpr uint8_t kUsageModifierFirst = 0xE0;  // left ctrl
inline constexpr uint8_t kUsageModifierLast = 0xE7;   // right GUI
inline constexpr uint8_t kModifierLeftShift = 0x02;   // the bit of 0xE1 in the modifier byte

// A character as a key and whether shift is down with it. usage 0: no key types it.
struct Stroke {
    uint8_t usage = 0;
    bool shift = false;
};

// US layout. "\n" is Enter, "\t" Tab; "\r" types nothing (a "\r\n" is one Enter: usb/hid.cpp skips it); other
// control characters and anything past ASCII have no key.
inline constexpr Stroke FromAscii(char c)
{
    if (c >= 'a' && c <= 'z') return {static_cast<uint8_t>(kUsageA + (c - 'a')), false};
    if (c >= 'A' && c <= 'Z') return {static_cast<uint8_t>(kUsageA + (c - 'A')), true};
    if (c >= '1' && c <= '9') return {static_cast<uint8_t>(kUsage1 + (c - '1')), false};
    switch (c)
    {
        case '0': return {kUsage0, false};
        case '\n': return {kUsageEnter, false};
        case '\t': return {kUsageTab, false};
        case ' ': return {kUsageSpace, false};
        case '!': return {0x1E, true};
        case '@': return {0x1F, true};
        case '#': return {0x20, true};
        case '$': return {0x21, true};
        case '%': return {0x22, true};
        case '^': return {0x23, true};
        case '&': return {0x24, true};
        case '*': return {0x25, true};
        case '(': return {0x26, true};
        case ')': return {0x27, true};
        case '-': return {0x2D, false};
        case '_': return {0x2D, true};
        case '=': return {0x2E, false};
        case '+': return {0x2E, true};
        case '[': return {0x2F, false};
        case '{': return {0x2F, true};
        case ']': return {0x30, false};
        case '}': return {0x30, true};
        case '\\': return {0x31, false};
        case '|': return {0x31, true};
        case ';': return {0x33, false};
        case ':': return {0x33, true};
        case '\'': return {0x34, false};
        case '"': return {0x34, true};
        case '`': return {0x35, false};
        case '~': return {0x35, true};
        case ',': return {0x36, false};
        case '<': return {0x36, true};
        case '.': return {0x37, false};
        case '>': return {0x37, true};
        case '/': return {0x38, false};
        case '?': return {0x38, true};
        default: return {};
    }
}

// What a key types on a US layout (the other way: for tests and for a firmware that shows what it sends).
// 0: nothing printable.
inline constexpr char ToAscii(uint8_t usage, bool shift)
{
    for (int c = 0; c < 128; ++c)
    {
        const Stroke stroke = FromAscii(static_cast<char>(c));
        if (stroke.usage == usage && stroke.shift == shift && stroke.usage != 0) return static_cast<char>(c);
    }
    return 0;
}

inline constexpr bool IsModifier(uint8_t usage) { return usage >= kUsageModifierFirst && usage <= kUsageModifierLast; }

// The keys held: the modifier bits and up to six other keys, in the order they went down.
class KeySet {
  public:
    enum class Result : uint8_t {
        Changed,    // the set is different now
        Unchanged,  // already so (a key pressed twice, a key released that was not down)
        Full,       // a seventh key: not taken (the six held stay as they are)
        Invalid,    // usage 0 (no key) or a reserved one (0x01-0x03: errors, not keys)
    };

    Result Press(uint8_t usage)
    {
        if (usage < kUsageA) return Result::Invalid;
        if (IsModifier(usage))
        {
            const uint8_t bit = static_cast<uint8_t>(1u << (usage - kUsageModifierFirst));
            if (modifiers_ & bit) return Result::Unchanged;
            modifiers_ = static_cast<uint8_t>(modifiers_ | bit);
            return Result::Changed;
        }
        for (uint8_t key : keys_)
            if (key == usage) return Result::Unchanged;
        for (uint8_t& key : keys_)
            if (key == 0)
            {
                key = usage;
                return Result::Changed;
            }
        return Result::Full;
    }

    Result Release(uint8_t usage)
    {
        if (usage < kUsageA) return Result::Invalid;
        if (IsModifier(usage))
        {
            const uint8_t bit = static_cast<uint8_t>(1u << (usage - kUsageModifierFirst));
            if (!(modifiers_ & bit)) return Result::Unchanged;
            modifiers_ = static_cast<uint8_t>(modifiers_ & ~bit);
            return Result::Changed;
        }
        for (int index = 0; index < 6; ++index)
            if (keys_[index] == usage)
            {
                // The others keep their order.
                for (int next = index; next < 5; ++next) keys_[next] = keys_[next + 1];
                keys_[5] = 0;
                return Result::Changed;
            }
        return Result::Unchanged;
    }

    void Clear()
    {
        modifiers_ = 0;
        for (uint8_t& key : keys_) key = 0;
    }

    bool Empty() const { return modifiers_ == 0 && keys_[0] == 0; }
    uint8_t Modifiers() const { return modifiers_; }
    bool Held(uint8_t usage) const
    {
        if (IsModifier(usage)) return modifiers_ & (1u << (usage - kUsageModifierFirst));
        for (uint8_t key : keys_)
            if (key != 0 && key == usage) return true;
        return false;
    }

    // The boot keyboard report: modifiers, a reserved byte, six keys.
    void Report(uint8_t out[8]) const
    {
        out[0] = modifiers_;
        out[1] = 0;
        for (int index = 0; index < 6; ++index) out[2 + index] = keys_[index];
    }

  private:
    uint8_t modifiers_ = 0;
    uint8_t keys_[6] = {};
};

// A gamepad's hat (1 up, clockwise to 8 up-left; 0 centred) from four directions; opposite ones cancel.
inline constexpr uint8_t HatFrom(bool up, bool down, bool left, bool right)
{
    const int vertical = (up ? 1 : 0) - (down ? 1 : 0);
    const int horizontal = (right ? 1 : 0) - (left ? 1 : 0);
    if (vertical > 0) return horizontal > 0 ? 2 : horizontal < 0 ? 8 : 1;
    if (vertical < 0) return horizontal > 0 ? 4 : horizontal < 0 ? 6 : 5;
    return horizontal > 0 ? 3 : horizontal < 0 ? 7 : 0;
}

}  // namespace CardenzaHAL::USB::HidKeys
