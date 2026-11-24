// system/menu/logic.h and frame.h: the system menu's press, backdrop and dissolve, keys, list, words, its frames (the
// Launcher's own, pixel for pixel, the selection bar's slide too) and what they send to the panel, and the
// NVS reader.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iterator>
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

// A press of HOME opens the menu once it has lasted kPressMs: once a press, never a bounce, never a press
// that was already down.
void TestPress()
{
    PressDetector hold;
    hold.Reset(false);
    assert(!hold.Pending());
    assert(!hold.Update(true, 0) && hold.Pending());
    assert(!hold.Update(true, kPressMs - 1));
    assert(hold.Update(true, kPressMs) && !hold.Pending());
    assert(!hold.Update(true, kPressMs + 500));  // once per press
    assert(!hold.Update(false, kPressMs + 600));
    assert(!hold.Update(true, 5000));
    assert(hold.Update(true, 5000 + kPressMs));
    // A bounce or a glitch: down for less than kPressMs opens nothing, and the next press counts from its own start.
    assert(!hold.Update(false, 6000) && !hold.Update(true, 6010) && !hold.Update(true, 6020) && !hold.Update(false, 6030));
    assert(!hold.Pending() && !hold.Update(true, 6040) && !hold.Update(true, 6040 + kPressMs - 1) && hold.Update(true, 6040 + kPressMs));
    assert(kPressMs >= 20 && kPressMs <= 50);  // past a contact's bounce, and at once to a finger

    // Down when watching began (the firmware started with HOME held): no menu until it was let go.
    hold.Reset(true);
    assert(!hold.Update(true, 0) && !hold.Pending());
    assert(!hold.Update(true, 1000 * kPressMs));
    assert(!hold.Update(false, 10 * kPressMs + 1));
    assert(!hold.Update(true, 20000));
    assert(hold.Update(true, 20000 + kPressMs));

    // Time wraps (a uint32_t of milliseconds, after 49 days).
    hold.Reset(false);
    assert(!hold.Update(true, 0xFFFFFF00u));
    assert(hold.Update(true, 0xFFFFFF00u + kPressMs));
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
    assert(!o.close && !o.leave && !o.changed && state.main.selected == 0);
    for (int i = 1; i < kItemCount; ++i)
    {
        o = Apply(state, Key::Down);
        assert(o.changed && state.main.selected == i);
        assert(state.main.selected >= state.main.scroll && state.main.selected < state.main.scroll + kVisibleRows);
    }
    o = Apply(state, Key::Down);  // at the bottom: stays (no wrap, as the Launcher)
    assert(!o.changed && state.main.selected == kItemCount - 1);
    assert(state.main.scroll == kItemCount - kVisibleRows);
    o = Apply(state, Key::Select);
    assert(o.leave && o.item == Item::Download);
    o = Apply(state, Key::Back);
    assert(o.close && !o.leave);
    state = State{};
    o = Apply(state, Key::Select);
    assert(o.close);
    // Back to Launcher, Restart, Turn off leave; Settings and About are pages.
    const Item leaving[] = {Item::Launcher, Item::Restart, Item::PowerOff};
    for (Item item : leaving)
    {
        state = State{};
        state.main.selected = static_cast<int>(item);
        o = Apply(state, Key::Select);
        assert(o.leave && o.item == item && state.page == Page::Main);
    }
    state = State{};
    state.main.selected = static_cast<int>(Item::About);
    o = Apply(state, Key::Select);
    assert(o.page && state.page == Page::About && !o.leave && !o.close);
    o = Apply(state, Key::Back);
    assert(o.page && state.page == Page::Main && state.main.selected == static_cast<int>(Item::About));
    o = Apply(state, Key::Select);
    o = Apply(state, Key::Select);  // ENTER closes About too
    assert(state.page == Page::Main);
}

namespace Settings = CardenzaHAL::Settings;
using Settings::Id;
using Settings::detail::Index;

