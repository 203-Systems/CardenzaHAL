#!/usr/bin/env python3
"""The system menu's copy of the 203 Launcher's drawing code, and the menu's words.

    python3 tools/launcher_ui.py --launcher ~/projects/203-launcher             copy and generate
    python3 tools/launcher_ui.py --launcher ~/projects/203-launcher --check     the copy is current
    python3 tools/launcher_ui.py --launcher ~/projects/203-launcher --compare   frames as the Launcher's code
                                                                                 draws them
    ... --fonts "<folder with HarmonyOS_Sans_SC.ttf>"   glyphs the Launcher's tables lack yet (freetype-py)

The system menu (system/menu.h) is the Launcher's list on the Cardenza, drawn over a firmware. So that the
two are one thing and stay one thing, it runs the Launcher's own code (MIT, 203-launcher LICENSE; the 5x7
font BSD-3-Clause), copied into system/menu/launcher/ unchanged, only wrapped in the namespace
CardenzaHAL::SystemMenu::launcher (a firmware may have a String of its own):

  text.h, font5x7.h, i18n.h, text_ink.h, ui_layout.h, ui_motion.h, power_state.h   whole (src/platform/)
  lcd_draw.inc     Lcd::greyRect, text and textVertical (src/platform/lcd.cpp)
  motion.inc       ease, Tween and ListSlide (src/display.cpp): the lists' easing and the selection bar's
                   slide
  labels.inc       labelLines and barPass (src/display.cpp): a row's name on one or two lines, and the
                   passes that draw a row's text white on the selection bar and dark off it
  i18n_text.inc    the language state, UTF-8 and text measuring of src/platform/i18n.cpp

and generates glyphs.inc: the 12 px glyphs the menu's words need, and the 16 px bold ones of the words it
draws at text scale 2 (SCALE2_IDS: the Reset question), read out of the Launcher's own tables (HarmonyOS Sans
rendered by the Launcher's tools/i18n.py; NOTICE.md there) by a small program built against its
src/platform/i18n.cpp, and words.inc (system/menu/strings.json). A character the Launcher's tables lack yet
(a word only the menu uses so far) is rendered by the Launcher's own tools/i18n.py rasterize from the font
file it names (--fonts): the glyph the Launcher's tables will have once it uses the word. What the HAL adds
around them is its own: system/menu/launcher/lcd.h (the canvas, one band of rows at a time), glyphs.cpp (the
glyph lookups over glyphs.inc) and frame.cpp (the Launcher's menuFrame, status rail and drawDialog on the
Cardenza, ported line by line, and its Backlight page and About rows, which it draws inline).

--check fails when the copy differs from what this checkout would give. --compare draws the menu in
every language and position with the Launcher's real menuFrame, drawDialog and status() (as its
tests/ui_render.cpp does; with the glyphs rendered for it, as above) and with the HAL's frame.cpp, and fails
on any pixel that differs: the still frames (STILLS: the main list, the Settings page, the languages, the
Reset question), and the selection bar's slide frame by frame (SLIDES: a move, key repeat, a reversal, a
scroll each way, every 4 ms until it settles). It also rewrites tests/system_menu_frames.inc, the frames'
hashes the host tests check without a Launcher.

Needs a C++17 compiler (CXX, else c++/g++/clang++) and Python 3; no ESP-IDF. --fonts needs freetype-py.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "system" / "menu" / "launcher"
STRINGS = ROOT / "system" / "menu" / "strings.json"
WORDS = ROOT / "system" / "menu" / "words.inc"
FRAMES = ROOT / "tests" / "system_menu_frames.inc"
NAMESPACE = "CardenzaHAL::SystemMenu::launcher"

WHOLE = ["text.h", "font5x7.h", "i18n.h", "text_ink.h", "ui_layout.h", "ui_motion.h", "power_state.h"]
# (output, source, [(start marker, end marker)]): the text from each start up to its end.
PIECES = [
    ("lcd_draw.inc", "src/platform/lcd.cpp", [
        # greyRect alone: colorPicture (the store's covers in colour) follows it there, and the menu draws
        # no pictures.
        ("void Lcd::greyRect(", "void Lcd::colorPicture("),
        ("void Lcd::text(", "void Lcd::print("),
        ("void Lcd::textVertical(", None),
    ]),
    ("motion.inc", "src/display.cpp", [
        ("float ease(float p) {", "bool buttonBackground("),
    ]),
    ("labels.inc", "src/display.cpp", [
        ("struct LabelLines {", "// selected: the row (between two"),
    ]),
    ("i18n_text.inc", "src/platform/i18n.cpp", [
        ("Language i18nLanguage()", "const char *i18nLanguageName("),
        ("String i18nPlain(", "String i18nLower("),
        ("String i18nTail(", "String T(const char *english)"),
        ("uint32_t utf8Next(const char *s", "namespace {\n// 12 and 16 px glyphs"),
        ("int textWidth(", "bool textDrawable("),
        ("bool textDrawable(", None),
        ("bool textHasWide(", None),
    ]),
]

# The only changes to the copied text: (old, new, why), each old found exactly once.
EDITS = {
    "motion.inc": [("float ease(float p) {", "inline float ease(float p) {",
                    "ease is inline (here it is in a header, included by more than one file).")],
    "labels.inc": [("LabelLines labelLines(", "inline LabelLines labelLines(",
                    "labelLines is inline (included by more than one file).")],
}

# The Launcher's languages, in the order of its NVS value launcher/lang (docs/SHARED_SETTINGS.md there):
# 0 English, 1 Simplified Chinese, 2 read as 1, 3 Japanese, 4 Indonesian, 5 Vietnamese.
LANGUAGES = ["en", "zh-Hans", "ja", "id", "vi"]
LANGUAGE_ENUM = {"en": "English", "zh-Hans": "SimplifiedChinese", "ja": "Japanese", "id": "Indonesian",
                 "vi": "Vietnamese"}
LATIN_FACE = {"vi"}  # its translations are drawn whole in the Latin face (the Launcher's kLatinFaceMark)
# The words' ids in strings.json, in the order of words.inc's TextId (every entry but the comment, the
# languages and language_names).
TEXT_IDS = ["title", "continue", "settings", "settings.detail", "launcher", "launcher.detail", "restart",
            "restart.detail", "off", "off.detail", "about", "about.detail", "about.rail", "download",
            "download.detail", "release", "volume", "volume.detail", "volume_limit", "volume_limit.detail",
            "volume_limit.rail", "screen", "screen.rail", "shared.detail", "keys", "keys.detail", "language",
            "webusb", "webusb.detail", "webusb.first", "webusb.always", "webusb.never", "reset", "reset.detail", "reset.question", "reset.explain",
            "reset.button", "cancel", "select_saves", "version", "about.launcher", "about.flash", "about.card",
            "about.free", "none", "english", "chinese", "japanese", "indonesian", "vietnamese",
            # Screenshots (screenshot/screenshot.h): the Settings row and the card the effect shows.
            "shot.upload", "shot.upload.detail", "value.on", "value.off", "shot.saved", "shot.taken", "shot.nocard",
            "shot.failed", "shot.nomemory", "shot.unavailable", "shot.notsaved"]
# A glyph the Launcher's tables do not have yet (a word only the system menu uses; the user's own words,
# 2026-11-23: 恢复默认) is rendered as the Launcher's tools/i18n.py renders its glyphs (its own
# rasterize, imported from the checkout: HarmonyOS_Sans_SC.ttf at 12 px, weight 400) from the font file it
# names (--fonts, NOTICE.md there), so it is the glyph the Launcher will have once it uses the word.
CJK_FONT = "HarmonyOS_Sans_SC.ttf"

# The selection bar's slides --compare draws (the Launcher's ListSlide on the Cardenza: keep, the rows jump
# to a scroll): presses (ms, selected row, scroll), the first placing the bar; a frame every SLIDE_FRAME_MS
# from the first press until SLIDE_SETTLE_MS after the last (the trailing edge's 230 ms and a frame), each
# drawn after the presses due by then, as the Launcher's loopMenu and the system menu's loop draw them.
SLIDE_START = 5000
SLIDE_FRAME_MS = 4
SLIDE_SETTLE_MS = 240
SLIDES = [
    ("move", [(0, 1, 0), (0, 2, 0)]),
    ("repeat", [(0, 1, 0), (0, 2, 0), (40, 3, 0)]),                 # a key repeat in the slide
    ("reverse", [(0, 2, 0), (0, 3, 0), (60, 2, 0)]),                # back the way it came
    ("scroll down", [(0, 2, 0), (0, 3, 0), (48, 4, 1)]),            # past the last row, still moving
    ("scroll up", [(0, 4, 1), (0, 3, 1), (40, 2, 1), (80, 1, 1), (120, 0, 0)]),
]
SLIDE_PRESSES = 8

# The still frames --compare draws in every language, in this order: the main list with each row selected,
# the Settings page with each row selected (SAMPLE_VALUES; the language's own; the browser notice at its default,
# the first time only; Upload screenshots on), the Language page with each selected, the Reset question with each
# button focused, and the browser notice's three choices with each selected. (page, rows): page 0 main, 1 settings,
# 2 language, 3 reset, 4 the browser notice.
STILLS = [(0, 7), (1, 8), (2, 5), (3, 2), (4, 3)]
STILL_PAGE_NAMES = ["main", "settings", "language", "reset", "browser notice"]
# Volume, volume limit, screen brightness, key lights on the Settings page's stills.
SAMPLE_VALUES = [64, 90, 80, 12]
SAMPLE_TEXTS = ["64", "90", "80%", "12"]
LANGUAGE_CODES = [0, 1, 3, 4, 5]

HEADER = ("// Copied from the 203 Launcher (https://github.com/203-Systems/203-launcher, MIT; LICENSE and\n"
          "// NOTICE.md there) by tools/launcher_ui.py: {source} at {commit}.\n"
          "// Do not edit: change it in the Launcher and copy it again.\n")


def git_commit(launcher):
    try:
        out = subprocess.run(["git", "-C", str(launcher), "log", "-1", "--format=%H"], capture_output=True,
                             text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(launcher), "status", "--porcelain", "--", "src"],
                               capture_output=True, text=True, check=True).stdout.strip()
        return out[:12] + ("+" if dirty else "")
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def wrap(text, source, commit):
    """The file's includes stay at the top, everything else goes into the namespace."""
    includes, body = [], []
    for line in text.splitlines():
        if line.strip() == "#pragma once":
            continue
        (includes if line.startswith("#include") else body).append(line)
    while body and not body[0].strip():
        body.pop(0)
    out = [HEADER.format(source=source, commit=commit).rstrip("\n"), "#pragma once", ""]
    out += includes + ["", f"namespace {NAMESPACE} {{", ""] + body
    out += ["", f"}}  // namespace {NAMESPACE}", ""]
    return "\n".join(out)


