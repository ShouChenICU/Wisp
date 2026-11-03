#!/usr/bin/env python3
"""Generates components/wisp_ui/fonts/lv_font_cjk_16.c, the built-in CJK
fallback font of the device UI (spec 8.3): one 16 px, 4 bpp LVGL font.

Character set (deduplicated, sorted by code point):
  - ASCII U+0020-U+007E;
  - GB2312, every code point the `gb2312` codec decodes (rows 1-87);
  - JIS X 0208 rows 1-8 (symbols, kana, Greek, Cyrillic, box drawing) and
    rows 16-47 (level-1 kanji), every code point the `euc_jp` codec
    decodes;
  - the full CJK Symbols and Punctuation block (U+3000-U+303F) and the
    fullwidth ASCII forms (U+FF01-U+FF5E), which the font has always
    covered in full;
  - minus the few code points the source font has no glyph for
    (ABSENT_FROM_SOURCE);
  - plus every character of the device UI string tables
    (components/wisp_ui/src/strings_*.cpp), so a translation can never
    name a glyph the font lacks.

Glyphs come from a pinned upstream Noto Sans CJK SC Regular (SIL Open Font
License 1.1), downloaded into the work directory and checked against
FONT_SHA256; a mismatch is fatal. The conversion runs a pinned
lv_font_conv through npx, with the options the font has always used
(no compression, plain bitmap format), so the output compiles against
LVGL 9's lv_font_fmt_txt unchanged and keeps the symbol `lv_font_cjk_16`.
Every requested code point must come out as a glyph, or this fails.

Only the Python standard library is used; npx (Node.js) must be on PATH.

Usage:
  gen_cjk_font.py --workdir <new empty dir outside the repo>
                  [--font <already downloaded OTF>] [--output <path>]
  gen_cjk_font.py --list     # print the character set summary and exit
"""

import argparse
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "components/wisp_ui/fonts/lv_font_cjk_16.c"
# One table per language (strings_zh.cpp, strings_ja.cpp, ...); a new
# language's table is picked up without touching this script.
STRING_TABLES = sorted((ROOT / "components/wisp_ui/src").glob("strings_*.cpp"))

FONT_RELEASE = "Sans2.004"
FONT_FILE = "NotoSansCJKsc-Regular.otf"
FONT_URL = (
    "https://raw.githubusercontent.com/notofonts/noto-cjk/"
    f"{FONT_RELEASE}/Sans/OTF/SimplifiedChinese/{FONT_FILE}"
)
FONT_SHA256 = "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b"
LV_FONT_CONV = "lv_font_conv@1.5.3"
FONT_NAME = "lv_font_cjk_16"
SIZE_PX = 16
BPP = 4

JIS_SYMBOL_ROWS = range(1, 9)
JIS_LEVEL1_ROWS = range(16, 48)

# Code points of the sets above that the source font has no glyph for;
# left out rather than failing the whole run. A string table may not use
# them.
ABSENT_FROM_SOURCE = {"\u2252"}  # JIS X 0208 row 2: APPROXIMATELY EQUAL TO OR THE IMAGE OF


def ascii_chars():
    return {chr(c) for c in range(0x20, 0x7F)}