// The Settings page: a row per setting and Reset; ENTER opens a value, the languages or the question; ESC
// goes back a page at a time.
void TestSettingsPages()
{
    State state;
    state.main.selected = static_cast<int>(Item::Settings);
    Outcome o = Apply(state, Key::Select);
    assert(o.page && state.page == Page::Settings && state.settings.selected == 0);
    // Volume: ; less, . more, by its step, live (the outcome says what to set), never past the limit.
    state.values[Index(Id::Volume)] = 70;
    state.values[Index(Id::VolumeLimit)] = 80;
    o = Apply(state, Key::Select);
    assert(o.page && state.page == Page::Value && state.editing == Id::Volume && state.original == 70);
    o = Apply(state, Key::Down);
    assert(o.set && o.id == Id::Volume && o.value == 72 && state.values[Index(Id::Volume)] == 72);
    for (int i = 0; i < 20; ++i) o = Apply(state, Key::Down);
    assert(state.values[Index(Id::Volume)] == 80);  // the limit
    o = Apply(state, Key::Down);
    assert(!o.set && !o.changed);
    // ESC puts back what it was.
    o = Apply(state, Key::Back);
    assert(o.set && o.id == Id::Volume && o.value == 70 && state.page == Page::Settings);
    // ENTER keeps it.
    o = Apply(state, Key::Select);
    o = Apply(state, Key::Up);
    assert(o.set && o.value == 68);
    o = Apply(state, Key::Select);
    assert(!o.set && state.page == Page::Settings && state.values[Index(Id::Volume)] == 68);
    // The limit lowers the volume with it.
    Apply(state, Key::Down);
    assert(state.settings.selected == static_cast<int>(Row::VolumeLimit));
    Apply(state, Key::Select);
    for (int i = 0; i < 40; ++i) Apply(state, Key::Up);
    assert(state.values[Index(Id::VolumeLimit)] == 10 && state.values[Index(Id::Volume)] == 10);
    Apply(state, Key::Select);
    // Screen brightness in the Launcher's steps of 5, from 10.
    Apply(state, Key::Down);
    Apply(state, Key::Select);
    assert(state.editing == Id::ScreenBrightness);
    state.values[Index(Id::ScreenBrightness)] = 100;
    o = Apply(state, Key::Up);
    assert(o.value == 95);
    Apply(state, Key::Back);
    // Language: the list opens on the current one; ENTER takes another.
    state.settings.selected = static_cast<int>(Row::Language);
    state.values[Index(Id::Language)] = 3;  // Japanese
    o = Apply(state, Key::Select);
    assert(o.page && state.page == Page::Language && state.language.selected == 2);
    Apply(state, Key::Up);
    o = Apply(state, Key::Select);
    assert(o.set && o.id == Id::Language && o.value == 1 && state.page == Page::Settings);
    state.settings.selected = static_cast<int>(Row::Language);
    Apply(state, Key::Select);
    o = Apply(state, Key::Back);
    assert(!o.set && state.page == Page::Settings && state.values[Index(Id::Language)] == 1);
    // The browser notice (WebUSB landing page): the list opens on the current choice; ENTER takes another, ESC
    // keeps what it was.
    state.settings.selected = static_cast<int>(Row::WebUsbLanding);
    assert(RowSetting(Row::WebUsbLanding) == Id::WebUsbLanding);
    assert(state.values[Index(Id::WebUsbLanding)] == Settings::kLandingFirstTime);
    o = Apply(state, Key::Select);
    assert(o.page && state.page == Page::Landing && state.landing.selected == Settings::kLandingFirstTime);
    o = Apply(state, Key::Up);
    assert(!o.changed && state.landing.selected == 0);  // no wrap
    Apply(state, Key::Down);
    Apply(state, Key::Down);
    o = Apply(state, Key::Down);
    assert(!o.changed && state.landing.selected == Settings::kLandingNever);
    o = Apply(state, Key::Select);
    assert(o.set && o.id == Id::WebUsbLanding && o.value == Settings::kLandingNever && state.page == Page::Settings);
    state.settings.selected = static_cast<int>(Row::WebUsbLanding);
    o = Apply(state, Key::Select);
    assert(state.landing.selected == Settings::kLandingNever);
    Apply(state, Key::Up);
    o = Apply(state, Key::Back);
    assert(!o.set && state.page == Page::Settings && state.values[Index(Id::WebUsbLanding)] == Settings::kLandingNever);
    // Reset: Cancel focused; ; and , to Reset, ENTER resets every setting but the language.
    state.settings.selected = static_cast<int>(Row::Reset);
    o = Apply(state, Key::Select);
    assert(o.page && state.page == Page::Reset && state.button == 1);
    o = Apply(state, Key::Select);
    assert(!o.reset && state.page == Page::Settings);
    Apply(state, Key::Select);
    o = Apply(state, Key::Up);
    assert(o.changed && state.button == 0);
    o = Apply(state, Key::Up);
    assert(!o.changed && state.button == 0);
    o = Apply(state, Key::Select);
    assert(o.reset && state.page == Page::Settings);
    assert(state.values[Index(Id::Volume)] == Settings::detail::kInfo[Index(Id::Volume)].defaultValue);
    assert(state.values[Index(Id::WebUsbLanding)] == Settings::kLandingFirstTime);  // back to the first time only
    assert(state.values[Index(Id::Language)] == 1);
    // ESC: Settings back to the main list.
    o = Apply(state, Key::Back);
    assert(o.page && state.page == Page::Main && state.main.selected == static_cast<int>(Item::Settings));
}

