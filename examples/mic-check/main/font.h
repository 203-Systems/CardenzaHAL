#pragma once

// Anti-aliased bitmap text for the 240 x 135 screen. The glyphs (font_data.inc) are rasterized from HarmonyOS
// Sans SC (licenses/HarmonyOS-Sans-LICENSE.txt in the HAL) by tools/make_font.py, only the characters this
// firmware shows. Pure (no ESP-IDF).

#include <cstdint>

namespace MicCheck {

struct Glyph
{
    uint32_t code;    // Unicode code point
    int8_t left;      // from the pen to the bitmap's first column
    int8_t top;       // from the line's top to the bitmap's first row
    uint8_t width;
    uint8_t height;
    uint8_t advance;
    uint32_t offset;  // into Font::alpha: 4 bits a pixel, two pixels a byte, rows packed one after another
};

struct Font
{
    const Glyph* glyphs;  // sorted by code
    uint16_t count;
    uint8_t height;       // the line: the tallest glyph's top to the lowest one's bottom
    const uint8_t* alpha;
};

extern const Font kTiny;    // 12 px: the table
extern const Font kSmall;   // 14 px: hints, labels
extern const Font kMedium;  // 18 px: the distance, the result's values
extern const Font kLarge;   // 26 px: what to do
extern const Font kDigits;  // 52 px: the countdown (digits only)

const Glyph* FindGlyph(const Font& font, uint32_t code);
int TextWidth(const Font& font, const char* utf8);
// Draws utf8 with its line's top at y, from x; returns the x after it. A character the font lacks is drawn
// as a box and counted (MissingGlyphs: the host test fails on any).
int DrawText(uint16_t* frame, const Font& font, int x, int y, const char* utf8, uint16_t colour);
int MissingGlyphs();

}  // namespace MicCheck
