#include "font.h"

#include "canvas.h"

namespace MicCheck {

#include "font_data.inc"

namespace {

int g_missing = 0;

// The next code point of a UTF-8 string; advances text. Malformed bytes read as U+FFFD.
uint32_t NextCode(const char*& text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    uint32_t code = bytes[0];
    int extra = 0;
    if (code >= 0xF0) { code &= 0x07; extra = 3; }
    else if (code >= 0xE0) { code &= 0x0F; extra = 2; }
    else if (code >= 0xC0) { code &= 0x1F; extra = 1; }
    else if (code >= 0x80) { ++text; return 0xFFFD; }
    ++text;
    for (int index = 0; index < extra; ++index)
    {
        const uint8_t next = static_cast<uint8_t>(*text);
        if ((next & 0xC0) != 0x80) return 0xFFFD;
        code = code << 6 | (next & 0x3F);
        ++text;
    }
    return code;
}

}  // namespace

const Glyph* FindGlyph(const Font& font, uint32_t code)
{
    int low = 0;
    int high = font.count - 1;
    while (low <= high)
    {
        const int middle = (low + high) / 2;
        const uint32_t at = font.glyphs[middle].code;
        if (at == code) return &font.glyphs[middle];
        if (at < code) low = middle + 1;
        else high = middle - 1;
    }
    return nullptr;
}

int TextWidth(const Font& font, const char* utf8)
{
    int width = 0;
    while (*utf8 != 0)
    {
        const Glyph* glyph = FindGlyph(font, NextCode(utf8));
        width += glyph != nullptr ? glyph->advance : font.height / 2;
    }
    return width;
}

int DrawText(uint16_t* frame, const Font& font, int x, int y, const char* utf8, uint16_t colour)
{
    while (*utf8 != 0)
    {
        const uint32_t code = NextCode(utf8);
        const Glyph* glyph = FindGlyph(font, code);
        if (glyph == nullptr)
        {
            ++g_missing;
            Outline(frame, x + 1, y + 2, font.height / 2 - 2, font.height - 4, colour);
            x += font.height / 2;
            continue;
        }
        uint32_t nibble = glyph->offset * 2;
        for (int row = 0; row < glyph->height; ++row)
            for (int column = 0; column < glyph->width; ++column, ++nibble)
            {
                const uint8_t byte = font.alpha[nibble / 2];
                const int alpha = (nibble & 1) != 0 ? byte >> 4 : byte & 0x0F;
                if (alpha != 0) BlendPixel(frame, x + glyph->left + column, y + glyph->top + row, colour, alpha);
            }
        x += glyph->advance;
    }
    return x;
}

int MissingGlyphs() { return g_missing; }

}  // namespace MicCheck
