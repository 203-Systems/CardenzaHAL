// system/menu/logic.h and frame.h: the system menu's long press, curtain, keys, list, words, its frames (the
// Launcher's own, pixel for pixel) and fades, and the NVS reader.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include <algorithm>
#include <cmath>

#include "system/menu/frame.h"
#include "system/menu/launcher/i18n.h"
#include "system/menu/launcher/motion.h"
#include "system/menu/logic.h"

using namespace CardenzaHAL::SystemMenu::detail;
using CardenzaHAL::SystemMenu::launcher::ListFade;
namespace launcher = CardenzaHAL::SystemMenu::launcher;

uint32_t CardenzaHAL::SystemMenu::launcher::launcherMillis() { return 0; }

#include "system_menu_frames.inc"

namespace {

void TestHold()
{
    HoldDetector hold;
    hold.Reset(false);
    assert(!hold.Update(true, 0));
    assert(!hold.Update(true, kHoldMs - 1));
    assert(hold.Update(true, kHoldMs));
    assert(!hold.Update(true, kHoldMs + 500));  // once per press
    assert(!hold.Update(false, kHoldMs + 600));
    assert(!hold.Update(true, 5000));
    assert(hold.Update(true, 5000 + kHoldMs));

    // Down when watching began (the firmware started with HOME held): no menu until it was let go.
    hold.Reset(true);
    assert(!hold.Update(true, 0));
    assert(!hold.Update(true, 10 * kHoldMs));
    assert(!hold.Update(false, 10 * kHoldMs + 1));
    assert(!hold.Update(true, 20000));
    assert(hold.Update(true, 20000 + kHoldMs));

    // Time wraps (a uint32_t of milliseconds, after 49 days).
    hold.Reset(false);
    assert(!hold.Update(true, 0xFFFFFF00u));
    assert(hold.Update(true, 0xFFFFFF00u + kHoldMs));
}

void TestKeys()
{
    assert(KeyAt(11, 2) == Key::Up);      // ;
    assert(KeyAt(11, 3) == Key::Down);    // .
    assert(KeyAt(10, 3) == Key::Up);      // ,
    assert(KeyAt(12, 3) == Key::Down);    // /
    assert(KeyAt(13, 2) == Key::Select);  // ENTER
    assert(KeyAt(0, 0) == Key::Back);     // ESC
    assert(KeyAt(13, 0) == Key::Back);    // del
    assert(KeyAt(5, 1) == Key::None);
    for (const auto& k : kNavigationKeys) assert(k.x >= 0 && k.x < 14 && k.y >= 0 && k.y < 4);

    KeyRepeat repeat;
    assert(repeat.Update(Key::Down, 0) == Key::Down);
    assert(repeat.Update(Key::Down, launcher::kKeyRepeatDelayMs - 1) == Key::None);
    assert(repeat.Update(Key::Down, launcher::kKeyRepeatDelayMs) == Key::Down);
    assert(repeat.Update(Key::Down, launcher::kKeyRepeatDelayMs + launcher::kKeyRepeatMs - 1) == Key::None);
    assert(repeat.Update(Key::Down, launcher::kKeyRepeatDelayMs + launcher::kKeyRepeatMs) == Key::Down);
    assert(repeat.Update(Key::None, 2000) == Key::None);
    // Select and Back never repeat: a held ENTER chooses once.
    assert(repeat.Update(Key::Select, 3000) == Key::Select);
    assert(repeat.Update(Key::Select, 3000 + 5 * launcher::kKeyRepeatDelayMs) == Key::None);
    assert(repeat.Update(Key::None, 9000) == Key::None);
    assert(repeat.Update(Key::Back, 9001) == Key::Back);
}

void TestList()
{
    State state;
    Outcome o = Apply(state, Key::Up);  // at the top: stays
    assert(!o.chosen && !o.changed && state.selected == 0);
    for (int i = 1; i < kItemCount; ++i)
    {
        o = Apply(state, Key::Down);
        assert(o.changed && state.selected == i);
        assert(state.selected >= state.scroll && state.selected < state.scroll + kVisibleRows);
    }
    o = Apply(state, Key::Down);  // at the bottom: stays (no wrap, as the Launcher)
    assert(!o.changed && state.selected == kItemCount - 1);
    assert(state.scroll == kItemCount - kVisibleRows);
    o = Apply(state, Key::Select);
    assert(o.chosen && o.item == Item::Download);
    o = Apply(state, Key::Back);
    assert(o.chosen && o.item == Item::Continue);
    state = State{};
    o = Apply(state, Key::Select);
    assert(o.chosen && o.item == Item::Continue);
    state.selected = 1;
    o = Apply(state, Key::Select);
    assert(o.item == Item::Launcher);
}

// Every word has its glyphs in the Launcher's tables as the menu carries them.
void TestWords()
{
    for (int language = 0; language < kLanguageCount; ++language)
        for (int id = 0; id < kTextCount; ++id)
        {
            const char* text = kTexts[language][id];
            assert(text != nullptr && text[0] != 0);
            if (!launcher::textDrawable(text))
            {
                std::fprintf(stderr, "no glyph in language %d, text %d: %s\n", language, id, text);
                assert(false);
            }
        }
    launcher::i18nSetLanguage(launcher::Language::English);
    assert(launcher::textWidth("Restart", 1) == 7 * 6);
    launcher::i18nSetLanguage(launcher::Language::SimplifiedChinese);
    assert(launcher::textWidth("\xe5\x85\xb3\xe6\x9c\xba", 1) == 24);  // 关机: two 12 px glyphs
    assert(launcher::textWidth("USB", 1) == 18);
}

uint64_t Fnv(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes) hash = (hash ^ b) * 0x100000001b3ull;
    return hash;
}

