#!/usr/bin/env python3
"""Generates system/menu/text.inc: the system menu's words (system/menu/strings.json) and the glyphs they
need, taken from the 203 Launcher's own bitmaps so the menu reads exactly like the Launcher.

    python3 tools/system_menu_text.py --launcher ~/projects/203-launcher

The Launcher keeps 12-pixel bitmaps rendered from HarmonyOS Sans for its Chinese, Japanese and Vietnamese
text (203-launcher src/platform/i18n_data.inc, made by its tools/i18n.py; NOTICE.md there). This tool
builds a small program against the Launcher's src/platform/i18n.cpp (a C++17 compiler is needed, no
ESP-IDF), asks it for each character's bitmap and writes those bitmaps here: no font file and no outlines
come into the HAL (README.md, "System menu", and licenses/HarmonyOS-Sans-LICENSE.txt). English and
Indonesian use the 5x7 font (system/menu/font5x7.h), like the Launcher's.

Run it after changing strings.json; tests/system_menu_test.cpp checks that every word has its glyphs.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
STRINGS = ROOT / "system" / "menu" / "strings.json"
OUT = ROOT / "system" / "menu" / "text.inc"

# The Launcher's languages, in the order of its NVS value launcher/lang (docs/SHARED_SETTINGS.md there):
# 0 English, 1 Simplified Chinese, 2 read as 1, 3 Japanese, 4 Indonesian, 5 Vietnamese.
LANGUAGES = ["en", "zh-Hans", "ja", "id", "vi"]
FACES = {"en": "Font5x7", "zh-Hans": "Cjk", "ja": "Cjk", "id": "Font5x7", "vi": "Latin"}
TEXT_IDS = ["title", "continue", "launcher", "launcher.detail", "restart", "restart.detail", "off",
            "off.detail", "download", "download.detail", "release"]

PROBE = r'''
#include "platform/i18n.h"
#include <cstdio>
int main(int argc, char** argv) {
  const bool latin = argc > 1;
  if (latin) printf("B %d %d %d\n", latinBaseline(1), latinTop(1), latinHeight(1));
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


def build_probe(launcher, work):
    source = work / "probe.cpp"
    source.write_text(PROBE)
    binary = work / "probe"
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        sys.exit("a C++17 compiler is needed (set CXX)")
    subprocess.run([compiler, "-std=c++17", "-O1", "-I", str(launcher / "src"),
                    "-I", str(launcher / "lib" / "ArduinoJson" / "src"), str(source),
                    str(launcher / "src" / "platform" / "i18n.cpp"), "-o", str(binary)], check=True)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--launcher", required=True, type=pathlib.Path, help="a 203-launcher checkout")
    parser.add_argument("--out", type=pathlib.Path, default=OUT)
    args = parser.parse_args()
    launcher = args.launcher.expanduser().resolve()
    if not (launcher / "src" / "platform" / "i18n.cpp").exists():
        sys.exit(f"{launcher} is not a 203-launcher checkout (no src/platform/i18n.cpp)")
    strings = json.loads(STRINGS.read_text(encoding="utf-8"))
    for key in TEXT_IDS:
        for language in LANGUAGES:
            if not strings.get(key, {}).get(language):
                sys.exit(f"strings.json: {key} has no {language}")

    cjk = sorted({ord(c) for key in TEXT_IDS for language in LANGUAGES if FACES[language] == "Cjk"
                  for c in strings[key][language] if ord(c) >= 0x80})
    latin = sorted({ord(c) for key in TEXT_IDS for language in LANGUAGES if FACES[language] == "Latin"
                    for c in strings[key][language]} | {ord(".")})
    for key in TEXT_IDS:
        for language in LANGUAGES:
            if FACES[language] != "Latin":
                bad = [c for c in strings[key][language] if ord(c) < 0x80 and not 32 <= ord(c) < 127]
                if bad:
                    sys.exit(f"strings.json: {key} ({language}) has control characters")

    with tempfile.TemporaryDirectory() as tmp:
        binary = build_probe(launcher, pathlib.Path(tmp))
        cjk_lines = ask(binary, cjk, False)
        latin_lines = ask(binary, latin, True)

    missing = [chr(int(l.split()[1])) for l in cjk_lines + latin_lines if l.startswith("M ")]
    if missing:
        sys.exit("the Launcher has no glyph for: " + "".join(missing) + " (change the words)")

    out = []
    out.append("// Generated by tools/system_menu_text.py from system/menu/strings.json and the 203 Launcher's")
    out.append("// bitmaps (HarmonyOS Sans at 12 px, rendered by the Launcher's tools/i18n.py). Do not edit.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("namespace CardenzaHAL::SystemMenu::detail {")
    out.append("")
    out.append(f"inline constexpr int kLanguageCount = {len(LANGUAGES)};")
    out.append("enum TextId : uint8_t {")
    for key in TEXT_IDS:
        name = "k" + "".join(part.capitalize() for part in key.replace(".", " ").split())
        out.append(f"    {name},")
    out.append("    kTextCount,")
    out.append("};")
    out.append("enum class Face : uint8_t { Font5x7, Cjk, Latin };")
    out.append("inline constexpr Face kFaces[kLanguageCount] = {" +
               ", ".join(f"Face::{FACES[l]}" for l in LANGUAGES) + "};")
    out.append("// UTF-8; " + ", ".join(LANGUAGES) + ".")
    out.append("inline constexpr const char* kTexts[kLanguageCount][kTextCount] = {")
    for language in LANGUAGES:
        out.append(f"    {{  // {language}")
        for key in TEXT_IDS:
            out.append(f"        {c_string(strings[key][language])},")
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("// 12 by 12 pixels, a row a value, the leftmost pixel in bit 11; the em box's top is 2 pixels")
    out.append("// above the 5x7 font's (the Launcher's cjkTop at scale 1). Sorted by code point.")
    out.append("struct CjkGlyph { uint32_t cp; uint16_t rows[12]; };")
    out.append("inline constexpr CjkGlyph kCjkGlyphs[] = {")
    for line in cjk_lines:
        parts = line.split()
        cp, em = int(parts[1]), int(parts[2])
        assert em == 12, line
        rows = [int(v, 16) >> 20 for v in parts[3:3 + 12]]
        out.append(f"    {{0x{cp:04X}, {{" + ", ".join(f"0x{r:03X}" for r in rows) + f"}}}},  // {chr(cp)}")
    out.append("};")
    out.append("")
    base = [l for l in latin_lines if l.startswith("B ")][0].split()
    out.append("// Vietnamese: the Launcher's Latin face at 12 px. A glyph's ink starts `left` pixels after the pen")
    out.append("// and `top` rows from the baseline (negative: above); rows have the leftmost pixel in bit 15.")
    out.append(f"inline constexpr int kLatinBaseline = {base[1]};  // below the text origin (5x7 font's top)")
    out.append(f"inline constexpr int kLatinTop = {base[2]};       // ink of a line, from the text origin")
    out.append(f"inline constexpr int kLatinHeight = {base[3]};")
    out.append("struct LatinGlyph { uint32_t cp; int8_t left, top; uint8_t width, height, advance; uint16_t rows[16]; };")
    out.append("inline constexpr LatinGlyph kLatinGlyphs[] = {")
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
    out.append("")
    out.append("}  // namespace CardenzaHAL::SystemMenu::detail")
    args.out.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{args.out}: {len(cjk_lines)} CJK and {len(latin_lines) - 1} Latin glyphs")


if __name__ == "__main__":
    main()
