#!/usr/bin/env python3
"""Tests for tools/pack_glyph_image.py: the glyphs partition image header
and sections (glyph-partition spec 3.1), its reproducibility, its failure
modes, and that it reproduces the committed `lv_font_cjk_16.c` exactly for
a Latin and a CJK glyph. Most tests cross-check the packed image against
independently-written regex extraction of the `.c` source, not against
pack_glyph_image.py's own parsing, so a shared parsing bug cannot make
both sides agree.

Run directly: python3 tools/pack_glyph_image_test.py
"""

import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
SCRIPT = TOOLS / "pack_glyph_image.py"
COMMITTED_FONT_C = ROOT / "components" / "wisp_ui" / "fonts" / "lv_font_cjk_16.c"
sys.path.insert(0, str(TOOLS))

# The reference board's glyphs partition (boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv).
REFERENCE_GLYPHS_PARTITION = 0x120000

HEADER = struct.Struct("<4sHHII16x")
METRICS = struct.Struct("<HhbbBBHHIIIII")
GLYPH = struct.Struct("<IHBBbb")
CMAP = struct.Struct("<IHHBBHII")
NO_OFFSET = 0xFFFFFFFF

# A source with one SPARSE_FULL cmap and just enough else (font_dsc
# scalars, one glyph, one bitmap byte) for pack() to reach the cmap-type
# check, which runs before kern is ever parsed.
SPARSE_FULL_SOURCE = """
static const lv_font_fmt_txt_dsc_t font_dsc = {
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t lv_font_test = {
    .line_height = 1,
    .base_line = 0,
    .underline_position = 0,
    .underline_thickness = 1,
};

static const uint8_t glyph_bitmap[] = {
    0x0,
};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
};

static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 1, .glyph_id_start = 0,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_FULL
    }
};
"""

# A minimal source carrying only the font_dsc scalar fields pack() reads
# before it ever looks for the array sections -- enough to exercise the
# early rejections without the full committed font.
MINIMAL_REJECTING_SOURCE = """
static const lv_font_fmt_txt_dsc_t font_dsc = {{
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = {bpp},
    .kern_classes = {kern_classes},
    .bitmap_format = {bitmap_format},
}};
"""


def source_glyph_dsc_entries(text):
    """Independent re-parse of glyph_dsc[] used only to build expected
    values; duplicates the shape of pack_glyph_image._parse_glyph_dsc but
    is written separately so the two can disagree if either has a bug."""
    start = text.index("glyph_dsc[] = {")
    end = text.index("\n};", start)
    body = text[start:end]
    entries = re.findall(
        r"\{\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*"
        r"\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*"
        r"\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\}",
        body,
    )
    return [tuple(int(v) for v in e) for e in entries]


def source_bitmap_for_comment(text, codepoint, char):
    """Independent extraction of one glyph's bitmap bytes, anchored on its
    `/* U+XXXX "c" */` comment rather than on array position."""
    marker = f'/* U+{codepoint:04X} "{char}" */'
    start = text.index(marker) + len(marker)
    end = text.index("/*", start)
    chunk = text[start:end]
    tokens = re.findall(r"0[xX][0-9a-fA-F]+", chunk)
    return bytes(int(t, 16) for t in tokens)


def source_resolve_glyph_id(text, codepoint):
    """Independent implementation of the cmap lookup documented in
    lv_font_fmt_txt.h, parsing cmaps[] and the named list arrays straight
    out of the source text rather than through pack_glyph_image.py."""
    cmaps_start = text.index("cmaps[] =")
    cmaps_start = text.index("{", cmaps_start)
    cmaps_end = text.index("\n};", cmaps_start)
    cmaps_body = text[cmaps_start:cmaps_end]
    entries = re.findall(
        r"\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),\s*"
        r"\.glyph_id_start\s*=\s*(\d+),\s*"
        r"\.unicode_list\s*=\s*(\w+),\s*\.glyph_id_ofs_list\s*=\s*(\w+),\s*"
        r"\.list_length\s*=\s*(\d+)",
        cmaps_body,
    )

    def named_array(name):
        m = re.search(rf"\b{name}\[\]\s*=\s*\{{(.*?)\n\}};", text, re.S)
        return [int(t, 0) for t in re.findall(r"0[xX][0-9a-fA-F]+|\d+", m.group(1))]

    for range_start, range_length, glyph_id_start, unicode_list, glyph_id_ofs_list, _ in entries:
        range_start = int(range_start)
        range_length = int(range_length)
        glyph_id_start = int(glyph_id_start)
        if not (range_start <= codepoint < range_start + range_length):
            continue
        rcp = codepoint - range_start
        if unicode_list == "NULL" and glyph_id_ofs_list == "NULL":
            return glyph_id_start + rcp
        if unicode_list == "NULL":
            return glyph_id_start + named_array(glyph_id_ofs_list)[rcp]
        ul = named_array(unicode_list)
        if rcp not in ul:
            return None
        idx = ul.index(rcp)
        if glyph_id_ofs_list == "NULL":
            return glyph_id_start + idx
        return glyph_id_start + named_array(glyph_id_ofs_list)[idx]
    return None


class PackGlyphImageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source_text = COMMITTED_FONT_C.read_text(encoding="utf-8")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = pathlib.Path(cls._tmp.name)
        cls.out = cls.tmp / "glyphs.bin"
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(COMMITTED_FONT_C), "-o", str(cls.out)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"packing the committed font failed: {result.stderr}")
        cls.image = cls.out.read_bytes()
        cls.payload = cls.image[HEADER.size :]
        m = METRICS.unpack_from(cls.payload, 0)
        cls.metrics = dict(
            zip(
                [
                    "line_height",
                    "base_line",
                    "underline_position",
                    "underline_thickness",
                    "bpp",
                    "reserved0",
                    "kern_scale",
                    "cmap_count",
                    "glyph_count",
                    "list_bytes",
                    "kern_pair_count",
                    "bitmap_bytes",
                    "reserved1",
                ],
                m,
            )
        )
        cls.glyphs_offset = METRICS.size
        cls.cmaps_offset = cls.glyphs_offset + GLYPH.size * cls.metrics["glyph_count"]
        cls.lists_offset = cls.cmaps_offset + CMAP.size * cls.metrics["cmap_count"]
        cls.kern_ids_offset = cls.lists_offset + cls.metrics["list_bytes"]
        cls.kern_values_offset = cls.kern_ids_offset + cls.metrics["kern_pair_count"] * 4
        cls.bitmap_offset = cls.kern_values_offset + cls.metrics["kern_pair_count"]

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_pack(self, text, *extra):
        with tempfile.TemporaryDirectory() as d:
            src = pathlib.Path(d) / "in.c"
            src.write_text(text, encoding="utf-8")
            out = pathlib.Path(d) / "glyphs.bin"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(src), "-o", str(out), *extra],
                capture_output=True,
                text=True,
            )
            image = out.read_bytes() if out.exists() else None
            return result, image

    def decode_glyph(self, glyph_id):
        return GLYPH.unpack_from(self.payload, self.glyphs_offset + GLYPH.size * glyph_id)

    def test_header(self):
        magic, version, header_len, payload_len, crc = HEADER.unpack_from(self.image, 0)
        self.assertEqual(magic, b"WGLY")
        self.assertEqual(version, 1)
        self.assertEqual(header_len, 32)
        self.assertEqual(HEADER.size, 32)
        self.assertEqual(payload_len, len(self.payload))
        self.assertEqual(payload_len, len(self.image) - 32)
        self.assertEqual(crc, zlib.crc32(self.payload))
        self.assertEqual(self.image[16:32], bytes(16))

    def test_metrics(self):
        self.assertEqual(self.metrics["line_height"], 31)
        self.assertEqual(self.metrics["base_line"], 9)
        self.assertEqual(self.metrics["underline_position"], -2)
        self.assertEqual(self.metrics["underline_thickness"], 1)
        self.assertEqual(self.metrics["bpp"], 4)
        self.assertEqual(self.metrics["kern_scale"], 16)
        self.assertEqual(self.metrics["cmap_count"], 69)
        self.assertEqual(self.metrics["glyph_count"], 8710)
        self.assertEqual(self.metrics["kern_pair_count"], 4516)
        self.assertEqual(self.metrics["bitmap_bytes"], 996827)

    def test_glyph_and_bitmap_match_source(self):
        for codepoint, char in ((0x0041, "A"), (0x4F60, "你")):
            with self.subTest(codepoint=codepoint):
                glyph_id = source_resolve_glyph_id(self.source_text, codepoint)
                self.assertIsNotNone(glyph_id, f"U+{codepoint:04X} not found in cmaps[]")

                expected = source_glyph_dsc_entries(self.source_text)[glyph_id]
                decoded = self.decode_glyph(glyph_id)
                self.assertEqual(decoded, expected)

                bitmap_index, _, box_w, box_h, _, _ = decoded
                size = -(-(box_w * box_h * self.metrics["bpp"]) // 8)  # ceil
                start = self.bitmap_offset + bitmap_index
                actual_bytes = self.payload[start : start + size]
                expected_bytes = source_bitmap_for_comment(self.source_text, codepoint, char)
                self.assertEqual(actual_bytes, expected_bytes)

    def test_sections_in_bounds(self):
        bitmap_bytes = self.metrics["bitmap_bytes"]
        for glyph_id in range(self.metrics["glyph_count"]):
            bitmap_index, _, box_w, box_h, _, _ = self.decode_glyph(glyph_id)
            size = -(-(box_w * box_h * self.metrics["bpp"]) // 8)
            self.assertLessEqual(
                bitmap_index + size, bitmap_bytes, f"glyph {glyph_id} overruns the bitmap section"
            )

        lists_start = self.lists_offset
        lists_end = lists_start + self.metrics["list_bytes"]
        for i in range(self.metrics["cmap_count"]):
            (
                _range_start,
                _range_length,
                _glyph_id_start,
                cmap_type,
                _reserved,
                list_length,
                unicode_list_offset,
                glyph_id_ofs_offset,
            ) = CMAP.unpack_from(self.payload, self.cmaps_offset + CMAP.size * i)
            if unicode_list_offset != NO_OFFSET:
                self.assertGreaterEqual(unicode_list_offset, lists_start)
                self.assertLessEqual(unicode_list_offset + list_length * 2, lists_end)
            if glyph_id_ofs_offset != NO_OFFSET:
                # FORMAT0_FULL (0) stores 8-bit entries, SPARSE_FULL (1) 16-bit ones.
                width = 1 if cmap_type == 0 else 2
                self.assertGreaterEqual(glyph_id_ofs_offset, lists_start)
                self.assertLessEqual(glyph_id_ofs_offset + list_length * width, lists_end)

    def test_deterministic(self):
        result, image_a = self.run_pack(self.source_text)
        self.assertEqual(result.returncode, 0, result.stderr)
        result, image_b = self.run_pack(self.source_text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(image_a, image_b)

    def test_max_bytes(self):
        result, _ = self.run_pack(self.source_text, "--max-bytes", "1000")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(result.stderr.strip())
        result, _ = self.run_pack(self.source_text, "--max-bytes", str(REFERENCE_GLYPHS_PARTITION))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_committed_font_fits_reference_partition(self):
        self.assertLessEqual(len(self.image), REFERENCE_GLYPHS_PARTITION)

    def test_rejects_kern_classes(self):
        src = MINIMAL_REJECTING_SOURCE.format(bpp=4, kern_classes=1, bitmap_format=0)
        result, _ = self.run_pack(src)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("kern class", result.stderr)

    def test_rejects_compressed_bitmaps(self):
        src = MINIMAL_REJECTING_SOURCE.format(bpp=4, kern_classes=0, bitmap_format=1)
        result, _ = self.run_pack(src)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bitmap_format", result.stderr)

    def test_rejects_sparse_full_cmaps(self):
        result, _ = self.run_pack(SPARSE_FULL_SOURCE)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SPARSE_FULL", result.stderr)


    def with_glyph_1(self, **fields):
        """The committed source with glyph 1 (an empty glyph) changed."""
        line = "{.bitmap_index = 0, .adv_w = 57, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"
        self.assertEqual(self.source_text.count(line), 1)
        values = {"bitmap_index": 0, "adv_w": 57}
        values.update(fields)
        changed = (
            f"{{.bitmap_index = {values['bitmap_index']}, .adv_w = {values['adv_w']}, "
            ".box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},"
        )
        return self.source_text.replace(line, changed)

    def test_rejects_adv_w_past_bitfield(self):
        result, _ = self.run_pack(self.with_glyph_1(adv_w=4096))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("12-bit", result.stderr)
        result, _ = self.run_pack(self.with_glyph_1(adv_w=4095))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_bitmap_index_past_bitfield(self):
        result, _ = self.run_pack(self.with_glyph_1(bitmap_index=2**20))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("20-bit", result.stderr)

    def test_rejects_empty_glyph_index_at_bitmap_end(self):
        bitmap_bytes = self.metrics["bitmap_bytes"]
        result, _ = self.run_pack(self.with_glyph_1(bitmap_index=bitmap_bytes))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("past glyph_bitmap[]", result.stderr)
        result, _ = self.run_pack(self.with_glyph_1(bitmap_index=bitmap_bytes - 1))
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