std::vector<uint8_t> Frame(const View& view, ListFade* fade = nullptr, uint32_t now = 0, int band = kHeight)
{
    std::vector<uint8_t> canvas(kWidth * kHeight, 0x55);
    for (int y = 0; y < kHeight; y += band)
        PaintGrey(view, fade, now, y, std::min(band, kHeight - y), canvas.data() + y * kWidth);
    return canvas;
}

// The frames are the Launcher's own (tools/launcher_ui.py --compare draws them with its menuFrame and
// status() and keeps their hashes), however they are cut into bands.
void TestFrames()
{
    for (int language = 0; language < kLanguageCount; ++language)
        for (int selected = 0; selected < kItemCount; ++selected)
        {
            View view;
            view.language = language;
            view.state.selected = selected;
            view.state.scroll = selected < kVisibleRows ? 0 : selected - kVisibleRows + 1;
            view.firmware = "CardenzaSynthFirmware 0.2.1";
            const std::vector<uint8_t> whole = Frame(view);
            if (Fnv(whole) != kLauncherFrameHashes[language * kItemCount + selected])
            {
                std::fprintf(stderr, "language %d, row %d: not the Launcher's frame\n", language, selected);
                assert(false);
            }
            assert(Frame(view, nullptr, 0, 8) == whole);
            assert(Frame(view, nullptr, 0, 5) == whole);
        }
    // The release prompt replaces the chosen row's description; Back held dims the rail's arrow.
    View view;
    view.state.selected = 2;
    View prompt = view;
    prompt.releasePrompt = true;
    assert(Frame(prompt) != Frame(view));
    View back = view;
    back.backHeld = true;
    const std::vector<uint8_t> a = Frame(view), b = Frame(back);
    assert(a != b);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 24; x < kWidth; ++x) assert(a[y * kWidth + x] == b[y * kWidth + x]);  // only the rail
}

// The Launcher's ListFade: the selection crossfades from row to row, a scroll fades through; at the end
// the frame is the still one.
void TestListFade()
{
    View from;
    from.state.selected = 1;
    View to = from;
    to.state.selected = 2;
    ListFade fade;
    fade.reset(1, 0);
    assert(Frame(from, &fade, 1000) == Frame(from));
    fade.select(2, 1000);
    const std::vector<uint8_t> middle = Frame(to, &fade, 1000 + ListFade::kFadeMs / 2);
    assert(middle != Frame(from) && middle != Frame(to));
    assert(Frame(to, &fade, 1000 + ListFade::kFadeMs) == Frame(to));
    assert(!fade.active(1000 + ListFade::kFadeMs + 1));
    // Down past the fourth row: the rows fade out, the new ones in.
    View last = to;
    last.state.selected = 4;
    last.state.scroll = 1;
    fade.reset(3, 0);
    fade.select(4, 2000);
    fade.scrollTo(1, 2000);
    assert(fade.active(2000));
    const std::vector<uint8_t> out = Frame(last, &fade, 2000 + ListFade::kScrollMs / 2);
    assert(out != Frame(last));
    assert(Frame(last, &fade, 2000 + 2 * ListFade::kScrollMs + ListFade::kFadeMs) == Frame(last));
}

