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

namespace {

// The Launcher's hold-to-act curve (powerHoldFill, a cubic in and out) over [0, 1], and its slope.
float InOut(float p)
{
    return launcher::powerHoldFill(uint32_t(std::clamp(p, 0.f, 1.f) * launcher::kPowerHoldMs + 0.5f));
}
float InOutSlope(float p)
{
    if (p <= 0 || p >= 1) return 0;
    const float q = p < .5f ? p : 1 - p;
    return 12 * q * q;
}

}  // namespace

float PullEdge(uint32_t heldMs)
{
    if (heldMs <= kPullDelayMs) return 0;
    if (heldMs <= kPullOutMs)
        return kPullOutPixels * InOut(float(heldMs - kPullDelayMs) / float(kPullOutMs - kPullDelayMs));
    if (heldMs >= kHoldMs) return kPullRestPixels;
    return kPullOutPixels - kPullBackPixels * InOut(float(heldMs - kPullOutMs) / float(kHoldMs - kPullOutMs));
}

float PullSpeed(uint32_t heldMs)
{
    if (heldMs <= kPullDelayMs || heldMs >= kHoldMs) return 0;
    if (heldMs <= kPullOutMs)
    {
        const float span = float(kPullOutMs - kPullDelayMs);
        return kPullOutPixels * InOutSlope(float(heldMs - kPullDelayMs) / span) / span;
    }
    const float span = float(kHoldMs - kPullOutMs);
    return -kPullBackPixels * InOutSlope(float(heldMs - kPullOutMs) / span) / span;
}

// The way back is the Launcher's ListSlide::Edge (launcher/motion.inc): a cubic from `from` at `speed` to 0,
// at rest after `ms`.
float Curtain::Back(uint32_t nowMs) const
{
    launcher::ListSlide::Edge edge{backFrom_, 0, backSpeed_, backSince_, backMs_};
    return backMs_ != 0 ? edge.at(nowMs) : 0.f;
}

float Curtain::BackSpeed(uint32_t nowMs) const
{
    launcher::ListSlide::Edge edge{backFrom_, 0, backSpeed_, backSince_, backMs_};
    return backMs_ != 0 ? edge.velocity(nowMs) : 0.f;
}

void Curtain::ReturnFrom(float edge, uint32_t nowMs)
{
    pulling_ = false;
    heldMs_ = 0;
    backFrom_ = std::clamp(edge, 0.f, float(kWidth));
    backSpeed_ = 0;
    backSince_ = nowMs;
    backMs_ = backFrom_ > 0 ? kReturnMs : 0;
}

float Curtain::Update(bool pulling, uint32_t heldMs, uint32_t nowMs)
{
    if (pulling_ && !pulling)
    {
        // Let go: back from where the edge was last drawn. The speed it has is kept when it is already toward
        // the left (never faster than the Launcher's ease-out would start); an edge still going out starts
        // back from rest (ListSlide::Edge::go's rule for an edge that turns round).
        const float from = std::clamp(PullEdge(heldMs_) + Back(nowMs), 0.f, float(kWidth));
        const float speed = PullSpeed(heldMs_) + BackSpeed(nowMs);
        ReturnFrom(from, nowMs);
        backSpeed_ = std::clamp(speed, -3 * from / float(kReturnMs), 0.f);
    }
    pulling_ = pulling;
    heldMs_ = pulling ? heldMs : 0;
    if (!pulling && nowMs - backSince_ >= backMs_) backMs_ = 0;  // at rest: nothing left (the clock may wrap)
    return std::clamp((pulling ? PullEdge(heldMs) : 0.f) + Back(nowMs), 0.f, float(kWidth));
}

uint16_t PanelGrey(uint8_t grey)
{
    return launcher::Lcd::rgb(grey);
}

uint16_t PanelMix(uint16_t a, uint16_t b, int weight)
{
    if (weight <= 0) return a;
    if (weight >= 256) return b;
    const auto mix = [&](int shift, int mask) {
        const int x = (a >> shift) & mask, y = (b >> shift) & mask;
        return ((x * (256 - weight) + y * weight + 128) >> 8) << shift;
    };
    return static_cast<uint16_t>(mix(11, 31) | mix(5, 63) | mix(0, 31));
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
