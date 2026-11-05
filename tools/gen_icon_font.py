#!/usr/bin/env python3
"""Generates components/wisp_ui/fonts/lv_font_wisp_icons_{24,32,48}.c, the
icon-only bitmap fonts behind the large UI fonts (vector-font spec 4.6).

The vector font (Noto Sans CJK) has no LVGL icons (LV_SYMBOL_*, FontAwesome glyphs in
the Unicode private use area), so ui::font() falls back to one of these for
them. They hold exactly the LV_SYMBOL_* code points of the pinned LVGL's
src/font/lv_symbol_def.h inside U+E000..U+F8FF, without LV_SYMBOL_DUMMY
(U+F8FF, which LVGL draws as an empty glyph itself): the same icons LVGL's
own Montserrat fonts carry, so no icon changes.

The glyphs come from the FontAwesome subset LVGL ships for its built-in
fonts (scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff), read
from the LVGL source tree the build already fetched, converted by the pinned
lv_font_conv through npx. Font Awesome Free's fonts are under the SIL OFL
1.1 with the Reserved Font Name "Font Awesome"; these subsets are modified
versions, so their names do not use it (THIRD_PARTY_NOTICES.md).

Usage:
  gen_icon_font.py [--lvgl <LVGL source dir>] [--workdir <empty dir>]
      Regenerates the three files. Needs npx and, the first time, network
      access to fetch lv_font_conv. The work directory (a new temporary one
      by default) must be outside the repository.
  gen_icon_font.py [--lvgl <LVGL source dir>] --check
      Offline: fails when the code points in the committed files differ
      from the current lv_symbol_def.h.
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
SIZES = (24, 32, 48)
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

_SYMBOL = re.compile(r'#define\s+LV_SYMBOL_\w+\s+"((?:\\x[0-9A-Fa-f]{2})+)"')
_GLYPH_COMMENT = re.compile(r"/\* U\+([0-9A-Fa-f]{4,6})\b")


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


def font_name(size):
    return f"lv_font_wisp_icons_{size}"


def output_path(size):
    return FONTS_DIR / f"{font_name(size)}.c"


def committed_code_points(path):
    return sorted({int(m.group(1), 16) for m in _GLYPH_COMMENT.finditer(path.read_text("utf-8"))})


def check(lvgl_root):
    expected = symbol_code_points(lvgl_root)
    ok = True
    for size in SIZES:
        path = output_path(size)
        if not path.is_file():
            print(f"error: {path.relative_to(ROOT)} is missing", file=sys.stderr)
            ok = False
            continue
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
    if not ok:
        sys.exit(1)
    print(f"icon fonts match lv_symbol_def.h ({len(expected)} icons, sizes {SIZES})")


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


def header(lvgl_root, size, cmd):
    # The command as run, with the LVGL path made relative to the LVGL tree
    # so the file does not record where a developer's checkout lives.
    shown = " ".join(cmd).replace(str(lvgl_root) + os.sep, "<lvgl>/")
    pinned = ".".join(map(str, LVGL_VERSION))
    return (
        "/*\n"
        f" * {font_name(size)}: the LV_SYMBOL_* icons at {size} px, 4 bpp.\n"
        " * Generated by tools/gen_icon_font.py; do not edit.\n"
        f" * Source: LVGL {pinned} {ICON_FONT}\n"
        f" * License: {LICENSE}\n"
        " * (THIRD_PARTY_NOTICES.md).\n"
        f" * Command: {shown}\n"
        " */\n\n"
    )


def generate(lvgl_root, workdir):
    points = symbol_code_points(lvgl_root)
    for size in SIZES:
        cmd = command(lvgl_root, size, points)
        subprocess.run(cmd, cwd=workdir, check=True)
        body = (workdir / f"{font_name(size)}.c").read_text(encoding="utf-8")
        # lv_font_conv echoes its options, LVGL path included.
        body = body.replace(str(lvgl_root) + os.sep, "<lvgl>/")
        output_path(size).write_text(header(lvgl_root, size, cmd) + body, encoding="utf-8")
        print(f"wrote {output_path(size).relative_to(ROOT)}")


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