def gb2312_chars():
    chars = set()
    for row in range(0xA1, 0xF8):
        for cell in range(0xA1, 0xFF):
            try:
                chars.add(bytes([row, cell]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def jis_x0208_chars(rows):
    """JIS X 0208 `rows` (1-94), via their EUC-JP encoding."""
    chars = set()
    for row in rows:
        for cell in range(1, 95):
            try:
                chars.add(bytes([0xA0 + row, 0xA0 + cell]).decode("euc_jp"))
            except UnicodeDecodeError:
                pass
    return chars


def legacy_block_chars():
    return {chr(c) for c in range(0x3000, 0x3040)} | {chr(c) for c in range(0xFF01, 0xFF5F)}


# A C++ string literal (no raw strings or prefixes in the tables) or a
# comment, matched in one pass so "//" inside a literal (a URL) is not
# taken for a comment and quotes inside a comment are not taken for text.
_CPP_TOKEN = re.compile(r'"((?:[^"\\\n]|\\.)*)"|//[^\n]*|/\*.*?\*/', re.DOTALL)


def string_table_chars(paths):
    """Every non-ASCII character inside a string literal in `paths`. ASCII
    is already in the base set, and escapes only ever spell ASCII."""
    chars = set()
    for path in paths:
        for m in _CPP_TOKEN.finditer(path.read_text(encoding="utf-8")):
            if m.group(1) is not None:
                chars.update(c for c in m.group(1) if ord(c) > 0x7E)
    return chars


def base_charset():
    return (
        ascii_chars()
        | gb2312_chars()
        | jis_x0208_chars(JIS_SYMBOL_ROWS)
        | jis_x0208_chars(JIS_LEVEL1_ROWS)
        | legacy_block_chars()
    )


def build_charset(string_tables=None):
    """Returns (sorted characters, sorted string-table extras not already in
    the base set)."""
    base = base_charset() - ABSENT_FROM_SOURCE
    from_tables = string_table_chars(STRING_TABLES if string_tables is None else string_tables)
    unrenderable = from_tables & ABSENT_FROM_SOURCE
    if unrenderable:
        sys.exit("error: the string tables use characters the source font lacks: " + "".join(sorted(unrenderable)))
    extras = sorted(from_tables - base, key=ord)
    return sorted(base | from_tables, key=ord), extras


def conv_args(symbols):
    """lv_font_conv's arguments, run inside the work directory (relative
    paths keep local paths out of the generated file's own header)."""
    return [
        "--font", FONT_FILE,
        "--symbols", symbols,
        "--size", str(SIZE_PX),
        "--format", "lvgl",
        "--bpp", str(BPP),
        "--no-compress",
        "-o", f"{FONT_NAME}.c",
        "--lv-include", "lvgl.h",
        "--lv-font-name", FONT_NAME,
    ]


def header(chars, extras):
    """The comment block put above lv_font_conv's own output."""
    command = " ".join(["npx", "--yes", LV_FONT_CONV] + conv_args("<the character set above>"))
    lines = [
        "/*",
        " * Generated by tools/gen_cjk_font.py -- do not edit; rerun the script.",
        " *",
        f" * Glyphs: Noto Sans CJK SC Regular ({FONT_FILE}), release {FONT_RELEASE} of",
        " * https://github.com/notofonts/noto-cjk",
        f" *   {FONT_URL}",
        f" *   SHA-256 {FONT_SHA256}",
        " * License: SIL Open Font License, Version 1.1 (see THIRD_PARTY_NOTICES.md).",
        " *",
        f" * {SIZE_PX} px, {BPP} bpp, {len(chars)} code points: ASCII, GB2312, JIS X 0208",
        " * rows 1-8 (symbols, kana) and 16-47 (level-1 kanji), U+3000-U+303F and",
        " * U+FF01-U+FF5E, minus "
        + ", ".join(f"U+{ord(c):04X}" for c in sorted(ABSENT_FROM_SOURCE, key=ord))
        + " (not in the source font), plus every",
        " * character of components/wisp_ui/src/strings_*.cpp"
        + (f" ({len(extras)} of them not in the sets above)." if extras else "."),
        " *",
        " * Command (from the repository root):",
        " *   python3 tools/gen_cjk_font.py --workdir <new empty directory>",
        " * which downloads and checks the font, then runs inside that directory:",
        f" *   {command}",
        " * lv_font_conv's own header below lists the full --symbols string.",
        " */",
    ]
    return "\n".join(lines) + "\n"


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch_font(workdir, font_arg):
    target = workdir / FONT_FILE
    if font_arg is not None:
        shutil.copyfile(font_arg, target)
    else:
        print(f"downloading {FONT_URL}", file=sys.stderr)
        with urllib.request.urlopen(FONT_URL) as response, open(target, "wb") as out:
            shutil.copyfileobj(response, out)
    actual = sha256_of(target)
    if actual != FONT_SHA256:
        sys.exit(f"error: {FONT_FILE} SHA-256 is {actual}, expected {FONT_SHA256}")


_GLYPH_COMMENT = re.compile(r"/\* U\+([0-9A-F]+) ")


def check_output(text, chars):
    produced = {int(cp, 16) for cp in _GLYPH_COMMENT.findall(text)}
    missing = sorted(ord(c) for c in chars if ord(c) not in produced)
    if missing:
        listed = " ".join(f"U+{cp:04X}" for cp in missing[:40])
        sys.exit(f"error: the font has no glyph for {len(missing)} code points: {listed}")
    if f"const lv_font_t {FONT_NAME} = {{" not in text:
        sys.exit(f"error: lv_font_conv output does not define {FONT_NAME}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--workdir", type=pathlib.Path)
    parser.add_argument("--font", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=OUTPUT)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)

    chars, extras = build_charset()
    print(f"{len(chars)} code points ({len(extras)} only from the string tables)", file=sys.stderr)
    if extras:
        print("  from the string tables: " + "".join(extras), file=sys.stderr)
    if args.list:
        return 0

    if args.workdir is None:
        parser.error("--workdir is required unless --list is given")
    workdir = args.workdir.resolve()
    if workdir == ROOT or ROOT in workdir.parents:
        parser.error("--workdir must be outside the repository")
    if workdir.exists() and any(workdir.iterdir()):
        parser.error("--workdir must be a new or empty directory")
    workdir.mkdir(parents=True, exist_ok=True)

    fetch_font(workdir, args.font)
    subprocess.run(
        ["npx", "--yes", LV_FONT_CONV] + conv_args("".join(chars)), cwd=workdir, check=True
    )
    text = (workdir / f"{FONT_NAME}.c").read_text(encoding="utf-8")
    check_output(text, chars)
    args.output.write_text(header(chars, extras) + text, encoding="utf-8")
    print(f"wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
