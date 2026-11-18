// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/i18n.h at 264061de6d32.
// Do not edit: change it in the Launcher and copy it again.
#pragma once

#include "text.h"
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace CardenzaHAL::SystemMenu::launcher {

// Languages and the text they are drawn with.
//
// Every menu, prompt and message goes through T() with its English text,
// which is the key. tools/i18n.py collects the keys from the sources (T, TF and N_),
// takes the translations from i18n/strings.json and writes the tables and the
// glyphs they need into src/platform/i18n_data.inc. English and Indonesian
// keep the 5x7 bitmap font. Chinese and Japanese draw their non-ASCII
// characters with HarmonyOS Sans glyphs rasterized 1-bit at 12 px (text scale
// 1) and 16 px (scale 2). Vietnamese text is drawn whole in the Latin face,
// proportional HarmonyOS Sans glyphs at the same sizes (kLatinFaceMark).

// The stored value (NVS launcher/lang) is the enum's: never renumber or reuse
// one. 2 was Traditional Chinese (dropped 2026-11): i18nLanguageFromCode
// reads it as Simplified Chinese.
enum class Language : uint8_t { English = 0, SimplifiedChinese = 1, Japanese = 3, Indonesian = 4, Vietnamese = 5 };
constexpr int kLanguageCount = 5;
// The language picker's order: English, the other Latin-script languages by
// their own names, then Chinese and Japanese.
constexpr Language kLanguageOrder[kLanguageCount] = {
    Language::English, Language::Indonesian, Language::Vietnamese, Language::SimplifiedChinese, Language::Japanese};

// How a language's text is drawn: the 5x7 font, CJK glyphs beside 5x7
// ASCII, or entirely in the Latin face.
enum class TextFace : uint8_t { Font5x7, Cjk, Latin };
TextFace i18nFace(Language language);

// FNV-1a of the English key; the tables are sorted by it.
constexpr uint32_t i18nHash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= uint8_t(*s++); h *= 16777619u; }
  return h;
}

Language i18nLanguage();
void i18nSetLanguage(Language language);
// Stored value (NVS launcher/lang) to a language: the retired 2 (Traditional
// Chinese) is Simplified Chinese, unknown values are English.
Language i18nLanguageFromCode(uint8_t code);
// The language's own name ("简体中文", "Tiếng Việt"), drawn the same in every
// language (a Latin-face name carries kLatinFaceMark).
const char *i18nLanguageName(Language language);
// The language's English name as a key ("Japanese"), shown translated with
// tr() under its own name in the picker.
const char *i18nLanguageKey(Language language);
// IETF tag: en, zh-Hans, ja, id, vi.
const char *i18nLanguageTag(Language language);

// The translation of an English key, or the key itself when the language is
// English or the key has no entry. In a Latin-face language the translation
// starts with kLatinFaceMark, and every line of it does.
String i18nLookup(uint32_t hash, const char *english);
// {0}..{9} in the pattern replaced by the arguments.
String i18nFormat(const String &pattern, std::initializer_list<String> args);

// Out of line: hundreds of call sites pass only the pointer.
String T(const char *english);
// A key kept in English (service errors, page titles) and translated where
// it is shown with tr().
#ifndef N_
#define N_(text) text
#endif
String tr(const String &english);
// T() with {0}, {1} filled in.
String TF(const char *english, const String &a);
String TF(const char *english, const String &a, const String &b);
template <class A, class = std::enable_if_t<std::is_arithmetic_v<A>>>
String TF(const char *english, A a) { return TF(english, String(a)); }

// --- Faces -----------------------------------------------------------------
// Text after this byte (Shift Out), to the end of its line, is drawn in the
// Latin face (Vietnamese translations): proportional HarmonyOS Sans glyphs.
// Text without it (names, network names, versions: what was not translated)
// keeps the 5x7 font, and so do the values with letters TF() puts into a
// translation (names, codes, passwords: unambiguous in 5x7; numbers take the
// face): they come between kLatinFaceEnd (Shift In) and a new
// kLatinFaceMark. 5x7 glyphs on a line with Latin-face text sit on its
// baseline. Only drawing
// and measuring read the marks; they have no width.
constexpr char kLatinFaceMark = '\x0e';
constexpr char kLatinFaceEnd = '\x0f';
constexpr bool i18nIsMark(char c) { return c == kLatinFaceMark || c == kLatinFaceEnd; }
// True when the line of s that starts at byte at has Latin-face text.
bool i18nLatinLine(const String &s, size_t at);
// The text without face marks, to compare or store.
String i18nPlain(const String &s);
// Lower case of ASCII and of the Latin letters the languages use (Vietnamese
// Đ, Ă, Ơ, Ư and the letters with tone marks): "Đặt lại" -> "đặt lại".
String i18nLower(const String &s);
// s from byte at on, starting in the face s is drawn in at that point (the
// second line of a wrapped label keeps the Latin face).
String i18nTail(const String &s, size_t at);

