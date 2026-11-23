// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/ui_layout.h at 4409843d25be.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include "font5x7.h"
#include "i18n.h"
#include "text.h"
#include <vector>

namespace CardenzaHAL::SystemMenu::launcher {

struct UiTextBounds {
  int left = 0, top = 0, right = 0, bottom = 0;
};
// Ink bounds of a line at scale 1; CJK and Latin-face glyphs reach above the origin.
inline UiTextBounds uiTextBounds(const String &text) {
  UiTextBounds bounds{textWidth(text, 1), 7, -1, -1};
  int x = 0;
  auto ink = [&](int px, int py) {
    bounds.left = std::min(bounds.left, px);
    bounds.right = std::max(bounds.right, px);
    bounds.top = std::min(bounds.top, py);
    bounds.bottom = std::max(bounds.bottom, py);
  };
  const bool latinLine = i18nLatinLine(text, 0);
  TextWalk walk(text);
  for (uint32_t c; walk.next(c);) {
    const bool cjkChar = c >= 0x80 && cjkHasGlyph(c);
    LatinGlyph latin;
    if ((walk.latin || (c >= 0x80 && !cjkChar)) && latinGlyph(c, 12, latin)) {
      const int baseline = latinBaseline(1);
      for (int row = 0; row < latin.height; ++row)
        for (int col = 0; col < latin.width; ++col)
          if (latin.ink(row, col)) ink(x + latin.left + col, baseline + latin.top + row);
      x += latin.advance;
      continue;
    }
    CjkGlyph glyph;
    if (cjkChar && cjkGlyph(c, 12, glyph)) {
      for (int row = 0; row < glyph.em; ++row)
        for (int col = 0; col < glyph.em; ++col)
          if (glyph.ink(row, col)) ink(x + col, cjkTop(1) + row);
      x += glyph.em;
      continue;
    }
    const auto &rows = kFont5x7[c < 128 ? c : '?'];
    const int offset = (c < 128 ? font5x7YOffset(char(c)) : 0) + (latinLine ? latinBaseline(1) - 7 : 0);
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if (rows[row] & (16 >> col)) ink(x + col, row + offset);
    x += 6;
  }
  return bounds.right < 0 ? UiTextBounds{} : bounds;
}

// Characters that never start a line (closing punctuation) or end one.
inline bool uiNoLineStart(uint32_t c) {
  switch (c) {
  case ',': case '.': case ':': case ';': case '!': case '?': case ')':
  case 0x3001: case 0x3002: case 0xff0c: case 0xff1a: case 0xff1b: case 0xff01: case 0xff1f:
  case 0xff09: case 0x300d: case 0x300f: case 0x3011: case 0x2026: case 0x30fc:
  case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083:
  case 0x3085: case 0x3087: case 0x30a1: case 0x30a3: case 0x30a5: case 0x30a7: case 0x30a9:
  case 0x30c3: case 0x30e3: case 0x30e5: case 0x30e7:
    return true;
  default:
    return false;
  }
}
inline bool uiNoLineEnd(uint32_t c) {
  return c == '(' || c == 0xff08 || c == 0x300c || c == 0x300e || c == 0x3010;
}

// The longest start of text (in bytes) that fits width, and where a line may
// break inside it: after a space, or beside a CJK character (CJK text has no
// spaces). breakAt is 0 when there is no break opportunity.
struct UiLineFit {
  size_t fits = 0, breakAt = 0, resumeAt = 0;
};
inline UiLineFit uiFitLine(const String &text, int width, int scale) {
  UiLineFit fit;
  int x = 0;
  uint32_t previous = 0;
  bool first = true;
  TextWalk walk(text);
  size_t start = 0;
  for (uint32_t c; walk.next(c, &start);) {
    if (c == '\n') break;
    // Latin-face text breaks at spaces, like English; CJK beside any CJK character.
    const bool wide = c >= 0x80 && cjkHasGlyph(c);
    const bool previousWide = previous >= 0x80 && cjkHasGlyph(previous);
    if (!first && (wide || previousWide) && c != ' ' && previous != ' ' &&
        !uiNoLineStart(c) && !uiNoLineEnd(previous)) {
      fit.breakAt = start;
      fit.resumeAt = start;
    }
    if (c == ' ' && !first) {
      fit.breakAt = start;
      fit.resumeAt = walk.at;
    }
    x += textAdvance(c, scale, walk.latin);
    if (x > width) return fit;
    fit.fits = walk.at;
    previous = c;
    first = false;
  }
  return fit;
}

// Leading face marks (i18n.h).
inline size_t uiMarks(const String &text) {
  size_t marks = 0;
  while (marks < text.size() && i18nIsMark(text[marks])) ++marks;
  return marks;
}
// Leading and trailing spaces off, face marks at the start kept.
inline void uiTrim(String &text) {
  const size_t marks = uiMarks(text);
  String rest = text.substring(marks);
  rest.trim();
  text = rest.isEmpty() ? rest : text.substring(0, marks) + rest;
}

// One truncation style: "..." at the end, cut at a nearby word boundary
// (within six 5x7 columns). width in pixels at the text scale.
inline String uiFitText(const String &text, int width, int scale) {
  if (width < 0 || textWidth(text, scale) <= width) return text;
  const int dots = 18 * scale;
  if (width <= dots) return text.substring(0, uiFitLine(text, width, scale).fits);
  size_t cut = uiFitLine(text, width - dots, scale).fits;
  const auto space = text.rfind(' ', cut);
  if (space != String::npos && space > 0 &&
      textWidth(i18nTail(text.substring(0, cut), space), scale) <= 6 * 6 * scale)
    cut = space;
  String head = text.substring(0, cut);
  uiTrim(head);
  return head + "...";
}
// The same in 5x7 columns of scale 1 (one column is 6 pixels).
inline String uiEllipsize(const String &text, int columns) { return uiFitText(text, columns * 6, 1); }

// True when the text has a character drawn with a CJK glyph.
inline bool uiHasCjk(const String &text) {
  for (size_t i = 0; i < text.size();) {
    const uint32_t c = utf8Next(text, i);
    if (c >= 0x80 && cjkHasGlyph(c)) return true;
  }
  return false;
}

// Word-wrapped lines; explicit newlines start a new line. The last allowed
// line ends in an ellipsis when text remains. Lines of Latin-face text keep
// the face (i18nTail).
inline std::vector<String> uiWrap(const String &text, int width, int scale, int maxLines) {
  std::vector<String> lines;
  String remaining = text;
  while (!i18nPlain(remaining).isEmpty() && int(lines.size()) < maxLines) {
    const UiLineFit fit = uiFitLine(remaining, width, scale);
    size_t count = fit.fits, resume = fit.fits;
    const size_t next = fit.fits;
    if (next < remaining.size() && remaining[next] == '\n') {
      resume = next + 1;
    } else if (next < remaining.size()) {
      if (remaining[next] == ' ') resume = next + 1;
      else if (fit.breakAt) { count = fit.breakAt; resume = fit.resumeAt; }
      const size_t mark = uiMarks(remaining);
      if (count <= mark) count = resume = mark + utf8Length(remaining, mark); // never an empty line
    }
    String line = remaining.substring(0, count);
    uiTrim(line);
    lines.push_back(line);
    remaining = i18nTail(remaining, resume);
    uiTrim(remaining);
  }
  const String rest = i18nPlain(remaining);
  if (!rest.isEmpty() && !lines.empty()) {
    const bool cjk = uiHasCjk(lines.back()) || uiHasCjk(rest.substring(0, utf8Length(rest, 0)));
    lines.back() = uiFitText(lines.back() + (cjk ? "" : " ") + remaining, width, scale);
  }
  return lines;
}

// Distance between stacked lines that were laid out for the 5x7 font:
// CJK glyphs are taller, so their lines keep two pixels between them; the
// Latin face keeps room for its marks above and descenders below.
inline int uiLineStep(int scale, int englishStep) {
  switch (i18nFace(i18nLanguage())) {
  case TextFace::Cjk: return std::max(englishStep, cjkScale(scale).em() + (scale == 1 ? 1 : 2));
  case TextFace::Latin: return std::max(englishStep, latinHeight(scale) + (scale == 1 ? 1 : 2));
  default: return englishStep;
  }
}

// Fill of the selected row and of focused or pressed buttons, with white text.
constexpr uint8_t kUiSelectedGrey = 20;

// Gap between a square menu label and its description line.
constexpr int kUiDetailGap = 8;

// Square software rows retain 12-pixel outer padding and 6-pixel gaps around
// the actions. Measure the header before placing actions, including wrapped names.
// Heights are of the ink: the 5x7 font by default, uiTextHeight for CJK.
struct UiSoftwarePanel {
  int labelTop, labelHeight, detailY, actionsY, height;
};
inline UiSoftwarePanel uiSoftwarePanel(int lines, bool description, int labelStep = 16,
                                       int labelGlyph = 14, int detailGlyph = 7) {
  const int labelHeight = (lines - 1) * labelStep + labelGlyph;
  const int labelTop = (52 - labelHeight - (description ? detailGlyph + kUiDetailGap : 0)) / 2;
  const int detailY = labelTop + labelHeight + kUiDetailGap;
  const int actionsY = (description ? detailY + detailGlyph : labelTop + labelHeight) + 6;
  return {labelTop, labelHeight, detailY, actionsY, actionsY + 32 + 12};
}

// Text metrics of the language: CJK glyphs and the Latin face are taller
// than the 5x7 font and reach above the text origin. English and Indonesian
// keep the 5x7 values.
inline bool uiCjkLayout() { return i18nFace(i18nLanguage()) != TextFace::Font5x7; }
inline int uiTextHeight(int scale) {
  switch (i18nFace(i18nLanguage())) {
  case TextFace::Cjk: return cjkScale(scale).em();
  case TextFace::Latin: return latinHeight(scale);
  default: return 7 * scale;
  }
}
inline int uiTextTop(int scale) {
  switch (i18nFace(i18nLanguage())) {
  case TextFace::Cjk: return cjkTop(scale);
  case TextFace::Latin: return latinTop(scale);
  default: return 0;
  }
}

}  // namespace CardenzaHAL::SystemMenu::launcher
