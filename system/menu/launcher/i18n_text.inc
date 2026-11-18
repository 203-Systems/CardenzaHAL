// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and
// NOTICE.md there) by tools/launcher_ui.py: src/platform/i18n.cpp at 6c318426bec3.
// Do not edit: change it in the Launcher and copy it again.
// Included inside namespace CardenzaHAL::SystemMenu::launcher (see the files that include it).

Language i18nLanguage() { return current; }
void i18nSetLanguage(Language language) { current = language; }
Language i18nLanguageFromCode(uint8_t code) {
  switch (code) {
  case 1:
  case 2: return Language::SimplifiedChinese; // 2: Traditional Chinese, dropped
  case 3: return Language::Japanese;
  case 4: return Language::Indonesian;
  case 5: return Language::Vietnamese;
  default: return Language::English;
  }
}
TextFace i18nFace(Language language) {
  switch (language) {
  case Language::SimplifiedChinese:
  case Language::Japanese: return TextFace::Cjk;
  case Language::Vietnamese: return TextFace::Latin;
  default: return TextFace::Font5x7;
  }
}
String i18nPlain(const String &s) {
  String out;
  for (const char c : s)
    if (!i18nIsMark(c)) out += c;
  return out;
}
bool i18nLatinLine(const String &s, size_t at) {
  const size_t end = s.find('\n', at);
  return s.find(kLatinFaceMark, at) < (end == String::npos ? s.size() : end);
}
String i18nTail(const String &s, size_t at) {
  if (at >= s.size()) return String();
  const size_t line = at ? s.rfind('\n', at - 1) : String::npos;
  const size_t from = line == String::npos ? 0 : line + 1;
  // The face at `at` is the last mark's before it on its line.
  bool latin = false;
  for (size_t i = from; i < at; ++i)
    if (i18nIsMark(s[i])) latin = s[i] == kLatinFaceMark;
  return latin && !i18nIsMark(s[at]) ? String(kLatinFaceMark) + s.substring(at) : s.substring(at);
}
uint32_t utf8Next(const char *s, size_t length, size_t &i) {
  const uint8_t c = uint8_t(s[i++]);
  if (c < 0x80) return c;
  int extra = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : -1;
  if (extra < 0) return 0xfffd;
  uint32_t cp = c & (0x3f >> extra);
  for (int k = 0; k < extra; ++k) {
    if (i >= length || (uint8_t(s[i]) & 0xc0) != 0x80) return 0xfffd;
    cp = (cp << 6) | (uint8_t(s[i++]) & 0x3f);
  }
  return cp;
}
size_t utf8Length(const String &s, size_t i) {
  size_t next = i;
  utf8Next(s, next);
  return next - i;
}
int textWidth(const String &s, int scale) {
  int width = 0;
  TextWalk walk(s);
  for (uint32_t cp; walk.next(cp);)
    if (cp != '\n') width += textAdvance(cp, scale, walk.latin);
  return width;
}
bool textDrawable(const String &s) {
  TextWalk walk(s);
  for (uint32_t cp; walk.next(cp);)
    if (cp >= 0x80 && !cjkHasGlyph(cp) && !latinHasGlyph(cp)) return false;
  return true;
}
bool textHasWide(const String &s) {
  TextWalk walk(s);
  for (uint32_t cp; walk.next(cp);) {
    if (cp == ' ' || cp == '\n') continue;
    if (walk.latin ? latinHasGlyph(cp) : cp >= 0x80 && (cjkHasGlyph(cp) || latinHasGlyph(cp))) return true;
  }
  return false;
}