def piece(text, start, end, source):
    at = text.find(start)
    if at < 0:
        sys.exit(f"{source}: no '{start}' (the Launcher changed: update PIECES in tools/launcher_ui.py)")
    if end is None:
        # To the end of the function: the first line that is a lone closing brace.
        stop = text.find("\n}\n", at)
        if stop < 0:
            sys.exit(f"{source}: the function at '{start}' has no end")
        return text[at:stop + 3]
    stop = text.find(end, at)
    if stop < 0:
        sys.exit(f"{source}: no '{end}' after '{start}'")
    return text[at:stop]


def copies(launcher, commit):
    files = {}
    for name in WHOLE:
        source = f"src/platform/{name}"
        files[name] = wrap((launcher / source).read_text(encoding="utf-8"), source, commit)
    for name, source, ranges in PIECES:
        text = (launcher / source).read_text(encoding="utf-8")
        parts = "\n".join(piece(text, start, end, source).rstrip() for start, end in ranges) + "\n"
        head = HEADER.format(source=source, commit=commit)
        note = "// Included inside namespace " + NAMESPACE + " (see the files that include it)."
        for old, new, why in EDITS.get(name, []):
            if parts.count(old) != 1:
                sys.exit(f"{source}: '{old}' is not there once (update EDITS in tools/launcher_ui.py)")
            parts = parts.replace(old, new)
            note += f"\n// Changed here: {why}"
        files[name] = head + note + "\n\n" + parts
    return files


