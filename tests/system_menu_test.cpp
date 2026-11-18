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

// HOME held: nothing for a tap's length, then the curtain's edge goes out to kPullOutPixels, eases back to
// kPullRestPixels and is at rest there when the hold is complete (the user, 2026-11-18: out some, back a
// little, then the pop).
void TestPull()
{
    assert(PullEdge(0) == 0 && PullEdge(kPullDelayMs) == 0);
    assert(std::fabs(PullEdge(kPullOutMs) - kPullOutPixels) < 0.01f);
    assert(std::fabs(PullEdge(kHoldMs) - kPullRestPixels) < 0.01f && PullEdge(kHoldMs + 500) == PullEdge(kHoldMs));
    // Enough to read what it is (the rail and the first words of the rows), never most of the screen; back
    // only a little.
    assert(kPullOutPixels >= 96 && kPullOutPixels <= kWidth / 2);
    assert(kPullBackPixels >= 8 && kPullBackPixels <= kPullOutPixels / 4);
    for (uint32_t ms = 0; ms < kHoldMs; ++ms)
    {
        const float edge = PullEdge(ms), next = PullEdge(ms + 1);
        assert(edge >= 0 && edge <= kPullOutPixels + 0.01f);
        // Out until kPullOutMs, back after it, and never a jump: under a pixel a millisecond.
        if (ms < kPullOutMs) assert(next >= edge - 0.001f);
        else assert(next <= edge + 0.001f);
        assert(std::fabs(next - edge) < 1.f);
        // PullSpeed is the curve's slope.
        if (ms > 0) assert(std::fabs(PullSpeed(ms) - (next - PullEdge(ms - 1)) / 2) < 0.02f);
    }
    // At rest where it starts, where it turns and where it pops (the Launcher's hold-to-act curve both ways).
    assert(PullSpeed(kPullDelayMs) == 0 && PullSpeed(kPullOutMs) == 0 && PullSpeed(kHoldMs) == 0);
    assert(PullEdge(kPullDelayMs + 10) < 0.5f && kPullOutPixels - PullEdge(kPullOutMs - 10) < 0.5f);
    assert(PullEdge(kHoldMs - 10) - kPullRestPixels < 0.5f);
    const float middle = PullEdge((kPullDelayMs + kPullOutMs) / 2);
    assert(std::fabs(middle - kPullOutPixels / 2.f) < 1.f);  // eased both ways: half way at half the time

    HoldDetector hold;
    hold.Reset(false);
    hold.Update(true, 100);
    assert(hold.Pressing() && hold.HeldMs(400) == 300);
    assert(hold.Update(true, 100 + kHoldMs) && !hold.Pressing());
    hold.Update(false, 2000);
    assert(!hold.Pressing());
}

// The curtain over time, a frame every 5 ms as system/menu.cpp draws it: `press` the times HOME goes down and
// up in turn. Returns the edge of every frame.
std::vector<float> RunCurtain(Curtain& curtain, std::initializer_list<uint32_t> changes, uint32_t until,
                              uint32_t start = 70000)
{
    std::vector<float> edges;
    bool down = false;
    uint32_t since = 0;
    auto next = changes.begin();
    for (uint32_t ms = 0; ms <= until; ms += 5)
    {
        while (next != changes.end() && *next <= ms)
        {
            down = !down;
            since = *next++;
        }
        edges.push_back(curtain.Update(down, down ? ms - since : 0, start + ms));
    }
    return edges;
}

