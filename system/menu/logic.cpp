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

int Bayer(int x, int y)
{
    static constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return kBayer[y & 3][x & 3];
}

namespace {

// A darkened level (0 to 103 or so) to the greys a step of 32 apart, by the dither.
uint8_t DitherGrey(int level, int x, int y)
{
    const int step = level >> 5, part = (level & 31) >> 1;  // part: 0 to 15 of the way to the next grey
    return static_cast<uint8_t>(std::min(255, (step + (part > Bayer(x, y) ? 1 : 0)) << 5));
}

}  // namespace

uint8_t BackdropGrey(uint16_t firmwarePixel, int x, int y)
{
    const int r5 = firmwarePixel >> 11, g6 = (firmwarePixel >> 5) & 63, b5 = firmwarePixel & 31;
    const int r = r5 << 3 | r5 >> 2, g = g6 << 2 | g6 >> 4, b = b5 << 3 | b5 >> 2;
    const int luma = (54 * r + 183 * g + 19 * b + 128) >> 8;
    return DitherGrey(luma * 2 / 5, x, y);
}

uint8_t BackdropField(int x, int y)
{
    return DitherGrey(20, x, y);
}

bool Dissolved(int x, int y, int step)
{
    return Bayer(x, y) * kDissolveSteps < step * 16;
}

uint8_t SceneGrey(int step, uint8_t menuGrey, const uint16_t* firmwarePixel, int x, int y)
{
    if (step >= kDissolveSteps || (step > 0 && Dissolved(x, y, step))) return menuGrey;
    return firmwarePixel != nullptr ? BackdropGrey(*firmwarePixel, x, y) : BackdropField(x, y);
}

uint16_t PanelGrey(uint8_t grey)
{
    const uint16_t five = grey >> 3;
    return static_cast<uint16_t>(five << 11 | (five << 1 | five >> 4) << 5 | five);
}

bool PanelNeutral(uint16_t pixel)
{
    const int red = pixel >> 11, green = (pixel >> 5) & 63, blue = pixel & 31;
    return red == blue && green == (red << 1 | red >> 4);
}

int LauncherBrightness(bool found, int32_t stored)
{
    // Its savedBrightness (src/settings.cpp): never dark by a stored 0, never under 10.
    if (!found || stored == 0) return 100;
    return int(std::clamp<int32_t>(stored, 10, 100));
}

int LauncherBacklightDuty(int percent, int full)
{
    // Its backlightDuty: 1023 * (1100 + 89 * p) / 10000 of 1023, so 20 % of the time at 10 and always on at
    // 100 (the backlight's driver needs a pulse long enough to rise).
    percent = std::clamp(percent, 0, 100);
    return percent == 0 ? 0 : (full * (1100 + 89 * percent) + 5000) / 10000;
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
constexpr uint8_t kTypeI32 = 0x14;

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

bool NvsFindI32(const uint8_t* page, uint8_t index, const char* key, int32_t& value)
{
    if (index == 0) return false;
    return EachItem(page, [&](const uint8_t* entry) {
        if (entry[0] != index || entry[1] != kTypeI32 || !KeyIs(entry, key)) return false;
        value = static_cast<int32_t>(ReadU32(entry + 24));
        return true;
    });
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
