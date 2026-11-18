// The system menu's decisions and drawing (system/menu/logic.h). Pure: built into the HAL and the host tests.
#include "system/menu/logic.h"

#include <algorithm>
#include <cstring>

#include "system/menu/launcher/lcd.h"
#include "system/menu/launcher/motion.h"

namespace CardenzaHAL::SystemMenu::detail {

namespace {

// ---- Keys and the list --------------------------------------------------------------------------------

void KeepInView(State& state)
{
    state.selected = std::clamp(state.selected, 0, kItemCount - 1);
    state.scroll = std::clamp(state.scroll, state.selected - (kVisibleRows - 1), state.selected);
    state.scroll = std::clamp(state.scroll, 0, std::max(0, kItemCount - kVisibleRows));
}

}  // namespace

Key KeyAt(int x, int y)
{
    for (const auto& k : kNavigationKeys)
        if (k.x == x && k.y == y) return k.key;
    return Key::None;
}

Outcome Apply(State& state, Key key)
{
    Outcome outcome;
    const State before = state;
    switch (key)
    {
        case Key::Up: --state.selected; break;
        case Key::Down: ++state.selected; break;
        case Key::Select:
            outcome.chosen = true;
            outcome.item = static_cast<Item>(std::clamp(state.selected, 0, kItemCount - 1));
            break;
        case Key::Back:
            outcome.chosen = true;
            outcome.item = Item::Continue;
            break;
        case Key::None: break;
    }
    KeepInView(state);
    outcome.changed = state.selected != before.selected || state.scroll != before.scroll;
    return outcome;
}

Key KeyRepeat::Update(Key held, uint32_t nowMs)
{
    if (held != held_)
    {
        held_ = held;
        since_ = last_ = nowMs;
        return held;
    }
    if (held != Key::Up && held != Key::Down) return Key::None;
    if (!launcher::uiKeyRepeatDue(nowMs - since_, nowMs - last_)) return Key::None;
    last_ = nowMs;
    return held;
}

int LanguageIndex(int launcherLang)
{
    switch (launcherLang)
    {
        case 1:
        case 2: return 1;  // Simplified Chinese (2 was Traditional Chinese until 2026-11)
        case 3: return 2;  // Japanese
        case 4: return 3;  // Indonesian
        case 5: return 4;  // Vietnamese
        default: return 0;
    }
}

float PullEdge(uint32_t heldMs)
{
    if (heldMs <= kPullDelayMs) return 0;
    const float p = std::min(1.f, float(heldMs - kPullDelayMs) / float(kHoldMs - kPullDelayMs));
    return kWidth * launcher::powerHoldFill(uint32_t(p * launcher::kPowerHoldMs));
}

uint16_t PanelGrey(uint8_t grey)
{
    const uint16_t c = launcher::Lcd::rgb(grey);
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

uint16_t PanelMix(uint16_t a, uint16_t b, int weight)
{
    if (weight <= 0) return a;
    if (weight >= 256) return b;
    const uint16_t ca = static_cast<uint16_t>((a >> 8) | (a << 8)), cb = static_cast<uint16_t>((b >> 8) | (b << 8));
    const auto mix = [&](int shift, int mask) {
        const int x = (ca >> shift) & mask, y = (cb >> shift) & mask;
        return ((x * (256 - weight) + y * weight + 128) >> 8) << shift;
    };
    const uint16_t c = static_cast<uint16_t>(mix(11, 31) | mix(5, 63) | mix(0, 31));
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

// ---- NVS ----------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kPageActive = 0xFFFFFFFEu;
constexpr uint32_t kPageFull = 0xFFFFFFFCu;
constexpr uint32_t kPageFreeing = 0xFFFFFFF8u;
constexpr std::size_t kEntryTable = 32;
constexpr std::size_t kFirstEntry = 64;
constexpr std::size_t kEntryBytes = 32;
constexpr int kEntries = 126;
constexpr uint8_t kEntryWritten = 2;
constexpr uint8_t kTypeU8 = 0x01;

uint32_t ReadU32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool Readable(const uint8_t* page)
{
    const uint32_t state = ReadU32(page);
    return state == kPageActive || state == kPageFull || state == kPageFreeing;
}

uint8_t EntryState(const uint8_t* page, int index)
{
    return (page[kEntryTable + index / 4] >> ((index % 4) * 2)) & 3;
}

bool KeyIs(const uint8_t* entry, const char* key)
{
    char stored[17] = {};
    std::memcpy(stored, entry + 8, 16);
    return std::strcmp(stored, key) == 0;
}

// Calls found(entry) for every written item that starts in this page; stops when it returns true.
template <typename Found>
bool EachItem(const uint8_t* page, Found found)
{
    if (!Readable(page)) return false;
    for (int i = 0; i < kEntries;)
    {
        if (EntryState(page, i) != kEntryWritten)
        {
            ++i;
            continue;
        }
        const uint8_t* entry = page + kFirstEntry + std::size_t(i) * kEntryBytes;
        if (found(entry)) return true;
        const int span = entry[2];
        i += span >= 1 && span <= kEntries - i ? span : 1;
    }
    return false;
}

}  // namespace

uint8_t NvsNamespaceIndex(const uint8_t* page, const char* name)
{
    uint8_t index = 0;
    EachItem(page, [&](const uint8_t* entry) {
        if (entry[0] != 0 || entry[1] != kTypeU8 || !KeyIs(entry, name)) return false;
        index = entry[24];
        return index != 0;
    });
    return index;
}

bool NvsFindU8(const uint8_t* page, uint8_t index, const char* key, uint8_t& value)
{
    if (index == 0) return false;
    return EachItem(page, [&](const uint8_t* entry) {
        if (entry[0] != index || entry[1] != kTypeU8 || !KeyIs(entry, key)) return false;
        value = entry[24];
        return true;
    });
}

}  // namespace CardenzaHAL::SystemMenu::detail
