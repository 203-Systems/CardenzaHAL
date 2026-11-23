// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/lcd.cpp at 4409843d25be.
// Do not edit: change it in the Launcher and copy it again.
// Included inside namespace CardenzaHAL::SystemMenu::launcher (see the files that include it).

void Lcd::greyRect(int x, int y, int w, int h, uint8_t g) {
  if (!pixels_)
    return;
  int left = std::max(x, x0_), right = std::min(x + w, x1_);
  if (right <= left)
    return;
  for (int row = std::max(y, y0_); row < std::min(y + h, y1_); ++row)
    memset(pixels_ + row * width() + left, g, right - left);
}
void Lcd::text(const String &s, int x, int y, int scale, uint8_t g, uint8_t opacity) {
  if (!pixels_ || !opacity) return;
  textInk(s, scale, [&](int dx, int dy, int w, int h) {
    const int left = std::max(x0_, x + dx), right = std::min(x1_, x + dx + w);
    if (right <= left) return;
    for (int py = std::max(y0_, y + dy); py < std::min(y1_, y + dy + h); ++py) {
      if (opacity == 255) {
        memset(pixels_ + py * width() + left, g, right - left);
        continue;
      }
      for (int px = left; px < right; ++px) {
        auto &background = pixels_[py * width() + px];
        background = (unsigned(background) * (255 - opacity) + unsigned(g) * opacity + 127) / 255;
      }
    }
  });
}
void Lcd::textVertical(const String &s, int x, int y, uint8_t g, int scale) {
  // Latin text turns sideways (reading downwards); CJK glyphs stand upright,
  // one under the other, centred on the same column, as in CJK vertical text.
  // 5x7 glyphs among Latin-face text sit on its baseline, as in text().
  const int shift = i18nLatinLine(s, 0) ? latinBaseline(scale) - 7 * scale : 0;
  TextWalk walk(s);
  for (uint32_t c; walk.next(c);) {
    const bool cjkChar = c >= 0x80 && cjkHasGlyph(c);
    LatinGlyph latin;
    int size, m;
    if ((walk.latin || (c >= 0x80 && !cjkChar)) && latinGlyphAt(c, scale, latin, size, m)) {
      // Turned like the 5x7 font: a row Y below the text origin lands at x + 6 * scale - Y.
      const int baseline = latinBaseline(scale);
      for (int r = 0; r < latin.height; ++r)
        for (int col = 0; col < latin.width; ++col)
          if (latin.ink(r, col))
            greyRect(x + 6 * scale - (baseline + (latin.top + r) * m), y + (latin.left + col) * m, m, m, g);
      y += latin.advance * m;
      continue;
    }
    CjkGlyph glyph;
    const CjkScale cjk = cjkScale(scale);
    if (cjkChar && cjkGlyph(c, cjk.size, glyph)) {
      const int m = cjk.multiplier, left = x + (7 * scale - cjk.em()) / 2;
      // Marks that run along the line (the long vowel, dashes, brackets)
      // turn with it, as in vertical CJK text.
      const bool turn = c == 0x30fc || c == 0x2026 || c == 0x2014 || c == 0xff5e || c == 0xff08 ||
                        c == 0xff09 || c == 0x300c || c == 0x300d || c == 0x3010 || c == 0x3011;
      for (int r = 0; r < glyph.em; ++r)
        for (int col = 0; col < glyph.em; ++col)
          if (glyph.ink(r, col)) {
            const int px = turn ? glyph.em - 1 - r : col, py = turn ? col : r;
            greyRect(left + px * m, y + py * m, m, m, g);
          }
      y += cjk.em();
      continue;
    }
    const auto &rows = kFont5x7[c < 128 ? c : '?'];
    const int glyphX = x - shift - (c < 128 ? font5x7YOffset(char(c)) : 0) * scale;
    for (int r = 0; r < 7; ++r)
      for (int col = 0; col < 5; ++col)
        if (rows[r] & (1 << (4 - col)))
          greyRect(glyphX + (6 - r) * scale, y + col * scale, scale, scale, g);
    y += 6 * scale;
  }
}