void TestValueText()
{
    char text[16];
    ValueText(Id::Volume, 70, text, sizeof(text));
    assert(std::strcmp(text, "70") == 0);
    ValueText(Id::ScreenBrightness, 80, text, sizeof(text));
    assert(std::strcmp(text, "80%") == 0);
    ValueText(Id::KeyBrightness, 12, text, sizeof(text));
    assert(std::strcmp(text, "12") == 0);
    SizeText(16u * 1024 * 1024, text, sizeof(text));
    assert(std::strcmp(text, "16 MB") == 0);
    SizeText(31914983424ull, text, sizeof(text));
    assert(std::strcmp(text, "29.7 GB") == 0);
    SizeText(2048, text, sizeof(text));
    assert(std::strcmp(text, "2 KB") == 0);
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
    view.state.main = {run.presses[0].selected, run.presses[0].scroll};
    frame(view, slide, kSlideStart);
    int frames = 1, next = 1;
    for (uint32_t ms = 0; ms <= run.length; ms += kSlideFrameMs, ++frames)
    {
        for (; next < run.count && run.presses[next].ms <= ms; ++next)
            view.state.main = {run.presses[next].selected, run.presses[next].scroll};
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
    {
        View view;
        view.language = language;
        view.firmware = "CardenzaSynthFirmware 0.2.1";
        for (int i = 0; i < int(std::size(kSampleValues)); ++i) view.state.values[i] = kSampleValues[i];
        view.state.values[Index(Id::Language)] = Settings::detail::kLanguageCodes[language];
        int at = 0;
        for (const StillPage& still : kStillPages)
            for (int index = 0; index < still.count; ++index, ++at)
            {
                const ListState list{index, index < kVisibleRows ? 0 : index - kVisibleRows + 1};
                view.state.page = still.page == 0 ? Page::Main : still.page == 1 ? Page::Settings
                                : still.page == 2 ? Page::Language : still.page == 3 ? Page::Reset : Page::Landing;
                view.state.main = still.page == 0 ? list : ListState{};
                view.state.settings = still.page == 1 ? list : ListState{};
                view.state.language = still.page == 2 ? list : ListState{};
                view.state.landing = still.page == 4 ? list : ListState{};
                view.state.button = index;
                const std::vector<uint8_t> whole = Frame(view);
                if (Fnv(whole) != kLauncherFrameHashes[language * kStillCount + at])
                {
                    std::fprintf(stderr, "language %d, page %d, %d: not the Launcher's frame\n", language, still.page, index);
                    assert(false);
                }
                assert(Frame(view, nullptr, 0, 8) == whole);
                assert(Frame(view, nullptr, 0, 5) == whole);
                // A slide that has come to rest draws the still frame.
                if (still.page != 3)
                {
                    ListSlide slide;
                    assert(Frame(view, &slide, 100) == whole);
                }
            }
        assert(at == kStillCount);
    }
    // The release prompt replaces the chosen row's description; Back held dims the rail's arrow.
    View view;
    view.state.main.selected = 2;
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
    from.state.main.selected = 1;
    View to = from;
    to.state.main.selected = 2;
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
    three.state.main.selected = 3;
    (void)Frame(three, &slide, 2000);
    const std::vector<uint8_t> away = Frame(three, &slide, 2060);
    assert(away != Frame(to) && away != Frame(three));
    (void)Frame(to, &slide, 2060);
    for (uint32_t ms = 2064; ms < 2060 + ListSlide::kTrailMs; ms += 4)
        assert(BarRows(Frame(to, &slide, ms)) <= int(std::lround(step * (1 + ListSlide::kStretch))) + 1);
    assert(Frame(to, &slide, 2060 + ListSlide::kTrailMs) == Frame(to));
    // Past the last row: the rows move up by one, the bar stays where it is on screen.
    View third = from;
    third.state.main.selected = 3;
    (void)Frame(third, &slide, 3000);
    (void)Frame(third, &slide, 3000 + ListSlide::kTrailMs);
    View last = from;
    last.state.main.selected = 4;
    last.state.main.scroll = 1;
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
                const bool jump = view.state.main.scroll != previousScroll;
                previousScroll = view.state.main.scroll;
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

// Everything the menu puts on the panel is black, white or grey (the user, 2026-11-18: "必须要让它只保持黑白灰"):
// red and blue the same five bits, green those bits widened to six as the panel widens them.
void TestPanel()
{
    assert(PanelGrey(255) == 0xFFFF && PanelGrey(0) == 0);
    uint16_t previous = 0;
    for (int grey = 0; grey < 256; ++grey)
    {
        const uint16_t c = PanelGrey(static_cast<uint8_t>(grey));
        const int red = c >> 11, green = (c >> 5) & 63, blue = c & 31;
        assert(PanelNeutral(c) && red == grey >> 3 && blue == red && green == (red << 1 | red >> 4));
        // Never further from the Launcher's own pixel than green's last bit, and never darker for a lighter grey.
        assert(std::abs(green - (grey >> 2)) <= 1 && c >= previous);
        previous = c;
    }
    assert(!PanelNeutral(0x10A2) && !PanelNeutral(0xA210) && !PanelNeutral(0xF800) && !PanelNeutral(0x0020));
}

// The backdrop: the firmware's picture dark and grey through the 4x4 dither; the dissolve: the menu's pixels
// in the dither's order.
void TestBackdrop()
{
    int seen[16] = {};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            assert(Bayer(x, y) >= 0 && Bayer(x, y) < 16 && Bayer(x + 4, y + 8) == Bayer(x, y));
            ++seen[Bayer(x, y)];
        }
    for (int count : seen) assert(count == 1);
    // Every colour a firmware can show, at every place of the dither: one of five dark greys, neutral.
    for (int pixel = 0; pixel < 65536; ++pixel)
    {
        int sum = 0;
        for (int at = 0; at < 16; ++at)
        {
            const uint8_t grey = BackdropGrey(static_cast<uint16_t>(pixel), at & 3, at >> 2);
            assert(grey % 32 == 0 && grey <= 128 && PanelNeutral(PanelGrey(grey)));
            sum += grey;
        }
        // The dither's sixteen pixels together are two fifths of the colour's luma.
        const int r5 = pixel >> 11, g6 = (pixel >> 5) & 63, b5 = pixel & 31;
        const int luma = (54 * (r5 << 3 | r5 >> 2) + 183 * (g6 << 2 | g6 >> 4) + 19 * (b5 << 3 | b5 >> 2) + 128) >> 8;
        assert(std::abs(sum / 16 - luma * 2 / 5) <= 2);
    }
    assert(BackdropGrey(0x0000, 1, 1) == 0 && BackdropGrey(0xFFFF, 0, 0) >= 96);
    // Pure red, green and blue are greys by how light they look: green the lightest.
    const auto mean = [](uint16_t pixel) {
        int sum = 0;
        for (int at = 0; at < 16; ++at) sum += BackdropGrey(pixel, at & 3, at >> 2);
        return sum / 16;
    };
    assert(mean(0x07E0) > mean(0xF800) && mean(0xF800) > mean(0x001F));
    // Without a picture: a dark field, dithered (two greys).
    int dark = 0;
    for (int at = 0; at < 16; ++at)
    {
        const uint8_t grey = BackdropField(at & 3, at >> 2);
        assert(grey == 0 || grey == 32);
        dark += grey == 0;
    }
    assert(dark > 0 && dark < 16);
    // The dissolve: none of the menu at step 0, a quarter more each step, a step's pixels kept by the next.
    for (int step = 0; step <= kDissolveSteps; ++step)
    {
        int count = 0;
        for (int at = 0; at < 16; ++at)
        {
            const bool menu = Dissolved(at & 3, at >> 2, step);
            count += menu;
            if (step > 0 && Dissolved(at & 3, at >> 2, step - 1)) assert(menu);
        }
        assert(count == step * 16 / kDissolveSteps);
    }
}

// Every pixel of every frame the menu shows is neutral: the backdrop over a firmware picture of every colour
// (and without one), each step of the dissolve into the menu, and the menu itself, in every language and
// with every row selected, the selection bar's slide included.
void TestNeutralFrames()
{
    std::vector<uint16_t> firmware(kWidth * kHeight);
    for (int i = 0; i < kWidth * kHeight; ++i) firmware[i] = static_cast<uint16_t>(i * 2654435761u >> 12);
    std::vector<uint8_t> menu(kWidth * kHeight);
    long menuPixels = 0, backdropPixels = 0;
    const auto check = [&](int step, bool picture) {
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
            {
                const int i = y * kWidth + x;
                const uint8_t grey = SceneGrey(step, menu[i], picture ? &firmware[i] : nullptr, x, y);
                assert(PanelNeutral(PanelGrey(grey)));
                if (step >= kDissolveSteps) assert(grey == menu[i]);
                if (step == 0) assert(grey == (picture ? BackdropGrey(firmware[i], x, y) : BackdropField(x, y)));
                (grey == menu[i] ? menuPixels : backdropPixels)++;
            }
    };
    for (int language = 0; language < kLanguageCount; ++language)
        for (int selected = 0; selected < kItemCount; ++selected)
        {
            View view;
            view.language = language;
            view.firmware = "CardenzaSynthFirmware 0.2.1";
            view.state.main.selected = selected;
            view.state.main.scroll = selected < kVisibleRows ? 0 : selected - kVisibleRows + 1;
            view.releasePrompt = selected == 2;
            view.backHeld = selected == 3;
            PaintGrey(view, nullptr, 0, 0, kHeight, menu.data());
            for (int step = 0; step <= kDissolveSteps; ++step)
                for (bool picture : {true, false}) check(step, picture);
            // The bar on its way to the next row.
            ListSlide slide;
            slide.keep = true;
            PaintGrey(view, &slide, 9000, 0, kHeight, menu.data());
            view.state.main.selected = std::min(selected + 1, kItemCount - 1);
            view.state.main.scroll = view.state.main.selected < kVisibleRows ? 0 : view.state.main.selected - kVisibleRows + 1;
            for (uint32_t ms : {20u, 60u, 120u, 200u})
            {
                PaintGrey(view, &slide, 9000 + ms, 0, kHeight, menu.data());
                check(kDissolveSteps, true);
            }
        }
    assert(menuPixels > 0 && backdropPixels > 0);
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

// The pages the Launcher has no function for (its Backlight page and About are drawn inline there): every
// setting's value page and the About page, in every language: the value moves the knob, the limit shows on
// the volume's track, every row of About is there, nothing is drawn under the rail but the rail.
void TestOwnPages()
{
    AboutInfo about;
    std::snprintf(about.firmware, sizeof(about.firmware), "CardenzaSynthFirmware");
    std::snprintf(about.version, sizeof(about.version), "0.2.1-dev.42");
    std::snprintf(about.launcher, sizeof(about.launcher), "0.3.3");
    std::snprintf(about.hal, sizeof(about.hal), "9597136abcde+");
    std::snprintf(about.mac, sizeof(about.mac), "F4:12:FA:00:11:22");
    about.flashBytes = 16u << 20;
    about.psramBytes = 2u << 20;
    about.card = true;
    about.cardBytes = 31914983424ull;
    about.cardFree = 30000000000ull;
    for (int language = 0; language < kLanguageCount; ++language)
    {
        View view;
        view.language = language;
        view.about = &about;
        view.state.page = CardenzaHAL::SystemMenu::detail::Page::About;
        const std::vector<uint8_t> page = Frame(view);
        int inked = 0;
        for (int y = 0; y < kHeight; ++y)
            for (int x = 24; x < kWidth; ++x) inked += page[y * kWidth + x] < 128;
        assert(inked > 1500);
        AboutInfo bare = about;
        bare.card = false;
        bare.launcher[0] = 0;
        view.about = &bare;
        assert(Frame(view) != page);
        view.about = &about;
        view.state.page = CardenzaHAL::SystemMenu::detail::Page::Value;
        for (int i = 0; i < Index(Id::Language); ++i)  // the settings with a value page
        {
            const Id id = static_cast<Id>(i);
            const auto& info = Settings::detail::kInfo[i];
            view.state.editing = id;
            view.state.values[i] = info.min;
            const std::vector<uint8_t> low = Frame(view);
            view.state.values[i] = info.max;
            const std::vector<uint8_t> high = Frame(view);
            assert(low != high);
            // The knob: at the track's left end at the minimum, at its right end at the maximum.
            const int knobY = kHeight - 32 - 4;
            assert(low[knobY * kWidth + 35] == 0 && low[knobY * kWidth + 226] == 255);
            assert(high[knobY * kWidth + 227] == 0 && high[knobY * kWidth + 36] == 255);
            view.state.values = Settings::detail::Defaults();
        }
        // The volume's track is lighter past its limit.
        view.state.editing = Id::Volume;
        view.state.values[Index(Id::VolumeLimit)] = 50;
        view.state.values[Index(Id::Volume)] = 20;
        const std::vector<uint8_t> limited = Frame(view);
        const int trackY = kHeight - 32;
        assert(limited[trackY * kWidth + 36 + 60] == 220 && limited[trackY * kWidth + 36 + 150] == 242);
    }
}

int main()
{
    TestPress();
    TestKeys();
    TestList();
    TestSettingsPages();
    TestValueText();
    TestOwnPages();
    TestWords();
    TestFrames();
    TestSlideFrames();
    TestListSlide();
    TestSlideBytes();
    TestBackdrop();
    TestNeutralFrames();
    TestBacklight();
    TestPanel();
    TestNvs();
    std::puts("system menu: ok");
    return 0;
}