// --- UTF-8 ---------------------------------------------------------------
// Decodes one code point at s[i] and advances i; malformed bytes give 0xFFFD.
uint32_t utf8Next(const char *s, size_t length, size_t &i);
inline uint32_t utf8Next(const String &s, size_t &i) { return utf8Next(s.data(), s.size(), i); }
// Byte length of the code point that starts at s[i] (1 for malformed bytes).
size_t utf8Length(const String &s, size_t i);

// The code points of a text with the face each is drawn in: kLatinFaceMark
// turns the Latin face on, kLatinFaceEnd and a line break turn it off.
// Marks are skipped.
struct TextWalk {
  const String &text;
  size_t at = 0;
  bool latin = false;
  explicit TextWalk(const String &s, size_t from = 0) : text(s), at(from) {}
  // The next code point; false at the end. start is its first byte.
  bool next(uint32_t &cp, size_t *start = nullptr) {
    while (at < text.size()) {
      if (start) *start = at;
      cp = utf8Next(text, at);
      if (cp == uint8_t(kLatinFaceMark) || cp == uint8_t(kLatinFaceEnd)) {
        latin = cp == uint8_t(kLatinFaceMark);
        continue;
      }
      if (cp == '\n') latin = false;
      return true;
    }
    return false;
  }
};

// --- Glyphs ----------------------------------------------------------------
// A glyph from the CJK tables, unpacked into rows (bit 31 is the left pixel)
// inside its em box (12, 16 or 24 pixels square).
struct CjkGlyph {
  uint32_t rows[24];
  uint8_t em = 0;
  bool ink(int row, int col) const { return rows[row] & (0x80000000u >> col); }
};
// The glyph for cp at a native size. 24 px exists only for the characters of
// the home titles. False when missing.
bool cjkGlyph(uint32_t cp, int size, CjkGlyph &glyph);
bool cjkHasGlyph(uint32_t cp);
// The native size and whole-pixel multiplier CJK text uses at a text scale:
// 12 px at scale 1, 16 at 2, 12x2 at 3 (or a 24 px title glyph), 16x2 at 4,
// 12x3 at 5 ...
struct CjkScale {
  int size, multiplier;
  constexpr int em() const { return size * multiplier; }
};
constexpr CjkScale cjkScale(int scale) {
  return scale % 2 == 0 ? CjkScale{16, scale / 2} : CjkScale{12, (scale + 1) / 2};
}
// CJK glyphs centre on the 5x7 capitals: this many pixels below the text
// origin (negative: above it).
constexpr int cjkTop(int scale) { return (7 * scale - cjkScale(scale).em()) / 2; }

// A glyph of the Latin face: its ink box (rows from the top, bit 31 the left
// pixel), placed left of the pen and top of the baseline (negative: above).
struct LatinGlyph {
  uint32_t rows[48];
  int8_t left = 0, top = 0;
  uint8_t width = 0, height = 0, advance = 0;
  bool ink(int row, int col) const { return rows[row] & (0x80000000u >> col); }
};
// The Latin glyph for cp at a native size (12, 16 or 24). False when the
// face has none (the 8 MB board, Cardenza, keeps the letters the
// translations use, and at 24 px the home titles').
bool latinGlyph(uint32_t cp, int size, LatinGlyph &glyph);
bool latinHasGlyph(uint32_t cp);
// The glyph text at a scale draws for cp, with its native size and
// multiplier: the CJK sizes (cjkScale), except 24 px bold at scale 3 (12 px
// doubled where there is no 24 px glyph).
bool latinGlyphAt(uint32_t cp, int scale, LatinGlyph &glyph, int &size, int &multiplier);
// The baseline of Latin text at a text scale, this many pixels below the
// text origin (its capitals centre on the 5x7 capitals).
int latinBaseline(int scale);
// Ink of a line of Latin text with the usual marks, from the text origin:
// where it starts (negative: above) and how tall it is. Capitals with marks
// and stacked tildes reach a little higher.
int latinTop(int scale);
int latinHeight(int scale);
// The highest any Latin glyph reaches above the text origin (negative).
int latinHighest(int scale);
// The topmost row of ink any text reaches at a scale, from the text origin.
int textInkTop(int scale);

// Advance of one code point at a text scale: 6 per scale for ASCII and for
// characters without a glyph (drawn as '?'), the em for CJK glyphs, the
// glyph's own advance in the Latin face (latin: inside a Latin-face span;
// outside one, only characters neither 5x7 nor CJK has use it).
int textAdvance(uint32_t cp, int scale, bool latin = false);
// Width of a line of text, as the 5x7 font's length * 6 * scale for ASCII.
int textWidth(const String &s, int scale);
// True when the text has glyphs taller than the 5x7 font (CJK or Latin face).
bool textHasWide(const String &s);
// False when a character of the text has no glyph in this build's tables
// (it would show as '?'): names the store then draws from a label.
bool textDrawable(const String &s);

}  // namespace CardenzaHAL::SystemMenu::launcher
