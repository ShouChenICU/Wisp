#!/usr/bin/env python3
"""Generates components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf, the
vector CJK+Latin font behind the device UI's large text sizes (spec
3.1): a TrueType (glyf) subset of Noto Sans CJK SC Regular, with its
CFF outlines converted to quadratic and its hinting stripped.

Character set (deduplicated, sorted by code point) -- see
vector_charset():
  - every code point of the built-in 16 px bitmap font
    (gen_cjk_font.build_charset()[0]: ASCII, GB2312, JIS X 0208 rows 1-8
    and 16-47, U+3000-U+303F, U+FF01-U+FF5E, plus the device UI string
    tables);
  - plus LATIN_EXTRA (the Latin-1 Supplement U+00A0-U+00FF, and the
    punctuation ranges U+2010-U+2027, U+2030-U+203A, U+20AC, U+2122),
    minus LATIN_ABSENT_FROM_SOURCE: the few code points in those ranges
    the source font has no glyph for.

Glyphs come from the same pinned upstream Noto Sans CJK SC Regular as
gen_cjk_font.py (SIL Open Font License 1.1); fetch_font() is reused so
both scripts download and verify the exact same file.

fontTools subsets the font (the arguments spec 3.1 pins, run in-process
via fontTools.subset.main() rather than shelling out to pyftsubset),
then the CFF outlines are converted to TrueType glyf outlines and the
hinting/name tables are stripped. fontTools must be exactly 4.55.3;
`import fontTools` only happens inside main(), so this module stays
importable with just the standard library for gen_vector_font_test.py.

Usage:
  gen_vector_font.py --workdir <new empty dir outside the repo>
                      [--font <already downloaded OTF>] [--output <path>]
"""

import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf"

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_cjk_font  # noqa: E402  (same directory; reuses build_charset()/fetch_font())

FONTTOOLS_VERSION = "4.55.3"

# The Latin-1 Supplement (the source font has every code point in it)
# plus the punctuation ranges spec 3.1 lists.
LATIN_EXTRA = (
    {chr(c) for c in range(0x00A0, 0x0100)}
    | {chr(c) for c in range(0x2010, 0x2028)}
    | {chr(c) for c in range(0x2030, 0x203B)}
    | {"€", "™"}
)

# Code points in LATIN_EXTRA the source font has no glyph for (verified
# against the pinned Sans2.004 release); excluded rather than failing
# the whole run. main() re-verifies this against the real subset's
# cmap, so a source update that fills or widens this gap is caught
# instead of silently staying wrong.
LATIN_ABSENT_FROM_SOURCE = {
    "‗",  # DOUBLE LOW LINE
    "‛",  # SINGLE HIGH-REVERSED-9 QUOTATION MARK
    "‟",  # DOUBLE HIGH-REVERSED-9 QUOTATION MARK
    "‣",  # TRIANGULAR BULLET
    "․",  # ONE DOT LEADER
    "‱",  # PER TEN THOUSAND SIGN
    "‴",  # TRIPLE PRIME
    "‶",  # REVERSED DOUBLE PRIME
    "‷",  # REVERSED TRIPLE PRIME
    "‸",  # CARET
}


def vector_charset():
    """The full character set of the committed font: the 16 px bitmap
    font's set plus LATIN_EXTRA, minus the code points the source font
    lacks. Only the standard library, so this is importable offline."""
    base, _extras = gen_cjk_font.build_charset()
    return sorted(set(base) | (LATIN_EXTRA - LATIN_ABSENT_FROM_SOURCE), key=ord)


def _check_fonttools_version():
    try:
        import fontTools
    except ImportError:
        fontTools = None
    version = getattr(fontTools, "version", None)
    if version != FONTTOOLS_VERSION:
        sys.exit(
            f"error: fontTools must be exactly {FONTTOOLS_VERSION} (found {version!r}); install it "
            f"in a venv: python3 -m venv <dir> && <dir>/bin/pip install fonttools=={FONTTOOLS_VERSION}"
        )


