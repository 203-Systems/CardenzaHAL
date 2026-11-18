// system/menu/logic.h and frame.h: the system menu's long press, curtain, keys, list, words, its frames (the
// Launcher's own, pixel for pixel, the selection bar's slide too) and what they send to the panel, and the
// NVS reader.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include <algorithm>
#include <cmath>

#include "display/changed_regions.h"
#include "system/menu/frame.h"
#include "system/menu/launcher/i18n.h"
#include "system/menu/launcher/motion.h"
#include "system/menu/launcher/ui_layout.h"
#include "system/menu/logic.h"

using namespace CardenzaHAL::SystemMenu::detail;
using CardenzaHAL::SystemMenu::launcher::ListSlide;
namespace launcher = CardenzaHAL::SystemMenu::launcher;
namespace Display = CardenzaHAL::Display;

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

// The menu drawn `band` rows at a time, as the overlay does without PSRAM (with it: kHeight at once).
std::vector<uint8_t> Frame(const View& view, ListSlide* slide = nullptr, uint32_t now = 0, int band = kHeight)
{
    std::vector<uint8_t> canvas(kWidth * kHeight, 0x55);
    for (int y = 0; y < kHeight; y += band)
        PaintGrey(view, slide, now, y, std::min(band, kHeight - y), canvas.data() + y * kWidth);
    return canvas;
}

