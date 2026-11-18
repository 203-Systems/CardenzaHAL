// The glyph lookups of the Launcher's i18n.h over the system menu's glyphs (glyphs.inc, read out of the
// Launcher's tables by tools/launcher_ui.py), and the Launcher's own language state, UTF-8 and text
// measuring (i18n_text.inc). Only text scale 1 (12 px) is there: the menu draws nothing larger.
#include <algorithm>
#include <cstring>

#include "i18n.h"

namespace CardenzaHAL::SystemMenu::launcher {

#include "glyphs.inc"

namespace {

Language current = Language::English;

template <class Bitmap, std::size_t N>
const Bitmap* Find(const Bitmap (&bitmaps)[N], uint32_t cp)
{
    const Bitmap* end = bitmaps + N;
    const Bitmap* at = std::lower_bound(bitmaps, end, cp, [](const Bitmap& b, uint32_t c) { return b.cp < c; });
    return at != end && at->cp == cp ? at : nullptr;
}

}  // namespace

#include "i18n_text.inc"

bool cjkGlyph(uint32_t cp, int size, CjkGlyph& glyph)
{
    const CjkBitmap* bitmap = size == 12 ? Find(kCjkBitmaps, cp) : nullptr;
    if (bitmap == nullptr) return false;
    std::memset(glyph.rows, 0, sizeof(glyph.rows));
    glyph.em = 12;
    for (int row = 0; row < 12; ++row) glyph.rows[row] = uint32_t(bitmap->rows[row]) << 20;
    return true;
}

bool cjkHasGlyph(uint32_t cp)
{
    return cp >= 0x80 && Find(kCjkBitmaps, cp) != nullptr;
}

bool latinGlyph(uint32_t cp, int size, LatinGlyph& glyph)
{
    const LatinBitmap* bitmap = size == 12 ? Find(kLatinBitmaps, cp) : nullptr;
    if (bitmap == nullptr) return false;
    std::memset(glyph.rows, 0, sizeof(glyph.rows));
    glyph.left = bitmap->left;
    glyph.top = bitmap->top;
    glyph.width = bitmap->width;
    glyph.height = bitmap->height;
    glyph.advance = bitmap->advance;
    for (int row = 0; row < bitmap->height; ++row) glyph.rows[row] = uint32_t(bitmap->rows[row]) << 16;
    return true;
}

bool latinHasGlyph(uint32_t cp)
{
    return Find(kLatinBitmaps, cp) != nullptr;
}

bool latinGlyphAt(uint32_t cp, int scale, LatinGlyph& glyph, int& size, int& multiplier)
{
    if (scale != 1) return false;
    size = 12;
    multiplier = 1;
    return latinGlyph(cp, 12, glyph);
}

int latinBaseline(int scale) { return kLatinBaseline1 * scale; }
int latinTop(int scale) { return kLatinTop1 * scale; }
int latinHeight(int scale) { return kLatinHeight1 * scale; }
int latinHighest(int scale) { return kLatinHighest1 * scale; }
int textInkTop(int scale) { return std::min({0, cjkTop(scale), latinHighest(scale)}); }

// As the Launcher's (i18n.cpp there), with the advance from the glyph rather than its packed record.
int textAdvance(uint32_t cp, int scale, bool latin)
{
    const bool cjk = cp >= 0x80 && cjkHasGlyph(cp);
    if (latin || (cp >= 0x80 && !cjk))
    {
        LatinGlyph glyph;
        int size, multiplier;
        if (latinGlyphAt(cp, scale, glyph, size, multiplier)) return glyph.advance * multiplier;
    }
    return cjk ? cjkScale(scale).em() : 6 * scale;
}

}  // namespace CardenzaHAL::SystemMenu::launcher
