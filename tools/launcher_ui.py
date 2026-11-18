#!/usr/bin/env python3
"""The system menu's copy of the 203 Launcher's drawing code, and the menu's words.

    python3 tools/launcher_ui.py --launcher ~/projects/203-launcher             copy and generate
    python3 tools/launcher_ui.py --launcher ~/projects/203-launcher --check     the copy is current
    python3 tools/launcher_ui.py --launcher ~/projects/203-launcher --compare   frames as the Launcher's code
                                                                                 draws them

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

and generates glyphs.inc: the 12 px glyphs the menu's words need, read out of the Launcher's own tables
(HarmonyOS Sans rendered by the Launcher's tools/i18n.py; NOTICE.md there) by a small program built
against its src/platform/i18n.cpp, and words.inc (system/menu/strings.json). What the HAL adds around
them is its own: system/menu/launcher/lcd.h (the canvas, one band of rows at a time), glyphs.cpp (the
glyph lookups over glyphs.inc) and frame.cpp (the Launcher's menuFrame and status rail on the Cardenza,
ported line by line).

--check fails when the copy differs from what this checkout would give. --compare draws the menu in
every language and position with the Launcher's real menuFrame and status() (as its tests/ui_render.cpp
does) and with the HAL's frame.cpp, and fails on any pixel that differs: the still frames, and the
selection bar's slide frame by frame (SLIDES: a move, key repeat, a reversal, a scroll each way, every
4 ms until it settles). It also rewrites tests/system_menu_frames.inc, the frames' hashes the host tests
check without a Launcher.

Needs a C++17 compiler (CXX, else c++/g++/clang++) and Python 3; no ESP-IDF.
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
TEXT_IDS = ["title", "continue", "launcher", "launcher.detail", "restart", "restart.detail", "off",
            "off.detail", "download", "download.detail", "release"]

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
int main(int argc, char** argv) {
  const bool latin = argc > 1;
  if (latin) printf("B %d %d %d %d\n", latinBaseline(1), latinTop(1), latinHeight(1), latinHighest(1));
  unsigned cp;
  while (scanf("%u", &cp) == 1) {
    if (latin) {
      LatinGlyph g;
      if (!latinGlyph(cp, 12, g)) { printf("M %u\n", cp); continue; }
      printf("L %u %d %d %d %d %d", cp, g.left, g.top, g.width, g.height, g.advance);
      for (int r = 0; r < g.height; ++r) printf(" %08x", (unsigned)g.rows[r]);
      printf("\n");
      continue;
    }
    CjkGlyph g;
    if (!cjkGlyph(cp, 12, g)) { printf("M %u\n", cp); continue; }
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
    # PART_08MB: the Cardenza's build of the tables (8 MB of flash).
    subprocess.run([compiler(), "-std=c++17", "-O1", "-DPART_08MB=1", "-I", str(launcher / "src"),
                    str(source), str(launcher / "src" / "platform" / "i18n.cpp"), "-o", str(binary)], check=True)
    return binary


def ask(binary, codepoints, latin):
    args = [str(binary)] + (["latin"] if latin else [])
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


def generate_glyphs(launcher, strings, commit):
    cjk = sorted({ord(c) for key in TEXT_IDS for language in LANGUAGES if language not in LATIN_FACE
                  for c in strings[key][language] if ord(c) >= 0x80})
    latin = sorted({ord(c) for key in TEXT_IDS for language in LATIN_FACE for c in strings[key][language]}
                   | {ord(".")})
    with tempfile.TemporaryDirectory() as tmp:
        binary = build_probe(launcher, pathlib.Path(tmp))
        cjk_lines = ask(binary, cjk, False)
        latin_lines = ask(binary, latin, True)
    missing = [chr(int(l.split()[1])) for l in cjk_lines + latin_lines if l.startswith("M ")]
    if missing:
        sys.exit("the Launcher has no glyph for: " + "".join(missing) + " (change the words)")

    out = [f"// Generated by tools/launcher_ui.py from the 203 Launcher's glyph tables at {commit}",
           "// (src/platform/i18n_data.inc there: HarmonyOS Sans at 12 px, made by its tools/i18n.py; NOTICE.md",
           "// there and licenses/HarmonyOS-Sans-LICENSE.txt here): only the characters of",
           "// system/menu/strings.json. Do not edit. Included inside namespace " + NAMESPACE + ".", ""]
    out.append("// 12 by 12 pixels, a row a value, the leftmost pixel in bit 11. Sorted by code point.")
    out.append("struct CjkBitmap { uint32_t cp; uint16_t rows[12]; };")
    out.append("constexpr CjkBitmap kCjkBitmaps[] = {")
    for line in cjk_lines:
        parts = line.split()
        cp, em = int(parts[1]), int(parts[2])
        assert em == 12, line
        rows = [int(v, 16) >> 20 for v in parts[3:3 + 12]]
        out.append(f"    {{0x{cp:04X}, {{" + ", ".join(f"0x{r:03X}" for r in rows) + f"}}}},  // {chr(cp)}")
    out.append("};")
    out.append("")
    base = [l for l in latin_lines if l.startswith("B ")][0].split()
    out.append("// The Latin face at 12 px (text scale 1): ink `left` pixels after the pen and `top` rows from the")
    out.append("// baseline (negative: above); rows with the leftmost pixel in bit 15. Sorted by code point.")
    out.append(f"constexpr int kLatinBaseline1 = {base[1]}, kLatinTop1 = {base[2]}, kLatinHeight1 = {base[3]}, "
               f"kLatinHighest1 = {base[4]};")
    out.append("struct LatinBitmap { uint32_t cp; int8_t left, top; uint8_t width, height, advance; uint16_t rows[16]; };")
    out.append("constexpr LatinBitmap kLatinBitmaps[] = {")
    for line in latin_lines:
        if not line.startswith("L "):
            continue
        parts = line.split()
        cp, left, top, width, height, advance = (int(v) for v in parts[1:7])
        assert width <= 16 and height <= 16, line
        rows = [int(v, 16) >> 16 for v in parts[7:7 + height]]
        out.append(f"    {{0x{cp:04X}, {left}, {top}, {width}, {height}, {advance}, {{" +
                   ", ".join(f"0x{r:04X}" for r in rows) + f"}}}},  // {chr(cp)}")
    out.append("};")
    return "\n".join(out) + "\n"


def generate_words(strings):
    out = ["// Generated by tools/launcher_ui.py from system/menu/strings.json. Do not edit.",
           "#pragma once", "", "#include <cstdint>", "", "namespace CardenzaHAL::SystemMenu::detail {", "",
           f"inline constexpr int kLanguageCount = {len(LANGUAGES)};",
           "// " + ", ".join(LANGUAGES) + ": the Launcher's Language value of each (its NVS launcher/lang).",
           "inline constexpr uint8_t kLanguageCodes[kLanguageCount] = {" +
           ", ".join(str(c) for c in (0, 1, 3, 4, 5)) + "};", "enum TextId : uint8_t {"]
    for key in TEXT_IDS:
        out.append("    k" + "".join(part.capitalize() for part in key.replace(".", " ").split()) + ",")
    out += ["    kTextCount,", "};",
            "// UTF-8 as the Launcher's T() gives a translation (a Latin-face one starts with \\016).",
            "inline constexpr const char* kTexts[kLanguageCount][kTextCount] = {"]
    for language in LANGUAGES:
        out.append(f"    {{  // {language}")
        for key in TEXT_IDS:
            out.append(f"        {c_string(shown(strings, key, language))},")
        out.append("    },")
    out += ["};", "", "}  // namespace CardenzaHAL::SystemMenu::detail", ""]
    return "\n".join(out)


def everything(launcher):
    commit = git_commit(launcher)
    strings = load_words()
    files = {OUT / name: text for name, text in copies(launcher, commit).items()}
    files[OUT / "glyphs.inc"] = generate_glyphs(launcher, strings, commit)
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
    for language in LANGUAGES:
        def w(key):
            return c_string(shown(strings, key, language))
        main += ["  {", f"    i18nSetLanguage(Language::{LANGUAGE_ENUM[language]});", f"    pageTitle = {w('title')};",
                 "    std::vector<Option> menu = {",
                 f"      {{{w('continue')}, nullptr, NO_COLOR, \"CardenzaSynthFirmware 0.2.1\"}},",
                 f"      {{{w('launcher')}, nullptr, NO_COLOR, {w('launcher.detail')}}},",
                 f"      {{{w('restart')}, nullptr, NO_COLOR, {w('restart.detail')}}},",
                 f"      {{{w('off')}, nullptr, NO_COLOR, {w('off.detail')}}},",
                 f"      {{{w('download')}, nullptr, NO_COLOR, {w('download.detail')}}}}};",
                 "    for (int selected = 0; selected < 5; ++selected) {",
                 "      menuFrame(menu, selected, selected < 4 ? 0 : selected - 3);",
                 "      status();",
                 "      fwrite(screen.canvas.data(), 1, screen.canvas.size(), stdout);",
                 "    }",
                 # The slides: loopMenu on the Cardenza (selection and viewport set at once, the bar aimed at
                 # the row as menuFrame draws).
                 "    motion = true;",
                 slide_loop("ListSlide slide;",
                            "menuFrame(menu, row, scroll, true, false, 0, &slide, row); status(); "
                            "fwrite(screen.canvas.data(), 1, screen.canvas.size(), stdout)"),
                 "    motion = false;",
                 "  }"]
    main += ["  return 0;", "}"]
    (work / "reference.cpp").write_text(head + "\n".join(main) + "\n", encoding="utf-8")
    binary = work / "reference"
    subprocess.run([compiler(), "-std=c++17", "-O1", "-DCARDENZA=1", "-DPART_08MB=1", "-I", str(launcher / "src"),
                    "-I", str(launcher / "lib" / "ArduinoJson" / "src"), "-I", str(launcher / "lib" / "tweetnacl"),
                    "-I", str(work), "-I", str(tests),
                    f'-DLABEL_FIXTURES="{tests / "fixtures" / "labels.json"}"',
                    f'-DNOTES_FIXTURES="{tests / "fixtures" / "notes.json"}"',
                    str(work / "reference.cpp"), str(launcher / "src" / "platform" / "i18n.cpp"), "-o", str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, check=True).stdout


HAL_FRAMES = r'''
#include "system/menu/frame.h"
#include "system/menu/launcher/motion.h"
#include <cstdio>
#include <vector>
using namespace CardenzaHAL::SystemMenu;
uint32_t CardenzaHAL::SystemMenu::launcher::launcherMillis() { return 0; }
SLIDES_TABLE
int main() {
  std::vector<uint8_t> canvas(detail::kWidth * detail::kHeight);
  for (int language = 0; language < detail::kLanguageCount; ++language) {
    detail::View view;
    view.language = language;
    view.firmware = "CardenzaSynthFirmware 0.2.1";
    for (int selected = 0; selected < detail::kItemCount; ++selected) {
      view.state.selected = selected;
      view.state.scroll = selected < detail::kVisibleRows ? 0 : selected - detail::kVisibleRows + 1;
      detail::PaintGrey(view, nullptr, 0, 0, detail::kHeight, canvas.data());
      fwrite(canvas.data(), 1, canvas.size(), stdout);
    }
    // The slides: system/menu.cpp's loop (the bar aimed at the selected row as each frame is drawn).
    uint32_t now = 0;
SLIDE_LOOP
  }
}
'''


def hal_frames(work):
    program = HAL_FRAMES.replace("SLIDES_TABLE", slides_table()).replace(
        "SLIDE_LOOP", slide_loop("launcher::ListSlide slide;",
                                 "view.state.selected = row; view.state.scroll = scroll; "
                                 "detail::PaintGrey(view, &slide, now, 0, detail::kHeight, canvas.data()); "
                                 "fwrite(canvas.data(), 1, canvas.size(), stdout)"))
    (work / "hal_frames.cpp").write_text(program)
    binary = work / "hal_frames"
    sources = [str(work / "hal_frames.cpp"), str(ROOT / "system" / "menu" / "logic.cpp"),
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
    stills = 5
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
            return f"{language} selected {at}"
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
           "// FNV-1a (64 bits) of each still frame, by language and selected row.",
           "static const uint64_t kLauncherFrameHashes[] = {"]
    for i in range(0, len(hashes), stills):
        out.append("    " + ", ".join(f"0x{h:016x}ull" for h in hashes[i:i + stills]) + ",")
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
    args = parser.parse_args()
    launcher = args.launcher.expanduser().resolve()
    if not (launcher / "src" / "platform" / "i18n.cpp").exists():
        sys.exit(f"{launcher} is not a 203-launcher checkout (no src/platform/i18n.cpp)")
    files, strings = everything(launcher)
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
