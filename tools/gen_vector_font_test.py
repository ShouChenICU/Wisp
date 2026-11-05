#!/usr/bin/env python3
"""Tests for tools/gen_vector_font.py's character set and for the
committed TrueType font matching it. Offline: nothing is downloaded and
fontTools is not imported -- the sfnt table directory and the cmap are
parsed by hand instead, so this file never needs the pinned fontTools
version to check the pinned fontTools version's own output.

Run directly: python3 tools/gen_vector_font_test.py
"""

import pathlib
import struct
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import gen_cjk_font  # noqa: E402
import gen_vector_font  # noqa: E402


def read_sfnt(data):
    """Returns (sfnt version bytes, {tag: (offset, length)}) from the
    sfnt header and table directory (OpenType spec)."""
    version = data[:4]
    num_tables = struct.unpack(">H", data[4:6])[0]
    tables = {}
    pos = 12
    for _ in range(num_tables):
        tag, _checksum, offset, length = struct.unpack(">4sIII", data[pos : pos + 16])
        tables[tag.decode("latin-1")] = (offset, length)
        pos += 16
    return version, tables


def _format4_codepoints(data, offset):
    seg_count = struct.unpack(">H", data[offset + 6 : offset + 8])[0] // 2
    end_base = offset + 14
    start_base = end_base + seg_count * 2 + 2
    chars = set()
    for i in range(seg_count):
        end = struct.unpack(">H", data[end_base + i * 2 : end_base + i * 2 + 2])[0]
        start = struct.unpack(">H", data[start_base + i * 2 : start_base + i * 2 + 2])[0]
        if (start, end) != (0xFFFF, 0xFFFF):
            chars.update(range(start, end + 1))
    return chars


def _format12_codepoints(data, offset):
    num_groups = struct.unpack(">I", data[offset + 12 : offset + 16])[0]
    base = offset + 16
    chars = set()
    for i in range(num_groups):
        start, end, _glyph = struct.unpack(">III", data[base + i * 12 : base + i * 12 + 12])
        chars.update(range(start, end + 1))
    return chars


def read_cmap_codepoints(data, tables):
    """The union of every format 4 / format 12 cmap subtable's code
    points -- the two formats stb_truetype (the firmware's renderer)
    reads, so other subtables (e.g. format 0) don't matter here."""
    cmap_offset, _length = tables["cmap"]
    num_subtables = struct.unpack(">H", data[cmap_offset + 2 : cmap_offset + 4])[0]
    chars = set()
    for i in range(num_subtables):
        rec = cmap_offset + 4 + i * 8
        _platform_id, _encoding_id, sub_offset = struct.unpack(">HHI", data[rec : rec + 8])
        sub_offset += cmap_offset
        fmt = struct.unpack(">H", data[sub_offset : sub_offset + 2])[0]
        if fmt == 4:
            chars |= _format4_codepoints(data, sub_offset)
        elif fmt == 12:
            chars |= _format12_codepoints(data, sub_offset)
    return chars


class Charset(unittest.TestCase):
    def test_vector_charset_extends_bitmap_charset(self):
        base, _extras = gen_cjk_font.build_charset()
        chars = gen_vector_font.vector_charset()
        self.assertTrue(set(base) <= set(chars))
        for c in "ÀÅéü€™–—‰":
            self.assertIn(c, chars)
        self.assertNotIn("‗", chars)
        self.assertEqual(chars, sorted(set(chars), key=ord))


class CommittedFont(unittest.TestCase):
    """The checked-in subset TTF must be a hinting-free TrueType font
    whose cmap matches the script's current character set."""

    def setUp(self):
        self.data = gen_vector_font.OUTPUT.read_bytes()
        self.version, self.tables = read_sfnt(self.data)

    def test_is_truetype_without_hinting(self):
        self.assertEqual(self.version, b"\x00\x01\x00\x00")
        for tag in ("glyf", "loca", "cmap", "head", "hhea", "hmtx", "maxp", "GPOS", "name"):
            self.assertIn(tag, self.tables)
        for tag in ("CFF ", "fpgm", "prep", "cvt ", "GSUB"):
            self.assertNotIn(tag, self.tables)

    def test_cmap_matches_charset(self):
        codepoints = read_cmap_codepoints(self.data, self.tables)
        chars = gen_vector_font.vector_charset()
        self.assertEqual(codepoints, {ord(c) for c in chars})


if __name__ == "__main__":
    unittest.main()
