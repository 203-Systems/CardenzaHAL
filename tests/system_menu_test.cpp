// system/menu/logic.h: the system menu's long press, keys, list, words, drawing and the NVS reader.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "system/menu/logic.h"

using namespace CardenzaHAL::SystemMenu::detail;

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
    assert(repeat.Update(Key::Down, kRepeatDelayMs - 1) == Key::None);
    assert(repeat.Update(Key::Down, kRepeatDelayMs) == Key::Down);
    assert(repeat.Update(Key::Down, kRepeatDelayMs + kRepeatMs - 1) == Key::None);
    assert(repeat.Update(Key::Down, kRepeatDelayMs + kRepeatMs) == Key::Down);
    assert(repeat.Update(Key::None, 2000) == Key::None);
    // Select and Back never repeat: a held ENTER chooses once.
    assert(repeat.Update(Key::Select, 3000) == Key::Select);
    assert(repeat.Update(Key::Select, 3000 + 5 * kRepeatDelayMs) == Key::None);
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

void TestWords()
{
    assert(LanguageIndex(0) == 0 && LanguageIndex(1) == 1 && LanguageIndex(2) == 1 && LanguageIndex(3) == 2 &&
           LanguageIndex(4) == 3 && LanguageIndex(5) == 4 && LanguageIndex(6) == 0 && LanguageIndex(255) == 0);
    for (int language = 0; language < kLanguageCount; ++language)
        for (int id = 0; id < kTextCount; ++id)
        {
            const char* text = kTexts[language][id];
            assert(text != nullptr && text[0] != 0);
            if (!HasGlyphs(kFaces[language], text))
            {
                std::fprintf(stderr, "no glyph in language %d, text %d: %s\n", language, id, text);
                assert(false);
            }
        }
    assert(TextWidth(Face::Font5x7, "Restart") == 7 * 6);
    assert(TextWidth(Face::Cjk, "关机") == 24);
    assert(TextWidth(Face::Cjk, "USB") == 18);
    assert(!HasGlyphs(Face::Font5x7, "关机"));
}

// Every pixel painted, the same in any band split, inside the screen.
void TestPaint()
{
    std::vector<uint16_t> whole(kWidth * kHeight, 0x1234);
    std::vector<uint16_t> banded(kWidth * kHeight, 0x5678);
    for (int language = 0; language < kLanguageCount; ++language)
        for (int selected = 0; selected < kItemCount; ++selected)
            for (int prompt = 0; prompt < 2; ++prompt)
            {
                View view;
                view.language = language;
                view.state.selected = selected;
                view.state.scroll = selected < kVisibleRows ? 0 : selected - kVisibleRows + 1;
                view.firmware = "A firmware with a very long name 1.2.3-dev.456";
                view.releasePrompt = prompt != 0;
                Paint(view, 0, kHeight, whole.data());
                for (int y = 0; y < kHeight; y += 8)
                    Paint(view, y, y + 8 <= kHeight ? 8 : kHeight - y, banded.data() + y * kWidth);
                assert(whole == banded);
                for (uint16_t pixel : whole) assert(pixel != 0x1234);
            }
    // The selection is a dark block, the rest of the content white, the rail black.
    View view;
    view.state.selected = 1;
    Paint(view, 0, kHeight, whole.data());
    const uint16_t white = 0xFFFF, black = 0x0000;
    assert(whole[10 * kWidth + 2] == black);              // the rail
    assert(whole[2 * kWidth + 100] == white);             // above the list
    const int selectedTop = 4 + 32;                        // the second row
    const uint16_t block = whole[(selectedTop + 1) * kWidth + 31];
    assert(block != white && block != black);
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
    TestPaint();
    TestNvs();
    std::puts("system menu: ok");
    return 0;
}
