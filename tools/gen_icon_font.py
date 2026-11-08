#!/usr/bin/env python3
"""Generates the icon fonts and the icon header of wisp_ui:

- components/wisp_ui/fonts/lv_font_wisp_icons_{16,24,32,48}.c, icon-only
  bitmap fonts (4 bpp, uncompressed). 24/32/48 hold every LV_SYMBOL_* and
  every Wisp icon (WISP_ICONS below) and back the large UI fonts
  (vector-font spec 4.6). 16 holds only the Wisp icons: the body font is
  lv_font_montserrat_16, which carries the LV_SYMBOL_* icons itself and
  falls back to this font before its CJK font
  (components/wisp_ui/src/fonts.cpp).
- components/wisp_ui/include/wisp/ui/symbols.hpp, a WISP_SYMBOL_<NAME>
  macro (UTF-8, like LV_SYMBOL_*) per Wisp icon and the kWispIcons table
  of names that scenes use (ui.icon).

The LV_SYMBOL_* set is every code point of the pinned LVGL's
src/font/lv_symbol_def.h inside U+E000..U+F8FF, without LV_SYMBOL_DUMMY
(U+F8FF, which LVGL draws as an empty glyph itself). WISP_ICONS are the
icons LVGL's set lacks that the shell and scenes want; it is the one list
of them.

The glyphs come from the FontAwesome subset LVGL ships for its built-in
fonts (scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff, the
Solid, Regular and Brands styles in one font), read from the LVGL source
tree the build already fetched, converted by the pinned lv_font_conv
through npx. Font Awesome Free's fonts are under the SIL OFL 1.1 with the
Reserved Font Name "Font Awesome"; these subsets are modified versions, so
their names do not use it (THIRD_PARTY_NOTICES.md).

Usage:
  gen_icon_font.py [--lvgl <LVGL source dir>] [--workdir <empty dir>]
      Regenerates the four fonts and the header. Needs npx and, the first
      time, network access to fetch lv_font_conv. The work directory (a new
      temporary one by default) must be outside the repository.
  gen_icon_font.py [--lvgl <LVGL source dir>] --check
      Offline: fails when the code points in the committed fonts differ
      from the current lv_symbol_def.h and WISP_ICONS, or the header from
      what WISP_ICONS gives.
Without --lvgl the script looks in build/sim/_deps/lvgl-src (the
simulator's FetchContent checkout) and firmware/managed_components/
lvgl__lvgl (the component manager's).
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
FONTS_DIR = ROOT / "components/wisp_ui/fonts"
HEADER = ROOT / "components/wisp_ui/include/wisp/ui/symbols.hpp"
BODY_SIZE = 16
LARGE_SIZES = (24, 32, 48)
SIZES = (BODY_SIZE, *LARGE_SIZES)
LV_FONT_CONV = "lv_font_conv@1.5.3"
LVGL_VERSION = (9, 5, 0)
LVGL_CANDIDATES = [
    ROOT / "build/sim/_deps/lvgl-src",
    ROOT / "firmware/managed_components/lvgl__lvgl",
]
SYMBOL_DEF = "src/font/lv_symbol_def.h"
ICON_FONT = "scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
LICENSE = "Font Awesome Free 5, SIL OFL 1.1; Reserved Font Name 'Font Awesome' not used"

PUA_FIRST = 0xE000
PUA_LAST = 0xF8FF
SYMBOL_DUMMY = 0xF8FF

# The Wisp icons: scene name (ui.icon's `name`; the macro is
# WISP_SYMBOL_<NAME>) -> Font Awesome 5 Free code point, with its Font
# Awesome name where that differs. Names must not repeat the scene names
# of LV_SYMBOL_* icons (components/wisp_scene/src/icons.hpp).
WISP_ICONS = [
    # Voice and chat
    ("microphone", 0xF130),
    ("microphone_slash", 0xF131),
    ("headphones", 0xF025),
    ("comment", 0xF075),
    ("comments", 0xF086),
    ("robot", 0xF544),
    ("brain", 0xF5DC),
    ("magic", 0xF0D0),
    ("lightbulb", 0xF0EB),
    # Time
    ("clock", 0xF017),
    ("stopwatch", 0xF2F2),
    ("hourglass", 0xF252),  # hourglass-half
    ("calendar", 0xF073),  # calendar-alt
    ("history", 0xF1DA),
    # Weather
    ("sun", 0xF185),
    ("moon", 0xF186),
    ("cloud", 0xF0C2),
    ("cloud_sun", 0xF6C4),
    ("cloud_moon", 0xF6C3),
    ("cloud_rain", 0xF73D),
    ("snowflake", 0xF2DC),
    ("wind", 0xF72E),
    ("thermometer", 0xF2C9),  # thermometer-half
    ("umbrella", 0xF0E9),
    ("fire", 0xF06D),
    # To-dos and notes
    ("tasks", 0xF0AE),
    ("check_circle", 0xF058),
    ("check_square", 0xF14A),
    ("square", 0xF0C8),
    ("sticky_note", 0xF249),
    ("book", 0xF02D),
    ("bookmark", 0xF02E),
    ("flag", 0xF024),
    ("newspaper", 0xF1EA),
    # Status
    ("info", 0xF05A),  # info-circle
    ("question", 0xF059),  # question-circle
    ("exclamation", 0xF06A),  # exclamation-circle
    ("times_circle", 0xF057),
    ("lock", 0xF023),
    ("unlock", 0xF09C),
    # People and feelings
    ("user", 0xF007),
    ("heart", 0xF004),
    ("star", 0xF005),
    ("smile", 0xF118),
    ("frown", 0xF119),
    ("thumbs_up", 0xF164),
    ("trophy", 0xF091),
    ("gift", 0xF06B),
    # Places and things
    ("search", 0xF002),
    ("globe", 0xF0AC),
    ("map_marker", 0xF3C5),  # map-marker-alt
    ("link", 0xF0C1),
    ("share", 0xF1E0),  # share-alt
    ("camera", 0xF030),
    ("gamepad", 0xF11B),
    ("sliders", 0xF1DE),  # sliders-h
    ("filter", 0xF0B0),
    ("calculator", 0xF1EC),
    ("coffee", 0xF0F4),
    ("utensils", 0xF2E7),
    ("shopping_cart", 0xF07A),
    ("car", 0xF1B9),
    ("bed", 0xF236),
    ("running", 0xF70C),
    # Actions
    ("undo", 0xF0E2),
    ("redo", 0xF01E),
]

_SYMBOL = re.compile(r'#define\s+LV_SYMBOL_\w+\s+"((?:\\x[0-9A-Fa-f]{2})+)"')
_GLYPH_COMMENT = re.compile(r"/\* U\+([0-9A-Fa-f]{4,6})\b")
_NAME = re.compile(r"[a-z][a-z0-9_]*\Z")


def lvgl_version(root):
    text = (root / "lv_version.h").read_text(encoding="utf-8")
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+LVGL_VERSION_{name}\s+(\d+)", text)
        if m is None:
            return None
        parts.append(int(m.group(1)))
    return tuple(parts)


def find_lvgl(explicit):
    candidates = [explicit] if explicit is not None else LVGL_CANDIDATES
    pinned = ".".join(map(str, LVGL_VERSION))
    for root in candidates:
        if not (root / SYMBOL_DEF).is_file() or not (root / "lv_version.h").is_file():
            continue
        version = lvgl_version(root)
        if version != LVGL_VERSION:
            found = ".".join(map(str, version)) if version else "unknown"
            sys.exit(f"error: {root} is LVGL {found}, expected the pinned {pinned}")
        return root
    where = str(explicit) if explicit is not None else " or ".join(str(c) for c in LVGL_CANDIDATES)
    sys.exit(
        f"error: no LVGL {pinned} source in {where}; configure the simulator first "
        "(cmake -S simulator -B build/sim -G Ninja) or pass --lvgl"
    )


def symbol_code_points(lvgl_root):
    """The sorted private-use code points of every LV_SYMBOL_* but DUMMY."""
    text = (lvgl_root / SYMBOL_DEF).read_text(encoding="utf-8")
    points = set()
    for m in _SYMBOL.finditer(text):
        raw = bytes(int(h, 16) for h in m.group(1).split("\\x")[1:])
        for ch in raw.decode("utf-8"):
            cp = ord(ch)
            if PUA_FIRST <= cp <= PUA_LAST and cp != SYMBOL_DUMMY:
                points.add(cp)
    if not points:
        sys.exit(f"error: no LV_SYMBOL_* code points found in {lvgl_root / SYMBOL_DEF}")
    return sorted(points)


def validate_icons(icons):
    """Problems with an icon table: bad or repeated names or code points."""
    problems = []
    names = set()
    points = set()
    for name, cp in icons:
        if not _NAME.match(name):
            problems.append(f"bad icon name {name!r}")
        if name in names:
            problems.append(f"icon name {name!r} repeats")
        if not PUA_FIRST <= cp < SYMBOL_DUMMY:
            problems.append(f"{name}: U+{cp:04X} is outside the private use area")
        if cp in points:
            problems.append(f"{name}: U+{cp:04X} repeats")
        names.add(name)
        points.add(cp)
    return problems


def wisp_code_points(lvgl_root):
    problems = validate_icons(WISP_ICONS)
    symbols = set(symbol_code_points(lvgl_root))
    problems += [
        f"{name}: U+{cp:04X} is already an LV_SYMBOL_*" for name, cp in WISP_ICONS if cp in symbols
    ]
    if problems:
        sys.exit("error: WISP_ICONS: " + "; ".join(problems))
    return sorted(cp for _, cp in WISP_ICONS)


def font_name(size):
    return f"lv_font_wisp_icons_{size}"


def output_path(size):
    return FONTS_DIR / f"{font_name(size)}.c"


def code_points(lvgl_root, size):
    """What the font of `size` holds, sorted."""
    wisp = wisp_code_points(lvgl_root)
    if size == BODY_SIZE:
        return wisp
    return sorted(set(symbol_code_points(lvgl_root)) | set(wisp))


def contents(size):
    if size == BODY_SIZE:
        return "the Wisp icons (WISP_SYMBOL_*)"
    return "the LV_SYMBOL_* and Wisp (WISP_SYMBOL_*) icons"


def utf8_escape(cp):
    return "".join(f"\\x{b:02X}" for b in chr(cp).encode("utf-8"))


def macro(name):
    return f"WISP_SYMBOL_{name.upper()}"


def header_text():
    width = max(len(macro(name)) for name, _ in WISP_ICONS)
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/gen_icon_font.py from its WISP_ICONS; do not edit.",
        "// tools/ci.sh fails when it is stale.",
        "//",
        "// Icons LVGL's LV_SYMBOL_* set lacks, as UTF-8 like LV_SYMBOL_*: every",
        "// UI font draws them (the 16 px body font through lv_font_wisp_icons_16,",
        "// the large fonts through lv_font_wisp_icons_{24,32,48}). Font Awesome",
        "// Free 5 glyphs (THIRD_PARTY_NOTICES.md).",
        "",
        "// clang-format off",
    ]
    for name, cp in WISP_ICONS:
        lines.append(f'#define {macro(name):<{width}} "{utf8_escape(cp)}" /* U+{cp:04X} */')
    lines += [
        "",
        "namespace wisp::ui {",
        "",
        "struct WispIcon {",
        "    const char* name;    // ui.icon's `name` in scenes",
        "    const char* symbol;  // the WISP_SYMBOL_* string",
        "};",
        "",
        "// Every Wisp icon, in the order above.",
        "inline constexpr WispIcon kWispIcons[] = {",
    ]
    lines += [f'    {{"{name}", {macro(name)}}},' for name, _ in WISP_ICONS]
    lines += ["};", "// clang-format on", "", "}  // namespace wisp::ui", ""]
    return "\n".join(lines)


def committed_code_points(path):
    return sorted({int(m.group(1), 16) for m in _GLYPH_COMMENT.finditer(path.read_text("utf-8"))})


def check(lvgl_root):
    ok = True
    for size in SIZES:
        path = output_path(size)
        if not path.is_file():
            print(f"error: {path.relative_to(ROOT)} is missing", file=sys.stderr)
            ok = False
            continue
        expected = code_points(lvgl_root, size)
        found = committed_code_points(path)
        if found != expected:
            missing = sorted(set(expected) - set(found))
            extra = sorted(set(found) - set(expected))
            print(
                f"error: {path.relative_to(ROOT)} is stale: missing "
                f"{[hex(c) for c in missing]}, extra {[hex(c) for c in extra]}; "
                "rerun tools/gen_icon_font.py",
                file=sys.stderr,
            )
            ok = False
    if not HEADER.is_file() or HEADER.read_text(encoding="utf-8") != header_text():
        print(
            f"error: {HEADER.relative_to(ROOT)} is stale; rerun tools/gen_icon_font.py",
            file=sys.stderr,
        )
        ok = False
    if not ok:
        sys.exit(1)
    print(
        f"icon fonts match lv_symbol_def.h ({len(symbol_code_points(lvgl_root))} icons) "
        f"and WISP_ICONS ({len(WISP_ICONS)} icons), sizes {SIZES}"
    )


def command(lvgl_root, size, points):
    return [
        "npx", "--yes", LV_FONT_CONV,
        "--font", str(lvgl_root / ICON_FONT),
        "-r", ",".join(f"0x{c:04X}" for c in points),
        "--size", str(size),
        "--format", "lvgl",
        "--bpp", "4",
        "--no-compress",
        "-o", f"{font_name(size)}.c",
        "--lv-include", "lvgl.h",
        "--lv-font-name", font_name(size),
    ]  # fmt: skip


def font_header(lvgl_root, size, cmd):
    # The command as run, with the LVGL path made relative to the LVGL tree
    # so the file does not record where a developer's checkout lives.
    shown = " ".join(cmd).replace(str(lvgl_root) + os.sep, "<lvgl>/")
    pinned = ".".join(map(str, LVGL_VERSION))
    return (
        "/*\n"
        f" * {font_name(size)}: {contents(size)} at {size} px, 4 bpp.\n"
        " * Generated by tools/gen_icon_font.py; do not edit.\n"
        f" * Source: LVGL {pinned} {ICON_FONT}\n"
        f" * License: {LICENSE}\n"
        " * (THIRD_PARTY_NOTICES.md).\n"
        f" * Command: {shown}\n"
        " */\n\n"
    )


def generate(lvgl_root, workdir):
    for size in SIZES:
        points = code_points(lvgl_root, size)
        cmd = command(lvgl_root, size, points)
        subprocess.run(cmd, cwd=workdir, check=True)
        body = (workdir / f"{font_name(size)}.c").read_text(encoding="utf-8")
        # lv_font_conv leaves out a code point the source font lacks.
        missing = sorted(set(points) - set(committed_code_points(workdir / f"{font_name(size)}.c")))
        if missing:
            sys.exit(f"error: {ICON_FONT} has no glyph for {[hex(c) for c in missing]}")
        # lv_font_conv echoes its options, LVGL path included.
        body = body.replace(str(lvgl_root) + os.sep, "<lvgl>/")
        output_path(size).write_text(font_header(lvgl_root, size, cmd) + body, encoding="utf-8")
        print(f"wrote {output_path(size).relative_to(ROOT)}")
    HEADER.write_text(header_text(), encoding="utf-8")
    print(f"wrote {HEADER.relative_to(ROOT)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lvgl", type=pathlib.Path, help="LVGL source directory")
    parser.add_argument("--check", action="store_true", help="only check the committed files")
    parser.add_argument("--workdir", type=pathlib.Path, help="empty directory outside the repo")
    args = parser.parse_args()
    lvgl_root = find_lvgl(args.lvgl)
    if args.check:
        check(lvgl_root)
    elif args.workdir is not None:
        if args.workdir.resolve().is_relative_to(ROOT):
            sys.exit(f"error: the work directory {args.workdir} must be outside the repository")
        args.workdir.mkdir(parents=True, exist_ok=True)
        generate(lvgl_root, args.workdir)
    else:
        with tempfile.TemporaryDirectory(prefix="wisp-icons-") as tmp:
            generate(lvgl_root, pathlib.Path(tmp))


if __name__ == "__main__":
    main()