def _subset(font_path, unicodes_path, output_path):
    """Runs fontTools' subsetter in-process with the arguments spec 3.1
    pins (the equivalent pyftsubset command line is spelled out there).
    Keeps the `kern` feature's GPOS (the PairPos format stb_truetype
    reads); drops vertical/ligature tables and glyph names it can't use."""
    from fontTools import subset

    subset.main(
        [
            str(font_path),
            f"--unicodes-file={unicodes_path}",
            f"--output-file={output_path}",
            "--layout-features=kern",
            "--no-hinting",
            "--desubroutinize",
            "--drop-tables+=GSUB,vhea,vmtx,VORG,DSIG",
            "--notdef-outline",
            "--name-IDs=0,1,2,3,4,5,6,13,14",
            "--name-languages=1033",
        ]
    )


def _cff_to_truetype(subset_path):
    """Loads the CFF-outline subset and converts it to TrueType glyf
    outlines, returning the mutated TTFont (not yet saved). Adapted
    from fontTools Snippets/otf2ttf.py (MIT): quadratic conversion via
    Cu2QuPen/TTGlyphPen (error 1.0 font unit, imperceptible), then a
    fresh maxp (version 1.0, instruction-related maxima zeroed except
    maxZones=1, since there is no hinting left to size for) and the
    TrueType sfnt version."""
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib import TTFont, newTable

    font = TTFont(str(subset_path))
    glyph_set = font.getGlyphSet()
    glyph_order = font.getGlyphOrder()
    glyphs = {}
    for name in glyph_order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(Cu2QuPen(pen, 1.0, reverse_direction=True))
        glyphs[name] = pen.glyph()

    font["loca"] = newTable("loca")
    font["glyf"] = glyf = newTable("glyf")
    glyf.glyphOrder = glyph_order
    glyf.glyphs = glyphs

    del font["CFF "]
    if "VORG" in font:
        del font["VORG"]

    maxp = font["maxp"] = newTable("maxp")
    maxp.tableVersion = 0x00010000
    for attr in (
        "maxZones",
        "maxTwilightPoints",
        "maxStorage",
        "maxFunctionDefs",
        "maxInstructionDefs",
        "maxStackElements",
        "maxSizeOfInstructions",
        "maxComponentElements",
    ):
        setattr(maxp, attr, 0)
    maxp.maxZones = 1
    # numGlyphs and the point/contour maxima are recalculated from the
    # new glyf table when the font is compiled (table__m_a_x_p.compile()
    # calls recalc() because "glyf" is loaded and recalcBBoxes is on by
    # default); only the instruction-related fields above need setting.

    font.sfntVersion = "\000\001\000\000"
    return font


def _check_cmap_covers(font, chars):
    cmap = font.getBestCmap()
    wanted = {ord(c) for c in chars}
    missing = sorted(wanted - set(cmap))
    if missing:
        listed = " ".join(f"U+{cp:04X}" for cp in missing[:40])
        sys.exit(f"error: the subset font has no glyph for {len(missing)} code points: {listed}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--workdir", type=pathlib.Path, required=True)
    parser.add_argument("--font", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=OUTPUT)
    args = parser.parse_args(argv)

    chars = vector_charset()
    print(f"{len(chars)} code points", file=sys.stderr)

    _check_fonttools_version()

    workdir = args.workdir.resolve()
    if workdir == ROOT or ROOT in workdir.parents:
        parser.error("--workdir must be outside the repository")
    if workdir.exists() and any(workdir.iterdir()):
        parser.error("--workdir must be a new or empty directory")
    workdir.mkdir(parents=True, exist_ok=True)

    gen_cjk_font.fetch_font(workdir, args.font)

    unicodes_path = workdir / "unicodes.txt"
    unicodes_path.write_text(" ".join(f"U+{ord(c):04X}" for c in chars), encoding="ascii")

    subset_path = workdir / "subset.otf"
    _subset(workdir / gen_cjk_font.FONT_FILE, unicodes_path, subset_path)

    font = _cff_to_truetype(subset_path)
    font["post"].formatType = 3.0

    _check_cmap_covers(font, chars)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    font.save(str(args.output))
    print(f"wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
