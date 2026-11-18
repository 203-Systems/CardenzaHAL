// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/text_ink.h at 6c318426bec3.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include "font5x7.h"
#include "i18n.h"
#include "text.h"
#include <algorithm>

namespace CardenzaHAL::SystemMenu::launcher {

// Calls span(x, y, w, h) for each run of ink in the text, relative to its
// origin: 5x7 glyphs for ASCII, CJK glyphs centred on the 5x7 capitals,
// Latin-face glyphs (kLatinFaceMark) on their own baseline.
template <class Span> inline void textInk(const String &s, int scale, Span &&span) {
  int x = 0, y = 0;
  bool latinLine = false; // the line had Latin-face glyphs, which need more room
  // On a line with Latin-face text, 5x7 glyphs sit on its baseline.
  const int lift = latinBaseline(scale) - 7 * scale;
  int shift = i18nLatinLine(s, 0) ? lift : 0;
  TextWalk walk(s);
  for (uint32_t c; walk.next(c);) {
    if (c == '\n') {
      x = 0;
      y += latinLine ? std::max(8 * scale, latinHeight(scale) + (scale == 1 ? 1 : 2)) : 8 * scale;
      latinLine = false;
      shift = i18nLatinLine(s, walk.at) ? lift : 0;
      continue;
    }
    const bool cjkChar = c >= 0x80 && cjkHasGlyph(c);
    if (walk.latin || (c >= 0x80 && !cjkChar)) {
      LatinGlyph glyph;
      int size, m;
      if (latinGlyphAt(c, scale, glyph, size, m)) {
        latinLine = true;
        const int left = x + glyph.left * m, top = y + latinBaseline(scale) + glyph.top * m;
        for (int r = 0; r < glyph.height; ++r)
          for (int col = 0; col < glyph.width; ++col)
            if (glyph.ink(r, col)) {
              int end = col;
              while (end + 1 < glyph.width && glyph.ink(r, end + 1)) ++end;
              span(left + col * m, top + r * m, (end - col + 1) * m, m);
              col = end;
            }
        x += glyph.advance * m;
        continue;
      }
    }
    CjkGlyph glyph;
    const CjkScale cjk = cjkScale(scale);
    // Titles have their own 24 px glyphs; other text doubles the 12 px ones.
    const bool title = cjkChar && cjk.em() == 24 && cjkGlyph(c, 24, glyph);
    if (title || (cjkChar && cjkGlyph(c, cjk.size, glyph))) {
      const int m = title ? 1 : cjk.multiplier, top = y + cjkTop(scale);
      for (int r = 0; r < glyph.em; ++r)
        for (int col = 0; col < glyph.em; ++col)
          if (glyph.ink(r, col)) {
            int end = col;
            while (end + 1 < glyph.em && glyph.ink(r, end + 1)) ++end;
            span(x + col * m, top + r * m, (end - col + 1) * m, m);
            col = end;
          }
      x += cjk.em();
      continue;
    }
    const auto &rows = kFont5x7[c < 128 ? c : '?'];
    const int glyphY = y + shift + (c < 128 ? font5x7YOffset(char(c)) : 0) * scale;
    for (int r = 0; r < 7; ++r)
      for (int col = 0; col < 5; ++col)
        if (rows[r] & (1 << (4 - col)))
          span(x + col * scale, glyphY + r * scale, scale, scale);
    x += 6 * scale;
  }
}

}  // namespace CardenzaHAL::SystemMenu::launcher
