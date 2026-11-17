// The system menu's decisions and drawing (system/menu/logic.h). Pure: built into the HAL and the host tests.
#include "system/menu/logic.h"

#include <algorithm>
#include <cstring>

#include "system/menu/font5x7.h"

namespace CardenzaHAL::SystemMenu::detail {

namespace {

// ---- Keys and the list --------------------------------------------------------------------------------

void KeepInView(State& state)
{
    state.selected = std::clamp(state.selected, 0, kItemCount - 1);
    state.scroll = std::clamp(state.scroll, state.selected - (kVisibleRows - 1), state.selected);
    state.scroll = std::clamp(state.scroll, 0, std::max(0, kItemCount - kVisibleRows));
}

// ---- Text ---------------------------------------------------------------------------------------------

// The next code point of UTF-8 `text` at `i` (advanced); 0 at the end. Malformed bytes read as '?'.
uint32_t NextCodePoint(const char* text, std::size_t& i)
{
    const auto* s = reinterpret_cast<const unsigned char*>(text);
    const unsigned char c = s[i];
    if (c == 0) return 0;
    int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0)
    {
        ++i;
        return '?';
    }
    uint32_t cp = extra == 0 ? c : c & (0x3F >> extra);
    ++i;
    for (int k = 0; k < extra; ++k)
    {
        if ((s[i] & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (s[i] & 0x3F);
        ++i;
    }
    return cp;
}

const CjkGlyph* FindCjk(uint32_t cp)
{
    const CjkGlyph* begin = kCjkGlyphs;
    const CjkGlyph* end = kCjkGlyphs + sizeof(kCjkGlyphs) / sizeof(kCjkGlyphs[0]);
    const CjkGlyph* found = std::lower_bound(begin, end, cp, [](const CjkGlyph& g, uint32_t v) { return g.cp < v; });
    return found != end && found->cp == cp ? found : nullptr;
}

const LatinGlyph* FindLatin(uint32_t cp)
{
    const LatinGlyph* begin = kLatinGlyphs;
    const LatinGlyph* end = kLatinGlyphs + sizeof(kLatinGlyphs) / sizeof(kLatinGlyphs[0]);
    const LatinGlyph* found = std::lower_bound(begin, end, cp, [](const LatinGlyph& g, uint32_t v) { return g.cp < v; });
    return found != end && found->cp == cp ? found : nullptr;
}

// How a character is drawn on a face.
enum class Glyph : uint8_t { Font5x7, Cjk, Latin };
struct Resolved {
    Glyph kind = Glyph::Font5x7;
    uint32_t cp = '?';
    const CjkGlyph* cjk = nullptr;
    const LatinGlyph* latin = nullptr;
    int advance = 6;
};

Resolved Resolve(Face face, uint32_t cp)
{
    Resolved r;
    if (face == Face::Latin)
        if (const LatinGlyph* g = FindLatin(cp))
        {
            r.kind = Glyph::Latin;
            r.latin = g;
            r.cp = cp;
            r.advance = g->advance;
            return r;
        }
    if (face == Face::Cjk && cp >= 0x80)
        if (const CjkGlyph* g = FindCjk(cp))
        {
            r.kind = Glyph::Cjk;
            r.cjk = g;
            r.cp = cp;
            r.advance = 12;
            return r;
        }
    r.cp = cp >= 32 && cp < 127 ? cp : '?';
    return r;
}

// ---- Painting a band ----------------------------------------------------------------------------------

uint16_t Grey(uint8_t g)
{
    const uint16_t v = static_cast<uint16_t>(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

struct Canvas {
    uint16_t* band;
    int top;   // the band's first screen row
    int rows;
    // Rows outside [clipTop, clipBottom) are left alone.
    int clipTop = 0;
    int clipBottom = kHeight;

    void Fill(int x, int y, int w, int h, uint8_t grey) const
    {
        const int x0 = std::max(x, 0), x1 = std::min(x + w, kWidth);
        const int y0 = std::max({y, top, clipTop}), y1 = std::min({y + h, top + rows, clipBottom});
        if (x0 >= x1 || y0 >= y1) return;
        const uint16_t c = Grey(grey);
        for (int yy = y0; yy < y1; ++yy)
        {
            uint16_t* row = band + (yy - top) * kWidth;
            for (int xx = x0; xx < x1; ++xx) row[xx] = c;
        }
    }
    void Dot(int x, int y, uint16_t c) const
    {
        if (x < 0 || x >= kWidth || y < top || y >= top + rows || y < clipTop || y >= clipBottom) return;
        band[(y - top) * kWidth + x] = c;
    }
};

// Draws one resolved character with its top-left pen at (x, y) (y: the 5x7 font's top row), across, or
// turned for the rail: then the pen runs down the screen from y, and x is the left of the 7-pixel column.
void DrawGlyph(const Canvas& canvas, const Resolved& r, int x, int y, uint8_t grey, bool vertical)
{
    const uint16_t c = Grey(grey);
    auto dot = [&](int col, int row) {  // row: from the text origin (5x7 font's top); col: from the pen
        if (vertical) canvas.Dot(x + 6 - row, y + col, c);
        else canvas.Dot(x + col, y + row, c);
    };
    switch (r.kind)
    {
        case Glyph::Cjk:
            for (int row = 0; row < 12; ++row)
                for (int col = 0; col < 12; ++col)
                    if (r.cjk->rows[row] & (0x800 >> col))
                    {
                        // Upright in the rail, one character under the other, centred on the column.
                        if (vertical) canvas.Dot(x - 2 + col, y + row, c);
                        else dot(col, row - 2);
                    }
            break;
        case Glyph::Latin:
            for (int row = 0; row < r.latin->height; ++row)
                for (int col = 0; col < r.latin->width; ++col)
                    if (r.latin->rows[row] & (0x8000 >> col))
                        dot(r.latin->left + col, kLatinBaseline + r.latin->top + row);
            break;
        case Glyph::Font5x7:
        {
            const auto& rows = kFont5x7[r.cp];
            const int offset = font5x7YOffset(static_cast<unsigned char>(r.cp));
            for (int row = 0; row < 7; ++row)
                for (int col = 0; col < 5; ++col)
                    if (rows[row] & (16 >> col)) dot(col, row + offset);
            break;
        }
    }
}

// Draws `text` from (x, y); past `maxWidth` it ends in "..." (the Launcher's uiFitText). Returns the width.
int DrawText(const Canvas* canvas, Face face, const char* text, int x, int y, uint8_t grey, int maxWidth,
             bool vertical = false)
{
    const Resolved dot = Resolve(face, '.');
    const int ellipsis = 3 * dot.advance;
    const bool fits = TextWidth(face, text) <= maxWidth;
    int pen = 0;
    std::size_t i = 0;
    for (uint32_t cp; (cp = NextCodePoint(text, i)) != 0;)
    {
        const Resolved r = Resolve(face, cp);
        if (!fits && pen + r.advance > maxWidth - ellipsis)
        {
            for (int k = 0; k < 3; ++k)
            {
                if (canvas) DrawGlyph(*canvas, dot, vertical ? x : x + pen, vertical ? y + pen : y, grey, vertical);
                pen += dot.advance;
            }
            return pen;
        }
        if (canvas) DrawGlyph(*canvas, r, vertical ? x : x + pen, vertical ? y + pen : y, grey, vertical);
        pen += r.advance;
    }
    return pen;
}

int TextHeight(Face face)
{
    return face == Face::Cjk ? 12 : face == Face::Latin ? kLatinHeight : 7;
}

// Where a line's ink starts, from the text origin (negative: above).
int TextTop(Face face)
{
    return face == Face::Cjk ? -2 : face == Face::Latin ? kLatinTop : 0;
}

TextId Label(Item item)
{
    switch (item)
    {
        case Item::Continue: return kContinue;
        case Item::Launcher: return kLauncher;
        case Item::Restart: return kRestart;
        case Item::PowerOff: return kOff;
        case Item::Download: return kDownload;
    }
    return kContinue;
}

TextId Detail(Item item)
{
    switch (item)
    {
        case Item::Launcher: return kLauncherDetail;
        case Item::Restart: return kRestartDetail;
        case Item::PowerOff: return kOffDetail;
        case Item::Download: return kDownloadDetail;
        case Item::Continue: break;
    }
    return kTextCount;
}

// The Launcher's Cardenza geometry (203-launcher src/display.cpp, menuFrame and status, narrow screens):
// a black rail 24 pixels wide with the page's title turned, white beside it, four rows of 32 pixels from
// y 4, the selected one a near-black block with white text and its detail under it.
constexpr int kRail = 24;
constexpr int kTop = 4;
constexpr int kStep = (kHeight - kTop) / kVisibleRows;
constexpr int kTextX = kRail + 12;
constexpr int kTextWidth = (kWidth - kRail - 28) / 6 * 6;
constexpr uint8_t kSelectedGrey = 20;

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
    if (nowMs - since_ < kRepeatDelayMs || nowMs - last_ < kRepeatMs) return Key::None;
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

int TextWidth(Face face, const char* text)
{
    int width = 0;
    std::size_t i = 0;
    for (uint32_t cp; (cp = NextCodePoint(text, i)) != 0;) width += Resolve(face, cp).advance;
    return width;
}

bool HasGlyphs(Face face, const char* text)
{
    std::size_t i = 0;
    for (uint32_t cp; (cp = NextCodePoint(text, i)) != 0;)
    {
        const Resolved r = Resolve(face, cp);
        if (r.kind == Glyph::Font5x7 && r.cp != cp) return false;  // drawn as '?'
    }
    return true;
}

void Paint(const View& view, int y, int rows, uint16_t* band)
{
    const int language = std::clamp(view.language, 0, kLanguageCount - 1);
    const Face face = kFaces[language];
    const char* const* texts = kTexts[language];
    Canvas canvas{band, y, rows};

    canvas.Fill(0, 0, kRail, kHeight, 0);
    canvas.Fill(kRail, 0, kWidth - kRail, kHeight, 255);
    DrawText(&canvas, face, texts[kTitle], 8, 8, 240, kHeight - 36, true);

    const int height = TextHeight(face), inkTop = TextTop(face);
    if (view.releasePrompt)
    {
        const char* text = texts[kRelease];
        const int width = std::min(TextWidth(face, text), kWidth - kRail - 24);
        const int ink = (kHeight - height) / 2;
        DrawText(&canvas, face, text, kRail + (kWidth - kRail - width) / 2, ink - inkTop, 0, kWidth - kRail - 24);
        return;
    }

    State state = view.state;
    KeepInView(state);
    canvas.clipTop = kTop;
    canvas.clipBottom = kHeight;
    canvas.Fill(kRail + 6, kTop + (state.selected - state.scroll) * kStep, kWidth - kRail - 16, kStep, kSelectedGrey);
    const bool cjkLayout = face != Face::Font5x7;
    for (int i = state.scroll; i < std::min(kItemCount, state.scroll + kVisibleRows); ++i)
    {
        const Item item = static_cast<Item>(i);
        const int rowY = kTop + (i - state.scroll) * kStep;
        const bool selected = i == state.selected;
        const char* detail = nullptr;
        Face detailFace = face;
        if (selected)
        {
            if (item == Item::Continue)
            {
                detail = view.firmware != nullptr && view.firmware[0] != 0 ? view.firmware : nullptr;
                detailFace = Face::Font5x7;  // a firmware's name and version are ASCII
            }
            else detail = texts[Detail(item)];
        }
        const int gap = cjkLayout ? 2 : 5;
        const int detailHeight = detail ? height + gap : 0;
        const int labelInk = rowY + (kStep - height - detailHeight) / 2;
        DrawText(&canvas, face, texts[Label(item)], kTextX, labelInk - inkTop, selected ? 255 : 0, kTextWidth);
        if (detail)
            DrawText(&canvas, detailFace, detail, kTextX, labelInk + height + gap - inkTop, 170, kTextWidth);
    }
    canvas.clipTop = 0;
    if (kItemCount > kVisibleRows)
    {
        constexpr int track = kHeight - 4 - kTop;
        constexpr int thumb = std::max(10, track * kVisibleRows / kItemCount);
        const int thumbY = kTop + ((track - thumb) * state.scroll + (kItemCount - kVisibleRows) / 2) /
                                      (kItemCount - kVisibleRows);
        canvas.Fill(236, kTop, 2, track, 218);
        canvas.Fill(236, thumbY, 2, thumb, 80);
    }
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