# ---- Glyphs and words ----------------------------------------------------------------------------------

PROBE = r'''
#include "platform/i18n.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
// probe cjk SIZE | probe latin SIZE: code points on stdin, a glyph (or M for missing) a line on stdout.
int main(int argc, char** argv) {
  const bool latin = argc > 1 && std::strcmp(argv[1], "latin") == 0;
  const int size = argc > 2 ? std::atoi(argv[2]) : 12, scale = size == 16 ? 2 : 1;
  if (latin) printf("B %d %d %d %d\n", latinBaseline(scale), latinTop(scale), latinHeight(scale), latinHighest(scale));
  unsigned cp;
  while (scanf("%u", &cp) == 1) {
    if (latin) {
      LatinGlyph g;
      if (!latinGlyph(cp, size, g)) { printf("M %u\n", cp); continue; }
      printf("L %u %d %d %d %d %d", cp, g.left, g.top, g.width, g.height, g.advance);
      for (int r = 0; r < g.height; ++r) printf(" %08x", (unsigned)g.rows[r]);
      printf("\n");
      continue;
    }
    CjkGlyph g;
    if (!cjkGlyph(cp, size, g)) { printf("M %u\n", cp); continue; }
    printf("C %u %d", cp, g.em);
    for (int r = 0; r < g.em; ++r) printf(" %08x", (unsigned)g.rows[r]);
    printf("\n");
  }
}
'''


def compiler():
    found = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not found:
        sys.exit("a C++17 compiler is needed (set CXX)")
    return found


def build_probe(launcher, work):
    source = work / "probe.cpp"
    source.write_text(PROBE)
    binary = work / "probe"
    # PART_08MB: the Cardenza's build of the tables (the compact ones for its Launcher partition of
    # 0x150000; the name is older than its 16 MB of flash).
    subprocess.run([compiler(), "-std=c++17", "-O1", "-DPART_08MB=1", "-I", str(launcher / "src"),
                    str(source), str(launcher / "src" / "platform" / "i18n.cpp"), "-o", str(binary)], check=True)
    return binary


def ask(binary, codepoints, latin, size=12):
    args = [str(binary), "latin" if latin else "cjk", str(size)]
    out = subprocess.run(args, input="\n".join(str(c) for c in codepoints), capture_output=True,
                         text=True, check=True).stdout
    return out.splitlines()


def c_string(text):
    out = '"'
    for byte in text.encode("utf-8"):
        out += chr(byte) if 32 <= byte < 127 and chr(byte) not in '"\\?' else f"\\{byte:03o}"
    return out + '"'


def load_words():
    strings = json.loads(STRINGS.read_text(encoding="utf-8"))
    extra = set(strings) - set(TEXT_IDS) - {"_comment", "languages", "language_names"}
    if extra:
        sys.exit("strings.json: not in TEXT_IDS (tools/launcher_ui.py): " + ", ".join(sorted(extra)))
    if len(strings.get("language_names", [])) != len(LANGUAGES):
        sys.exit("strings.json: language_names needs one name per language")
    for key in TEXT_IDS:
        for language in LANGUAGES:
            if not strings.get(key, {}).get(language):
                sys.exit(f"strings.json: {key} has no {language}")
            if any(ord(c) < 32 for c in strings[key][language]):
                sys.exit(f"strings.json: {key} ({language}) has control characters")
    return strings


def shown(strings, key, language):
    """The text as the Launcher's T() gives it: a Latin-face translation starts with its mark."""
    return ("\x0e" if language in LATIN_FACE else "") + strings[key][language]


def language_name_faces(strings):
    """Each language's own name with the language whose face draws it (its picker draws each in its own)."""
    return list(zip(LANGUAGES, strings["language_names"]))