void TestCurtain()
{
    // Held to the end: the pull itself, and still moving.
    {
        Curtain curtain;
        assert(!curtain.Moving(70000));
        const auto edges = RunCurtain(curtain, {0}, kHoldMs);
        for (std::size_t i = 0; i < edges.size(); ++i) assert(edges[i] == PullEdge(uint32_t(i) * 5));
        assert(curtain.Moving(70000 + kHoldMs));
    }
    // Let go at any moment of the hold: the edge goes back to the left from where it is, with no jump (a
    // step is never larger than the pull's own largest), never further out than it was, and is gone
    // kReturnMs later.
    float largest = 0;
    for (uint32_t ms = 0; ms < kHoldMs; ms += 5) largest = std::max(largest, std::fabs(PullEdge(ms + 5) - PullEdge(ms)));
    for (uint32_t release = 10; release < kHoldMs; release += 35)
    {
        Curtain curtain;
        const auto edges = RunCurtain(curtain, {0, release}, release + kReturnMs + 20);
        const std::size_t at = release / 5 + (release % 5 ? 1 : 0);  // the first frame after HOME went up
        const float from = edges[at - 1];
        for (std::size_t i = at; i < edges.size(); ++i)
        {
            assert(edges[i] <= edges[i - 1] + 0.001f && edges[i] <= from + 0.001f);
            assert(edges[i - 1] - edges[i] <= std::max(largest, 3 * from / kReturnMs * 5) + 0.01f);
        }
        assert(std::fabs(edges[at] - from) < 2.f);
        assert(edges.back() == 0 && !curtain.Moving(70000 + release + kReturnMs + 20));
        // Not linear: half way through the return, more than half of the way is... eased: slow at the end.
        if (from > 20)
        {
            const float nearEnd = edges[at + (kReturnMs - 30) / 5];
            assert(nearEnd < from * 0.06f);
        }
    }
    // Let go while it eases back: it keeps the speed it has (the first step is as large as the last one of
    // the pull, within a little); let go while it goes out: it starts back from rest.
    {
        Curtain back;
        const auto edges = RunCurtain(back, {0, 800}, 900);
        const float before = edges[159] - edges[158], after = edges[161] - edges[160];
        assert(before < -0.1f && after < before * 0.6f);
        Curtain out;
        const auto going = RunCurtain(out, {0, 370}, 500);
        assert(going[73] - going[72] > 1.f && std::fabs(going[75] - going[74]) < 0.4f);
    }
    // A new press while it goes back: no jump, the pull is added to what is left of the way back, and held
    // to the end the edge is where the pop expects it.
    {
        Curtain curtain;
        const auto edges = RunCurtain(curtain, {0, 600, 700}, 700 + kHoldMs);
        for (std::size_t i = 1; i < edges.size(); ++i) assert(std::fabs(edges[i] - edges[i - 1]) <= largest + 0.5f);
        assert(edges[140] > 20 && edges[140] < kPullOutPixels);  // the press came in mid-return
        assert(std::fabs(edges.back() - kPullRestPixels) < 0.01f);
        for (float edge : edges) assert(edge >= 0 && edge <= kWidth);
    }
    // After the menu closed: from the curtain's rest back to the left, from rest, eased.
    {
        Curtain curtain;
        curtain.ReturnFrom(kPullRestPixels, 70000);
        assert(curtain.Moving(70000) && curtain.Update(false, 0, 70000) == kPullRestPixels);
        const float early = curtain.Update(false, 0, 70000 + 30), middle = curtain.Update(false, 0, 70000 + kReturnMs / 2);
        assert(early > kPullRestPixels - 4 && std::fabs(middle - kPullRestPixels / 2.f) < 1.f);
        assert(curtain.Update(false, 0, 70000 + kReturnMs) == 0 && !curtain.Moving(70000 + kReturnMs));
    }
    // The clock wraps: nothing comes back.
    {
        Curtain curtain;
        (void)RunCurtain(curtain, {0, 300}, 300 + kReturnMs + 20, 0xFFFFFF00u);
        assert(curtain.Update(false, 0, 0x7FFFFFFFu) == 0 && curtain.Update(false, 0, 0xFFFFFFF0u) == 0);
    }
}