// HOME held: the curtain comes out after a tap's length, past where it stays, and settles on the rail and
// the rows' start when the hold is complete; let go, it goes back in the Launcher's 300 ms.
void TestCurtain()
{
    assert(PullEdge(0) == 0 && PullEdge(kPullDelayMs) == 0);
    float previous = 0, peak = 0;
    uint32_t peakAt = 0;
    for (uint32_t ms = kPullDelayMs; ms <= kHoldMs; ms += 10)
    {
        const float edge = PullEdge(ms);
        assert(edge >= 0 && edge <= kPullPeak + 0.01f);
        if (edge > peak) peak = edge, peakAt = ms;
        if (ms < peakAt + 1) assert(edge >= previous - 0.01f);  // out, then...
        previous = edge;
    }
    assert(peak > kPullPeak - 0.5f);
    assert(peakAt > kPullDelayMs && peakAt < kHoldMs);
    assert(std::fabs(PullEdge(kHoldMs) - kClothWidth) < 0.01f);  // ...back to where it stays
    assert(std::fabs(PullEdge(kHoldMs + 500) - kClothWidth) < 0.01f);
    assert(ClothOffset(kClothWidth) == 0);
    assert(ReturnEdge(40, 0) == 40);
    float last = 40;
    for (uint32_t ms = 10; ms <= launcher::kPowerReturnMs; ms += 10)
    {
        const float edge = ReturnEdge(40, ms);
        assert(edge <= last + 0.001f);
        last = edge;
    }
    assert(ReturnEdge(40, launcher::kPowerReturnMs) == 0);
    // Eased: slow at both ends.
    assert(40 - ReturnEdge(40, 30) < ReturnEdge(40, 135) - ReturnEdge(40, 165));

    HoldDetector hold;
    hold.Reset(false);
    hold.Update(true, 100);
    assert(hold.Pressing() && hold.HeldMs(400) == 300);
    assert(hold.Update(true, 100 + kHoldMs) && !hold.Pressing());
    hold.Update(false, 2000);
    assert(!hold.Pressing());
}

void TestPanel()
{
    assert(PanelGrey(255) == 0xFFFF && PanelGrey(0) == 0);
    const uint16_t white = PanelGrey(255), black = PanelGrey(0);
    assert(PanelMix(white, black, 0) == white && PanelMix(white, black, 256) == black);
    const uint16_t half = PanelMix(white, black, 128);
    const uint16_t c = static_cast<uint16_t>((half >> 8) | (half << 8));
    assert(((c >> 11) & 31) == 16 && ((c >> 5) & 63) == 32);
}

// NVS pages as ESP-IDF 5.3 writes them: header, entry state table, 32-byte items.
struct Page {
    uint8_t bytes[kNvsPageBytes];
    int next = 0;
    Page()
    {
        std::memset(bytes, 0xFF, sizeof(bytes));
        const uint32_t active = 0xFFFFFFFEu;
        std::memcpy(bytes, &active, 4);
    }
    void SetState(int index, uint8_t state)
    {
        uint8_t& b = bytes[32 + index / 4];
        b = static_cast<uint8_t>((b & ~(3 << ((index % 4) * 2))) | (state << ((index % 4) * 2)));
    }
    int Add(uint8_t ns, uint8_t type, const char* key, uint8_t value, uint8_t span = 1, uint8_t state = 2)
    {
        const int index = next;
        uint8_t* item = bytes + 64 + index * 32;
        std::memset(item, 0xFF, 32);
        item[0] = ns;
        item[1] = type;
        item[2] = span;
        item[3] = 0xFF;
        std::memset(item + 8, 0, 16);
        std::strncpy(reinterpret_cast<char*>(item + 8), key, 15);
        item[24] = value;
        for (int i = 0; i < span; ++i) SetState(index + i, state);
        next += span;
        return index;
    }
};

void TestNvs()
{
    Page namespaces;
    namespaces.Add(0, 0x01, "l_wifi", 1);
    namespaces.Add(0, 0x01, "launcher", 2);
    Page values;
    values.Add(1, 0x21, "s_00000000", 0, 3);         // a string of three entries (its data is not an item)
    values.Add(2, 0x01, "lang", 3, 1, 0);           // erased: an older value
    values.Add(2, 0x01, "remote_install", 1);
    values.Add(2, 0x01, "lang", 1);
    values.Add(1, 0x01, "lang", 5);                 // another namespace's key of the same name
    assert(NvsNamespaceIndex(namespaces.bytes, "launcher") == 2);
    assert(NvsNamespaceIndex(namespaces.bytes, "nothing") == 0);
    assert(NvsNamespaceIndex(values.bytes, "launcher") == 0);
    uint8_t value = 0;
    assert(NvsFindU8(values.bytes, 2, "lang", value) && value == 1);
    assert(!NvsFindU8(values.bytes, 3, "lang", value));
    assert(!NvsFindU8(values.bytes, 0, "lang", value));
    Page empty;
    std::memset(empty.bytes, 0xFF, sizeof(empty.bytes));  // uninitialised: not read
    assert(NvsNamespaceIndex(empty.bytes, "launcher") == 0);
    Page corrupt;
    corrupt.Add(0, 0x01, "launcher", 2);
    const uint32_t unknown = 0x12345678u;
    std::memcpy(corrupt.bytes, &unknown, 4);
    assert(NvsNamespaceIndex(corrupt.bytes, "launcher") == 0);
}

}  // namespace

int main()
{
    TestHold();
    TestKeys();
    TestList();
    TestWords();
    TestFrames();
    TestListFade();
    TestCurtain();
    TestPanel();
    TestNvs();
    std::puts("system menu: ok");
    return 0;
}