def render_missing(launcher, fonts, chars, size=12):
    """CJK glyphs the Launcher's tables lack, rendered by the Launcher's own tools/i18n.py (rasterize)."""
    if fonts is None:
        sys.exit("the Launcher's tables have no glyph for: " + "".join(chars) + " (pass --fonts with the folder "
                 f"of {CJK_FONT}, the font the Launcher's NOTICE.md names, to render them as its tools/i18n.py does; "
                 "needs freetype-py)")
    import importlib.util
    spec = importlib.util.spec_from_file_location("launcher_i18n", launcher / "tools" / "i18n.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    font = pathlib.Path(fonts).expanduser() / CJK_FONT
    if not font.exists():
        sys.exit(f"no {font}")
    glyphs, missing = module.rasterize(font, set(chars), size, module.SIZES[size])
    if missing:
        sys.exit("HarmonyOS Sans SC has no glyph for: " + "".join(missing) + " (change the words)")
    return glyphs


# Texts the menu draws at text scale 2 (16 px glyphs): the Reset dialog's question and buttons.
SCALE2_IDS = ["reset.question", "reset.button", "cancel"]
# Characters the Latin face is asked for besides the words' (versions, sizes and values written in a Latin-face
# line), when the Launcher's tables have them: as the Launcher draws them.
LATIN_EXTRA = "0123456789 .,:;%+-/()"


# The glyphs rendered for characters the Launcher's tables lack ({size: {char: rows}}), for --compare.
RENDERED = {}


def cjk_chars(strings, ids, names=False):
    chars = {ord(c) for key in ids for language in LANGUAGES if language not in LATIN_FACE
             for c in strings[key][language] if ord(c) >= 0x80}
    if names:
        chars |= {ord(c) for language, name in language_name_faces(strings) if language not in LATIN_FACE
                  for c in name if ord(c) >= 0x80}
    return sorted(chars)


def latin_chars(strings, ids, names=False):
    chars = {ord(c) for key in ids for language in LATIN_FACE for c in strings[key][language]}
    if names:
        chars |= {ord(c) for language, name in language_name_faces(strings) if language in LATIN_FACE for c in name}
    return chars


def cjk_table(name, lines, size, rendered):
    words = 32 if size > 16 else 16
    out = [f"// {size} by {size} pixels, a row a value, the leftmost pixel in bit {size - 1}. Sorted by code point."]
    if rendered:
        out.append("// Not in the Launcher's tables yet, rendered by its tools/i18n.py (rasterize, " + CJK_FONT +
                   f" {size} px): " + "".join(sorted(rendered)) + ".")
    out.append(f"constexpr CjkBitmap{size} kCjkBitmaps{size}[] = {{")
    for line in lines:
        parts = line.split()
        cp, em = int(parts[1]), int(parts[2])
        assert em == size and parts[0] in ("C", "R"), line
        rows = [int(v, 16) >> (32 - size) for v in parts[3:3 + size]]
        out.append(f"    {{0x{cp:04X}, {{" + ", ".join(f"0x{r:0{(size + 3) // 4}X}" for r in rows) + f"}}}},  // {chr(cp)}")
    out.append("};")
    return out


def latin_table(lines, size):
    base = [l for l in lines if l.startswith("B ")][0].split()
    scale = 2 if size == 16 else 1
    out = [f"// The Latin face at {size} px (text scale {scale}): ink `left` pixels after the pen and `top` rows from the",
           "// baseline (negative: above); rows with the leftmost pixel in bit 15. Sorted by code point.",
           f"constexpr int kLatinBaseline{scale} = {base[1]}, kLatinTop{scale} = {base[2]}, kLatinHeight{scale} = {base[3]}, "
           f"kLatinHighest{scale} = {base[4]};",
           f"constexpr LatinBitmap kLatinBitmaps{size}[] = {{"]
    for line in lines:
        if not line.startswith("L "):
            continue
        parts = line.split()
        cp, left, top, width, height, advance = (int(v) for v in parts[1:7])
        assert width <= 16 and height <= 20, line
        rows = [int(v, 16) >> 16 for v in parts[7:7 + height]]
        out.append(f"    {{0x{cp:04X}, {left}, {top}, {width}, {height}, {advance}, {{" +
                   ", ".join(f"0x{r:04X}" for r in rows) + f"}}}},  // {chr(cp)}")
    out.append("};")
    return out


def generate_glyphs(launcher, strings, commit, fonts=None):
    cjk12 = cjk_chars(strings, TEXT_IDS, names=True)
    cjk16 = cjk_chars(strings, SCALE2_IDS)
    latin12_words = latin_chars(strings, TEXT_IDS, names=True)
    latin16_words = latin_chars(strings, SCALE2_IDS)
    extra = {ord(c) for c in LATIN_EXTRA}
    with tempfile.TemporaryDirectory() as tmp:
        binary = build_probe(launcher, pathlib.Path(tmp))
        lines = {
            ("cjk", 12): ask(binary, cjk12, False, 12),
            ("cjk", 16): ask(binary, cjk16, False, 16),
            ("latin", 12): ask(binary, sorted(latin12_words | extra), True, 12),
            ("latin", 16): ask(binary, sorted(latin16_words | extra), True, 16),
        }
    for size, words in ((12, latin12_words), (16, latin16_words)):
        missing = [chr(int(l.split()[1])) for l in lines[("latin", size)]
                   if l.startswith("M ") and int(l.split()[1]) in words]
        if missing:
            sys.exit(f"the Launcher has no {size} px Latin glyph for: " + "".join(missing) + " (change the words)")
        lines[("latin", size)] = [l for l in lines[("latin", size)] if not l.startswith("M ")]
    rendered = RENDERED
    rendered.clear()
    for size in (12, 16):
        missing = [chr(int(l.split()[1])) for l in lines[("cjk", size)] if l.startswith("M ")]
        if not missing:
            continue
        rendered[size] = render_missing(launcher, fonts, missing, size)
        lines[("cjk", size)] = [
            l if not l.startswith("M ") else
            f"R {l.split()[1]} {size} " + " ".join(f"{r << (32 - size):08x}" for r in rendered[size][chr(int(l.split()[1]))])
            for l in lines[("cjk", size)]]

    out = [f"// Generated by tools/launcher_ui.py from the 203 Launcher's glyph tables at {commit}",
           "// (src/platform/i18n_data.inc there: HarmonyOS Sans at 12 px and 16 px bold, made by its tools/i18n.py;",
           "// NOTICE.md there and licenses/HarmonyOS-Sans-LICENSE.txt here): only the characters of",
           "// system/menu/strings.json (16 px: the words drawn at text scale 2). Do not edit. Included inside namespace",
           "// " + NAMESPACE + ".", "",
           "struct CjkBitmap12 { uint32_t cp; uint16_t rows[12]; };",
           "struct CjkBitmap16 { uint32_t cp; uint16_t rows[16]; };",
           "struct LatinBitmap { uint32_t cp; int8_t left, top; uint8_t width, height, advance; uint16_t rows[20]; };",
           ""]
    out += cjk_table("Cjk", lines[("cjk", 12)], 12, rendered.get(12))
    out.append("")
    out += cjk_table("Cjk", lines[("cjk", 16)], 16, rendered.get(16))
    out.append("")
    out += latin_table(lines[("latin", 12)], 12)
    out.append("")
    out += latin_table(lines[("latin", 16)], 16)
    return "\n".join(out) + "\n"


def generate_words(strings):
    out = ["// Generated by tools/launcher_ui.py from system/menu/strings.json. Do not edit.",
           "#pragma once", "", "#include <cstdint>", "", "namespace CardenzaHAL::SystemMenu::detail {", "",
           f"inline constexpr int kLanguageCount = {len(LANGUAGES)};",
           "// " + ", ".join(LANGUAGES) + ": the Launcher's Language value of each (its NVS launcher/lang).",
           "inline constexpr uint8_t kLanguageCodes[kLanguageCount] = {" +
           ", ".join(str(c) for c in (0, 1, 3, 4, 5)) + "};", "enum TextId : uint8_t {"]
    for key in TEXT_IDS:
        out.append("    k" + "".join(part.capitalize() for part in key.replace(".", " ").replace("_", " ").split()) + ",")
    out += ["    kTextCount,", "};",
            "// UTF-8 as the Launcher's T() gives a translation (a Latin-face one starts with \\016).",
            "inline constexpr const char* kTexts[kLanguageCount][kTextCount] = {"]
    for language in LANGUAGES:
        out.append(f"    {{  // {language}")
        for key in TEXT_IDS:
            out.append(f"        {c_string(shown(strings, key, language))},")
        out.append("    },")
    out += ["};",
            "// Each language in its own script, as the Launcher's picker names it (a Latin-face name starts with \\016).",
            "inline constexpr const char* kLanguageNames[kLanguageCount] = {"]
    for language, name in language_name_faces(strings):
        out.append(f"    {c_string(chr(14) + name if language in LATIN_FACE else name)},  // {language}")
    out += ["};", "", "}  // namespace CardenzaHAL::SystemMenu::detail", ""]
    return "\n".join(out)


def everything(launcher, fonts=None):
    commit = git_commit(launcher)
    strings = load_words()
    files = {OUT / name: text for name, text in copies(launcher, commit).items()}
    files[OUT / "glyphs.inc"] = generate_glyphs(launcher, strings, commit, fonts)
    files[WORDS] = generate_words(strings)
    return files, strings


# ---- Comparing frames with the Launcher's own drawing ------------------------------------------------

def slide_frames(presses):
    """How many frames a slide draws: the bar placed, then one every SLIDE_FRAME_MS."""
    return 1 + (presses[-1][0] + SLIDE_SETTLE_MS) // SLIDE_FRAME_MS + 1


def slides_table():
    """SLIDES as C++ (the reference, the HAL's program and tests/system_menu_frames.inc)."""
    out = ["struct SlidePress { uint32_t ms; int selected, scroll; };",
           f"struct SlideRun {{ const char *name; int count; SlidePress presses[{SLIDE_PRESSES}]; uint32_t length; }};",
           f"static const uint32_t kSlideStart = {SLIDE_START}, kSlideFrameMs = {SLIDE_FRAME_MS};",
           "static const SlideRun kSlideRuns[] = {"]
    for name, presses in SLIDES:
        assert len(presses) <= SLIDE_PRESSES and presses[0][0] == 0
        out.append(f'    {{"{name}", {len(presses)}, {{' +
                   ", ".join(f"{{{ms}, {row}, {scroll}}}" for ms, row, scroll in presses) +
                   f"}}, {presses[-1][0] + SLIDE_SETTLE_MS}u}},")
    out.append("};")
    out.append("static const int kSlideRunCount = int(sizeof(kSlideRuns) / sizeof(kSlideRuns[0]));")
    return "\n".join(out) + "\n"


def slide_loop(slide, draw):
    """The slides' frames as C++, the same in both programs: `slide` declares a ListSlide `slide`, `draw`
    draws the frame of `row` and `scroll` at `now` and writes it out."""
    return f"""
  for (const SlideRun &run : kSlideRuns) {{
    {slide}
    slide.keep = true;  // the Cardenza: the rows jump to a scroll, the bar keeps its place on screen
    int row = run.presses[0].selected, scroll = run.presses[0].scroll, next = 1;
    now = kSlideStart;
    {draw};
    for (uint32_t ms = 0; ms <= run.length; ms += kSlideFrameMs) {{
      now = kSlideStart + ms;
      for (; next < run.count && run.presses[next].ms <= ms; ++next) {{
        row = run.presses[next].selected;
        scroll = run.presses[next].scroll;
      }}
      {draw};
    }}
  }}
"""


def launcher_reference(launcher, strings, work):
    """tests/ui_render.cpp's world with the menu's words: the Launcher's menuFrame and status()."""
    tests = launcher / "tests"
    display = (launcher / "src" / "display.cpp").read_text(encoding="utf-8")
    lcd = (launcher / "src" / "platform" / "lcd.cpp").read_text(encoding="utf-8")
    cmake = (tests / "CMakeLists.txt").read_text(encoding="utf-8")

    # The same pieces tests/CMakeLists.txt cuts out for ui_render (its string(FIND ...) pairs).
    def cmake_piece(text, start, end):
        at, stop = text.find(start), text.find(end)
        if at < 0 or stop < 0:
            sys.exit(f"the Launcher's tests changed ('{start[:30]}'): update tools/launcher_ui.py")
        return text[at:stop]
    lcd_logic = "\n".join([cmake_piece(lcd, "void Lcd::greyRect(", "void Lcd::fillRect("),
                           cmake_piece(lcd, "void Lcd::text(", "void Lcd::print("),
                           lcd[lcd.find("void Lcd::textVertical("):]])
    (work / "lcd_text_logic.inc").write_text(lcd_logic, encoding="utf-8")
    pairs = re.findall(r'string\(FIND "\$\{display\}" "([^"]*)" draw_start\)\s*'
                       r'string\(FIND "\$\{display\}" "([^"]*)" draw_end\)', cmake)
    if not pairs:
        sys.exit("the Launcher's tests/CMakeLists.txt changed: update tools/launcher_ui.py")
    draw = [cmake_piece(display, a.replace("\\n", "\n"), b.replace("\\n", "\n")) for a, b in pairs]
    (work / "ui_render_draw.inc").write_text("\n".join(draw), encoding="utf-8")

    render = (tests / "ui_render.cpp").read_text(encoding="utf-8")
    # The world above its main (which list_slide_tests leaves out with UI_RENDER_NO_MAIN).
    cut = render.find("#ifndef UI_RENDER_NO_MAIN")
    head = render[:cut if cut >= 0 else render.index("int main(int argc")]
    main = [slides_table(), "int main(int argc, char **argv) {", "  motion = false;", "  depth = 1;"]
    names = [c_string(chr(14) + name if language in LATIN_FACE else name)
             for language, name in language_name_faces(strings)]
    for li, language in enumerate(LANGUAGES):
        def w(key):
            return c_string(shown(strings, key, language))
        def still(call):
            return f"      {call}; fwrite(screen.canvas.data(), 1, screen.canvas.size(), stdout);"
        translated = [shown(strings, key, language) for key in ("english", "chinese", "japanese", "indonesian", "vietnamese")]
        plain = lambda text: text.replace(chr(14), "")
        main += ["  {", f"    i18nSetLanguage(Language::{LANGUAGE_ENUM[language]});", f"    pageTitle = {w('title')};",
                 "    std::vector<Option> menu = {",
                 f"      {{{w('continue')}, nullptr, NO_COLOR, \"CardenzaSynthFirmware 0.2.1\"}},",
                 f"      {{{w('settings')}, nullptr, NO_COLOR, {w('settings.detail')}}},",
                 f"      {{{w('launcher')}, nullptr, NO_COLOR, {w('launcher.detail')}}},",
                 f"      {{{w('restart')}, nullptr, NO_COLOR, {w('restart.detail')}}},",
                 f"      {{{w('off')}, nullptr, NO_COLOR, {w('off.detail')}}},",
                 f"      {{{w('about')}, nullptr, NO_COLOR, {w('about.detail')}}},",
                 f"      {{{w('download')}, nullptr, NO_COLOR, {w('download.detail')}}}}};",
                 "    for (int selected = 0; selected < 7; ++selected) {",
                 still("menuFrame(menu, selected, selected < 4 ? 0 : selected - 3); status()"),
                 "    }",
                 f"    pageTitle = {w('settings')};",
                 "    std::vector<Option> settings = {",
                 f"      {{{w('volume')}, nullptr, NO_COLOR, {w('volume.detail')}, \"{SAMPLE_TEXTS[0]}\"}},",
                 f"      {{{w('volume_limit')}, nullptr, NO_COLOR, {w('volume_limit.detail')}, \"{SAMPLE_TEXTS[1]}\"}},",
                 f"      {{{w('screen')}, nullptr, NO_COLOR, {w('shared.detail')}, \"{SAMPLE_TEXTS[2]}\"}},",
                 f"      {{{w('keys')}, nullptr, NO_COLOR, {w('keys.detail')}, \"{SAMPLE_TEXTS[3]}\"}},",
                 f"      {{{w('language')}, nullptr, NO_COLOR, {w('shared.detail')}, {names[li]}}},",
                 f"      {{{w('webusb')}, nullptr, NO_COLOR, {w('webusb.detail')}, {w('webusb.first')}}},",
                 f"      {{{w('shot.upload')}, nullptr, NO_COLOR, {w('shot.upload.detail')}, {w('value.on')}}},",
                 f"      {{{w('reset')}, nullptr, NO_COLOR, {w('reset.detail')}}}}};",
                 "    for (int selected = 0; selected < 8; ++selected) {",
                 still("menuFrame(settings, selected, selected < 4 ? 0 : selected - 3); status()"),
                 "    }",
                 f"    pageTitle = {w('language')};",
                 "    std::vector<Option> languages = {"]
        for i, name in enumerate(names):
            own = plain(language_name_faces(strings)[i][1])
            detail = "" if plain(translated[i]) == own else c_string(translated[i])
            main.append(f"      {{{name}, nullptr, NO_COLOR, {detail or chr(34) * 2}}},")
        main += ["    };",
                 "    for (int selected = 0; selected < 5; ++selected) {",
                 still("menuFrame(languages, selected, selected < 4 ? 0 : selected - 3); status()"),
                 "    }",
                 f"    pageTitle = {w('reset.button')};",
                 "    for (int button = 0; button < 2; ++button) {",
                 still(f"drawDialog({w('reset.question')}, {w('reset.explain')}, {w('reset.button')}, {w('cancel')}, button)"),
                 "    }",
                 # The browser notice's choices: a list like the languages' (the Launcher's lists), no details.
                 f"    pageTitle = {w('webusb')};",
                 "    std::vector<Option> landing = {",
                 f"      {{{w('webusb.first')}, nullptr, NO_COLOR, \"\"}},",
                 f"      {{{w('webusb.always')}, nullptr, NO_COLOR, \"\"}},",
                 f"      {{{w('webusb.never')}, nullptr, NO_COLOR, \"\"}}}};",
                 "    for (int selected = 0; selected < 3; ++selected) {",
                 still("menuFrame(landing, selected, 0); status()"),
                 "    }",
                 # The slides: loopMenu on the Cardenza (selection and viewport set at once, the bar aimed at
                 # the row as menuFrame draws).
                 f"    pageTitle = {w('title')};",
                 "    motion = true;",
                 slide_loop("ListSlide slide;",
                            "menuFrame(menu, row, scroll, true, false, 0, &slide, row); status(); "
                            "fwrite(screen.canvas.data(), 1, screen.canvas.size(), stdout)"),
                 "    motion = false;",
                 "  }"]
    main += ["  return 0;", "}"]
    (work / "reference.cpp").write_text(head + "\n".join(main) + "\n", encoding="utf-8")
    # The Launcher's i18n.cpp with the glyphs the menu renders for words its tables lack yet (RENDERED), as
    # the Launcher will have them once it uses the words: its own cjkGlyph and cjkHasGlyph renamed, and the
    # two looking in those first.
    i18n = (launcher / "src" / "platform" / "i18n.cpp").read_text(encoding="utf-8")
    for old, new in (("bool cjkGlyph(uint32_t cp, int size, CjkGlyph &glyph) {",
                      "bool launcherCjkGlyph(uint32_t cp, int size, CjkGlyph &glyph) {"),
                     ("bool cjkHasGlyph(uint32_t cp) {", "bool launcherCjkHasGlyph(uint32_t cp) {")):
        if i18n.count(old) != 1:
            sys.exit(f"the Launcher's i18n.cpp changed ('{old}'): update tools/launcher_ui.py")
        i18n = i18n.replace(old, new)
    extra = []
    for size, glyphs in sorted(RENDERED.items()):
        for char, rows in sorted(glyphs.items()):
            extra.append(f"    {{0x{ord(char):04X}u, {size}, {{" + ", ".join(f"0x{r << (32 - size):08x}u" for r in rows) + "}},")
    i18n += """
namespace {
struct MenuGlyph { uint32_t cp; int size; uint32_t rows[16]; };
const MenuGlyph kMenuGlyphs[] = {
""" + "\n".join(extra or ["    {0, 0, {0}},"]) + """
};
}  // namespace
bool cjkGlyph(uint32_t cp, int size, CjkGlyph &glyph) {
  for (const MenuGlyph &g : kMenuGlyphs)
    if (g.size != 0 && g.cp == cp && size < 24 && g.size == (size >= 16 ? 16 : 12)) {
      std::memset(glyph.rows, 0, sizeof(glyph.rows));
      glyph.em = uint8_t(g.size);
      for (int r = 0; r < g.size; ++r) glyph.rows[r] = g.rows[r];
      return true;
    }
  return launcherCjkGlyph(cp, size, glyph);
}
bool cjkHasGlyph(uint32_t cp) {
  for (const MenuGlyph &g : kMenuGlyphs)
    if (g.size != 0 && g.cp == cp) return true;
  return launcherCjkHasGlyph(cp);
}
"""
    (work / "i18n_menu.cpp").write_text(i18n, encoding="utf-8")
    binary = work / "reference"
    subprocess.run([compiler(), "-std=c++17", "-O1", "-DCARDENZA=1", "-DPART_08MB=1", "-I", str(launcher / "src"),
                    "-I", str(launcher / "lib" / "ArduinoJson" / "src"), "-I", str(launcher / "lib" / "tweetnacl"),
                    "-I", str(work), "-I", str(tests),
                    f'-DLABEL_FIXTURES="{tests / "fixtures" / "labels.json"}"',
                    f'-DNOTES_FIXTURES="{tests / "fixtures" / "notes.json"}"',
                    "-I", str(launcher / "src" / "platform"),
                    str(work / "reference.cpp"), str(work / "i18n_menu.cpp"), "-o", str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, check=True).stdout


HAL_FRAMES = r'''
#include "system/menu/frame.h"
#include "system/menu/launcher/motion.h"
#include <cstdio>
#include <vector>
using namespace CardenzaHAL::SystemMenu;
namespace Settings = CardenzaHAL::Settings;
uint32_t CardenzaHAL::SystemMenu::launcher::launcherMillis() { return 0; }
SLIDES_TABLE
static const int kSamples[] = {SAMPLE_VALUES};
static const int kCodes[] = {LANGUAGE_CODES};
int main() {
  std::vector<uint8_t> canvas(detail::kWidth * detail::kHeight);
  const auto out = [&] { fwrite(canvas.data(), 1, canvas.size(), stdout); };
  for (int language = 0; language < detail::kLanguageCount; ++language) {
    detail::View view;
    view.language = language;
    view.firmware = "CardenzaSynthFirmware 0.2.1";
    for (int i = 0; i < int(sizeof(kSamples) / sizeof(kSamples[0])); ++i) view.state.values[i] = kSamples[i];
    view.state.values[int(Settings::Id::Language)] = kCodes[language];
    const auto list = [](int selected) { return detail::ListState{selected, selected < 4 ? 0 : selected - 3}; };
    for (int selected = 0; selected < detail::kItemCount; ++selected) {
      view.state.page = detail::Page::Main;
      view.state.main = list(selected);
      detail::PaintGrey(view, nullptr, 0, 0, detail::kHeight, canvas.data());
      out();
    }
    for (int selected = 0; selected < detail::kRowCount; ++selected) {
      view.state.page = detail::Page::Settings;
      view.state.settings = list(selected);
      detail::PaintGrey(view, nullptr, 0, 0, detail::kHeight, canvas.data());
      out();
    }
    for (int selected = 0; selected < 5; ++selected) {
      view.state.page = detail::Page::Language;
      view.state.language = list(selected);
      detail::PaintGrey(view, nullptr, 0, 0, detail::kHeight, canvas.data());
      out();
    }
    for (int button = 0; button < 2; ++button) {
      view.state.page = detail::Page::Reset;
      view.state.button = button;
      detail::PaintGrey(view, nullptr, 0, 0, detail::kHeight, canvas.data());
      out();
    }
    for (int selected = 0; selected < detail::kLandingChoices; ++selected) {
      view.state.page = detail::Page::Landing;
      view.state.landing = list(selected);
      detail::PaintGrey(view, nullptr, 0, 0, detail::kHeight, canvas.data());
      out();
    }
    view.state.page = detail::Page::Main;
    // The slides: system/menu.cpp's loop (the bar aimed at the selected row as each frame is drawn).
    uint32_t now = 0;
SLIDE_LOOP
  }
}
'''


def hal_frames(work):
    program = HAL_FRAMES.replace("SLIDES_TABLE", slides_table()).replace(
        "SAMPLE_VALUES", ", ".join(str(v) for v in SAMPLE_VALUES)).replace(
        "LANGUAGE_CODES", ", ".join(str(c) for c in LANGUAGE_CODES)).replace(
        "SLIDE_LOOP", slide_loop("launcher::ListSlide slide;",
                                 "view.state.main = {row, scroll}; "
                                 "detail::PaintGrey(view, &slide, now, 0, detail::kHeight, canvas.data()); "
                                 "fwrite(canvas.data(), 1, canvas.size(), stdout)"))
    (work / "hal_frames.cpp").write_text(program)
    binary = work / "hal_frames"
    sources = [str(work / "hal_frames.cpp"), str(ROOT / "system" / "menu" / "logic.cpp"),
               str(ROOT / "system" / "settings_logic.cpp"),
               str(ROOT / "system" / "menu" / "frame.cpp"), str(ROOT / "system" / "menu" / "launcher" / "glyphs.cpp"),
               str(ROOT / "system" / "menu" / "launcher" / "lcd.cpp")]
    subprocess.run([compiler(), "-std=c++17", "-O1", "-I", str(ROOT)] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, check=True).stdout


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return h


def compare(launcher, strings):
    size = 240 * 135
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        reference = launcher_reference(launcher, strings, work)
        ours = hal_frames(work)
    stills = sum(rows for _, rows in STILLS)
    slides = [slide_frames(presses) for _, presses in SLIDES]
    per_language = stills + sum(slides)
    count = len(LANGUAGES) * per_language
    if len(reference) != count * size or len(ours) != count * size:
        sys.exit(f"frame sizes differ: Launcher {len(reference)} bytes, HAL {len(ours)} (expected {count * size})")

    def frame(data, index):
        return data[index * size:(index + 1) * size]

    def name(index):
        language, at = LANGUAGES[index // per_language], index % per_language
        if at < stills:
            for page, rows in STILLS:
                if at < rows:
                    return f"{language} {STILL_PAGE_NAMES[page]} {at}"
                at -= rows
        at -= stills
        for (run, presses), frames in zip(SLIDES, slides):
            if at < frames:
                return f"{language} slide '{run}' frame {at} ({max(0, at - 1) * SLIDE_FRAME_MS} ms)"
            at -= frames
        return f"{language} frame {at}"

    bad = 0
    for i in range(count):
        a, b = frame(reference, i), frame(ours, i)
        if a == b:
            continue
        bad += 1
        if bad <= 10:
            diff = [p for p in range(size) if a[p] != b[p]]
            first = diff[0]
            print(f"{name(i)}: {len(diff)} pixels differ, first at x {first % 240} y {first // 240}"
                  f" (Launcher {a[first]}, HAL {b[first]})")
    if bad:
        sys.exit(f"{bad} of {count} frames differ from the Launcher's")
    hashes, crcs = [], []
    for language in range(len(LANGUAGES)):
        base = language * per_language
        hashes += [fnv1a64(frame(reference, base + i)) for i in range(stills)]
        at = base + stills
        for frames in slides:
            crcs.append(zlib.crc32(reference[at * size:(at + frames) * size]))
            at += frames
    out = ["// Generated by tools/launcher_ui.py --compare. Do not edit. tests/system_menu_test.cpp draws these",
           "// frames with the HAL's copy and checks them against what the Launcher's own menuFrame and status()",
           "// drew, by language (" + ", ".join(LANGUAGES) + ").",
           "",
           "// The still frames, in this order in each language: the page (0 main, 1 settings, 2 language, 3 the",
           "// Reset question, 4 the browser notice's choices) and how many (a row selected, or a button focused, each).",
           "struct StillPage { int page, count; };",
           "static const StillPage kStillPages[] = {" + ", ".join(f"{{{p}, {n}}}" for p, n in STILLS) + "};",
           f"static const int kStillCount = {stills};",
           "// The Settings page's values in them: volume, limit, screen, key lights (the language: its own; the",
           "// browser notice: its default).",
           "static const int kSampleValues[] = {" + ", ".join(str(v) for v in SAMPLE_VALUES) + "};",
           "// FNV-1a (64 bits) of each still frame, by language.",
           "static const uint64_t kLauncherFrameHashes[] = {"]
    for i in range(0, len(hashes), 4):
        out.append("    " + ", ".join(f"0x{h:016x}ull" for h in hashes[i:i + 4]) + ",")
    out += ["};", "",
            "// The selection bar's slides (SLIDES there): the bar placed at kSlideStart on the first press's row,",
            "// then a frame every kSlideFrameMs until `length`, each after the presses due by then.",
            slides_table().rstrip("\n"),
            "// CRC-32 (zlib's) of each slide's frames one after another, by language and slide.",
            "static const uint32_t kLauncherSlideCrcs[] = {"]
    for i in range(0, len(crcs), len(SLIDES)):
        out.append("    " + ", ".join(f"0x{c:08x}u" for c in crcs[i:i + len(SLIDES)]) + ",")
    out.append("};")
    FRAMES.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"all {count} frames are the Launcher's, pixel for pixel ({len(LANGUAGES) * stills} still, "
          f"{len(LANGUAGES) * sum(slides)} of {len(LANGUAGES) * len(SLIDES)} slides); "
          f"{FRAMES.relative_to(ROOT)} written")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--launcher", required=True, type=pathlib.Path, help="a 203-launcher checkout")
    parser.add_argument("--check", action="store_true", help="fail when the copy is not current")
    parser.add_argument("--compare", action="store_true", help="compare frames with the Launcher's drawing")
    parser.add_argument("--fonts", type=pathlib.Path, help=f"the folder of {CJK_FONT} (the official HarmonyOS Sans, "
                        "as the Launcher's NOTICE.md names it): glyphs its tables lack are rendered as it renders them")
    args = parser.parse_args()
    launcher = args.launcher.expanduser().resolve()
    if not (launcher / "src" / "platform" / "i18n.cpp").exists():
        sys.exit(f"{launcher} is not a 203-launcher checkout (no src/platform/i18n.cpp)")
    files, strings = everything(launcher, args.fonts)
    if args.check:
        commit = re.compile(r" at ([0-9a-f]{12}\+?|unknown)\b")
        same = lambda a, b: commit.sub(" at -", a) == commit.sub(" at -", b)
        stale = [str(path.relative_to(ROOT)) for path, text in files.items()
                 if not path.exists() or not same(path.read_text(encoding="utf-8"), text)]
        if stale:
            sys.exit("not current: " + ", ".join(stale) + " (run tools/launcher_ui.py --launcher ...)")
        print("system/menu/launcher is current")
    else:
        OUT.mkdir(parents=True, exist_ok=True)
        for path, text in files.items():
            path.write_text(text, encoding="utf-8")
        print(f"wrote {len(files)} files from {launcher} ({git_commit(launcher)})")
    if args.compare:
        compare(launcher, strings)


if __name__ == "__main__":
    main()