// zlib's CRC-32, as tools/launcher_ui.py keeps the slides' frames.
uint32_t Crc32(uint32_t crc, const std::vector<uint8_t>& bytes)
{
    crc = ~crc;
    for (uint8_t b : bytes)
    {
        crc ^= b;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// A slide of kSlideRuns as the menu's loop draws it (system/menu.cpp): the bar placed on the first press's
// row, then a frame every kSlideFrameMs, each after the presses due by then. frame(view, slide, now) draws
// one; returns how many.
template <typename Draw>
int RunSlide(const SlideRun& run, int language, Draw frame)
{
    View view;
    view.language = language;
    view.firmware = "CardenzaSynthFirmware 0.2.1";
    ListSlide slide;
    slide.keep = true;  // the Cardenza: the rows jump to a scroll, the bar keeps its place on screen
    view.state = {run.presses[0].selected, run.presses[0].scroll};
    frame(view, slide, kSlideStart);
    int frames = 1, next = 1;
    for (uint32_t ms = 0; ms <= run.length; ms += kSlideFrameMs, ++frames)
    {
        for (; next < run.count && run.presses[next].ms <= ms; ++next)
            view.state = {run.presses[next].selected, run.presses[next].scroll};
        frame(view, slide, kSlideStart + ms);
    }
    return frames;
}

// The selection bar's height in a frame: the rows of column 31 (on the bar, left of the text) that are the
// bar's grey.
int BarRows(const std::vector<uint8_t>& frame, int* top = nullptr)
{
    int rows = 0, first = -1;
    for (int y = 0; y < kHeight; ++y)
        if (frame[y * kWidth + 31] == launcher::kUiSelectedGrey)
        {
            if (first < 0) first = y;
            ++rows;
        }
    if (top != nullptr) *top = first;
    return rows;
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
            // A slide that has come to rest draws the still frame.
            ListSlide slide;
            assert(Frame(view, &slide, 100) == whole);
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

// The selection bar's slides are the Launcher's own, frame by frame (tools/launcher_ui.py --compare keeps
// their CRCs), and the same however a frame is cut into bands (the overlay without PSRAM draws every band
// at the frame's `now`: the first aims the slide, the others find it aimed).
void TestSlideFrames()
{
    for (int language = 0; language < kLanguageCount; ++language)
        for (int r = 0; r < kSlideRunCount; ++r)
        {
            uint32_t crc = 0;
            RunSlide(kSlideRuns[r], language, [&](const View& view, ListSlide& slide, uint32_t now) {
                ListSlide banded = slide;
                const std::vector<uint8_t> whole = Frame(view, &slide, now);
                crc = Crc32(crc, whole);
                if (language < 2) assert(Frame(view, &banded, now, 8) == whole);
            });
            if (crc != kLauncherSlideCrcs[language * kSlideRunCount + r])
            {
                std::fprintf(stderr, "language %d, slide %s: not the Launcher's frames\n", language, kSlideRuns[r].name);
                assert(false);
            }
        }
}

// The Launcher's ListSlide: the bar moves by its edges, the leading one first, so it stretches toward the
// new row (never past 1.75 rows) and settles back on it; a press during the slide goes on from where it is;
// past the last row the rows change at once and the bar keeps its place on screen.
void TestListSlide()
{
    const int step = (kHeight - 4) / 4;
    View from;
    from.state.selected = 1;
    View to = from;
    to.state.selected = 2;
    ListSlide slide;
    slide.keep = true;
    assert(Frame(from, &slide, 1000) == Frame(from));  // the first frame places it
    assert(!slide.active(1000));
    // At the press nothing has moved yet; then the bottom edge leads.
    assert(Frame(to, &slide, 1000) == Frame(from) && slide.active(1000));
    int longest = 0, top = 0, previousTop = -1;
    bool grew = false;
    for (uint32_t ms = 4; ms < ListSlide::kTrailMs; ms += 4)
    {
        const std::vector<uint8_t> frame = Frame(to, &slide, 1000 + ms);
        const int rows = BarRows(frame, &top);
        assert(rows >= step && rows <= int(std::lround(step * (1 + ListSlide::kStretch))) + 1);
        assert(top >= previousTop);  // downward, never back
        previousTop = top;
        longest = std::max(longest, rows);
        if (ms == 72) grew = rows > step + step / 2;  // about 1.6 rows at its longest
    }
    assert(grew && longest > step);
    assert(Frame(to, &slide, 1000 + ListSlide::kTrailMs) == Frame(to));
    assert(!slide.active(1000 + ListSlide::kTrailMs));
    // Down, then back up after 60 ms: the bar turns where it is and settles on the row it left.
    View three = from;
    three.state.selected = 3;
    (void)Frame(three, &slide, 2000);
    const std::vector<uint8_t> away = Frame(three, &slide, 2060);
    assert(away != Frame(to) && away != Frame(three));
    (void)Frame(to, &slide, 2060);
    for (uint32_t ms = 2064; ms < 2060 + ListSlide::kTrailMs; ms += 4)
        assert(BarRows(Frame(to, &slide, ms)) <= int(std::lround(step * (1 + ListSlide::kStretch))) + 1);
    assert(Frame(to, &slide, 2060 + ListSlide::kTrailMs) == Frame(to));
    // Past the last row: the rows move up by one, the bar stays where it is on screen.
    View third = from;
    third.state.selected = 3;
    (void)Frame(third, &slide, 3000);
    (void)Frame(third, &slide, 3000 + ListSlide::kTrailMs);
    View last = from;
    last.state.selected = 4;
    last.state.scroll = 1;
    int before = 0, after = 0;
    BarRows(Frame(third, &slide, 4000), &before);
    assert(Frame(last, &slide, 4000) == Frame(last));  // the selected row is where the bar was
    BarRows(Frame(last, &slide, 4000), &after);
    assert(before == after && !slide.active(4000));
    // Without motion (the Launcher's setting): on its row in one frame.
    launcher::motion = false;
    ListSlide still;
    (void)Frame(from, &still, 5000);
    assert(Frame(last, &still, 5000) == Frame(last) && !still.active(5000));
    launcher::motion = true;
}

// What the overlay sends of each frame of a slide (Display::detail::PaintOverlay, the menu's 8-row bands as
// the panel takes them): before, every band that changed went whole; now only the rectangle over its
// changed rows and 30-pixel segments (ChangedRegions::FindBand). Frames every kSlideFrameMs.
struct Sent {
    int frames = 0;
    std::size_t bands = 0, rects = 0, mostBands = 0, mostRects = 0;
};
void TestSlideBytes()
{
    constexpr int kBandRows = Display::ChangedRegions::kBandRows;
    std::vector<uint32_t> table(Display::ChangedRegions::kHashCount);
    std::vector<uint16_t> panel(kWidth * kHeight), shown(kWidth * kHeight);
    std::printf("system menu: bytes a frame of the slide sends (a frame every %u ms; whole changed bands -> changed rectangles)\n",
                unsigned(kSlideFrameMs));
    for (int language = 0; language < 2; ++language)
        for (int r = 0; r < kSlideRunCount; ++r)
        {
            Display::ChangedRegions regions(table.data());
            Sent moving, scroll;
            bool first = true;
            int previousScroll = kSlideRuns[r].presses[0].scroll;
            RunSlide(kSlideRuns[r], language, [&](const View& view, ListSlide& slide, uint32_t now) {
                const std::vector<uint8_t> grey = Frame(view, &slide, now);
                for (int i = 0; i < kWidth * kHeight; ++i) panel[i] = PanelGrey(grey[i]);
                std::size_t bands = 0, rects = 0;
                for (int y = 0; y < kHeight; y += kBandRows)
                {
                    const int rows = std::min(kBandRows, kHeight - y);
                    const std::size_t at = std::size_t(y) * kWidth, count = std::size_t(rows) * kWidth;
                    if (!std::equal(panel.begin() + at, panel.begin() + at + count, shown.begin() + at))
                        bands += count * 2;
                    const Display::Region changed = regions.FindBand(panel.data() + at, kWidth, y, y + rows, first);
                    rects += std::size_t(changed.width) * changed.height * 2;
                    // What the panel shows now: the rectangle sent.
                    for (int row = changed.y; row < changed.y + changed.height; ++row)
                        std::copy_n(panel.begin() + row * kWidth + changed.x, changed.width,
                                    shown.begin() + row * kWidth + changed.x);
                }
                assert(shown == panel);  // nothing changed was left out
                const bool jump = view.state.scroll != previousScroll;
                previousScroll = view.state.scroll;
                if (!first)
                {
                    Sent& sent = jump ? scroll : moving;
                    ++sent.frames;
                    sent.bands += bands;
                    sent.rects += rects;
                    sent.mostBands = std::max(sent.mostBands, bands);
                    sent.mostRects = std::max(sent.mostRects, rects);
                }
                first = false;
            });
            std::printf("  %-7s %-11s %2d frames: avg %5zu -> %4zu, most %5zu -> %4zu",
                        language ? "zh-Hans" : "en", kSlideRuns[r].name, moving.frames,
                        moving.bands / moving.frames, moving.rects / moving.frames, moving.mostBands, moving.mostRects);
            if (scroll.frames)
                std::printf("; the scroll's frame %5zu -> %5zu", scroll.mostBands, scroll.mostRects);
            std::printf("\n");
            std::fflush(stdout);
            // A frame that moves the bar sends the strips its edges swept, and the lines of the rows whose
            // detail opens or closes where their label rises or falls a pixel (three rows at once when a key
            // repeats): under 18 KB (3.6 ms at the panel's 40 MHz), on average under a third of the whole
            // bands. A scroll's frame changes every row's text.
            assert(moving.mostRects <= 18000 && moving.rects * 3 <= moving.bands);
            assert(scroll.mostRects <= scroll.mostBands);
        }
}

// HOME held: the curtain comes out after a tap's length, moves only right while held, and covers the screen when
// the hold is complete (the user, 2026-11-18: pulled out, never drawn back). Letting go removes it at once (menu.cpp).
void TestCurtain()
{
    assert(PullEdge(0) == 0 && PullEdge(kPullDelayMs) == 0);
    float previous = 0;
    for (uint32_t ms = kPullDelayMs; ms <= kHoldMs; ms += 10)
    {
        const float edge = PullEdge(ms);
        assert(edge >= previous - 0.001f && edge <= kWidth + 0.01f);
        previous = edge;
    }
    assert(std::fabs(PullEdge(kHoldMs) - kWidth) < 0.01f);
    assert(std::fabs(PullEdge(kHoldMs + 500) - kWidth) < 0.01f);
    const float middle = PullEdge((kPullDelayMs + kHoldMs) / 2);
    assert(middle > kWidth * 0.4f && middle < kWidth * 0.6f);   // eased both ways: half way at half the hold
    // Each step uncovers a few columns: at 10 ms a step, never more than a sixth of the screen
    for (uint32_t ms = kPullDelayMs; ms + 10 <= kHoldMs; ms += 10) assert(PullEdge(ms + 10) - PullEdge(ms) < kWidth / 6.f);

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
    TestSlideFrames();
    TestListSlide();
    TestSlideBytes();
    TestCurtain();
    TestPanel();
    TestNvs();
    std::puts("system menu: ok");
    return 0;
}