// The panel takes RGB565 in the processor's byte order (the HAL sets it to little-endian data): a grey is
// the Launcher's Lcd::rgb as it is, not with its bytes swapped as the Launcher sends it to its own setup of
// the same panel.
void TestPanel()
{
    assert(PanelGrey(255) == 0xFFFF && PanelGrey(0) == 0);
    for (int grey = 0; grey < 256; ++grey)
    {
        const uint16_t c = PanelGrey(static_cast<uint8_t>(grey));
        const int red = c >> 11, green = (c >> 5) & 63, blue = c & 31;
        assert(red == grey >> 3 && green == grey >> 2 && blue == grey >> 3);  // a grey: no tint
    }
    assert(PanelGrey(20) == 0x10A2);  // the selection bar (swapped it was 0xA210: a dull pink)
    const uint16_t white = PanelGrey(255), black = PanelGrey(0);
    assert(PanelMix(white, black, 0) == white && PanelMix(white, black, 256) == black);
    const uint16_t half = PanelMix(white, black, 128);
    assert(((half >> 11) & 31) == 16 && ((half >> 5) & 63) == 32 && (half & 31) == 16);
    // A fade between a colour of the firmware's and a grey stays between them, channel by channel.
    const uint16_t orange = 0xFAC6;  // 255, 90, 54
    for (int weight = 0; weight <= 256; weight += 16)
    {
        const uint16_t mixed = PanelMix(orange, PanelGrey(20), weight);
        assert((mixed >> 11) <= 31 && (mixed >> 11) >= 2 && ((mixed >> 5) & 63) <= 22 && ((mixed >> 5) & 63) >= 5);
        assert((mixed & 31) <= 6 && (mixed & 31) >= 2);
    }
}

// The backlight as the Launcher sets it: its brightness setting on its duty curve.
void TestBacklight()
{
    assert(LauncherBrightness(false, 0) == 100 && LauncherBrightness(true, 0) == 100);
    assert(LauncherBrightness(true, 80) == 80 && LauncherBrightness(true, 3) == 10 && LauncherBrightness(true, 400) == 100);
    assert(LauncherBrightness(true, -5) == 10);
    // The Launcher's own numbers (its static_assert: 0, 203 and 1023 of 1023 at 0, 10 and 100 %).
    assert(LauncherBacklightDuty(0, 1023) == 0 && LauncherBacklightDuty(100, 1023) == 1023);
    assert(std::abs(LauncherBacklightDuty(10, 1023) - 203) <= 1);
    for (int percent = 1; percent <= 100; ++percent)
    {
        const int launcher = 1023 * (1100 + 89 * percent) / 10000;
        assert(std::abs(LauncherBacklightDuty(percent, 1023) - launcher) <= 1);
        // The HAL's 12 bits: the same part of the time.
        assert(std::abs(LauncherBacklightDuty(percent, 4095) * 1023 - launcher * 4095) <= 4095 + 1023);
    }
    assert(LauncherBacklightDuty(100, 4095) == 4095);
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
    int Add(uint8_t ns, uint8_t type, const char* key, int32_t value, uint8_t span = 1, uint8_t state = 2)
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
        std::memcpy(item + 24, &value, 4);  // little-endian, as the chip
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
    // The Launcher's brightness is an i32; a u8 of the same name is not it.
    values.Add(2, 0x14, "bright", 60, 1, 0);
    values.Add(2, 0x01, "bright", 7);
    values.Add(2, 0x14, "bright", 80);
    values.Add(1, 0x14, "bright", 30);
    int32_t bright = 0;
    assert(NvsFindI32(values.bytes, 2, "bright", bright) && bright == 80);
    assert(!NvsFindI32(values.bytes, 2, "lang", bright) && !NvsFindI32(values.bytes, 0, "bright", bright));
    values.Add(2, 0x14, "dimtime", -3);
    assert(NvsFindI32(values.bytes, 2, "dimtime", bright) && bright == -3);
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
    TestPull();
    TestCurtain();
    TestBacklight();
    TestPanel();
    TestNvs();
    std::puts("system menu: ok");
    return 0;
}
