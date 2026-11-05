#!/usr/bin/env python3
"""Tests for tools/pack_font_image.py: the fonts partition image header
(vector-font spec 3.2), its reproducibility and its failure modes. The
script runs as a subprocess, exactly as CMake runs it.

Run directly: python3 tools/pack_font_image_test.py
"""

import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
SCRIPT = TOOLS / "pack_font_image.py"
COMMITTED_TTF = ROOT / "components" / "wisp_ui" / "fonts" / "noto_sans_cjk_sc_subset.ttf"
sys.path.insert(0, str(TOOLS))

# The reference board's fonts partition (boards/waveshare_esp32_s3_touch_amoled_2_16/partitions.csv).
REFERENCE_FONTS_PARTITION = 0x2D0000

HEADER = struct.Struct("<4sHHII16x")

# Not a real font: the packer only looks at the sfnt version tag.
FAKE_TTF = b"\x00\x01\x00\x00" + bytes(range(256)) * 4


class PackFontImageTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def run_pack(self, ttf_bytes, *extra):
        src = self.tmp / "in.ttf"
        src.write_bytes(ttf_bytes)
        out = self.tmp / "fonts.bin"
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(src), "-o", str(out), *extra],
            capture_output=True,
            text=True,
        )
        return result, out

    def test_header_fields(self):
        result, out = self.run_pack(FAKE_TTF)
        self.assertEqual(result.returncode, 0, result.stderr)
        blob = out.read_bytes()
        magic, version, header_len, length, crc = HEADER.unpack_from(blob, 0)
        self.assertEqual(magic, b"WFNT")
        self.assertEqual(version, 1)
        self.assertEqual(header_len, 32)
        self.assertEqual(HEADER.size, 32)
        self.assertEqual(length, len(FAKE_TTF))
        self.assertEqual(crc, zlib.crc32(FAKE_TTF))
        self.assertEqual(blob[16:32], bytes(16))
        self.assertEqual(blob[32:], FAKE_TTF)

    def test_reproducible(self):
        _, out = self.run_pack(FAKE_TTF)
        first = out.read_bytes()
        result, out = self.run_pack(FAKE_TTF)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(out.read_bytes(), first)

    def test_rejects_cff(self):
        result, _ = self.run_pack(b"OTTO" + bytes(64))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CFF", result.stderr)

    def test_rejects_non_sfnt(self):
        result, _ = self.run_pack(b"\x00\x00\x00\x00" + bytes(64))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(result.stderr.strip())

    def test_max_bytes(self):
        image_len = 32 + len(FAKE_TTF)
        result, _ = self.run_pack(FAKE_TTF, "--max-bytes", str(image_len - 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(image_len), result.stderr)
        self.assertIn(str(image_len - 1), result.stderr)
        result, _ = self.run_pack(FAKE_TTF, "--max-bytes", str(image_len))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_max_bytes_accepts_hex(self):
        # partition_table_get_partition_info() reports sizes as hex.
        image_len = 32 + len(FAKE_TTF)
        result, _ = self.run_pack(FAKE_TTF, "--max-bytes", hex(image_len))
        self.assertEqual(result.returncode, 0, result.stderr)
        result, _ = self.run_pack(FAKE_TTF, "--max-bytes", hex(image_len - 1))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(str(image_len - 1), result.stderr)

    def test_committed_font_fits_reference_partition(self):
        import pack_font_image

        image = pack_font_image.pack(COMMITTED_TTF.read_bytes())
        self.assertEqual(image[:4], b"WFNT")
        self.assertLessEqual(len(image), REFERENCE_FONTS_PARTITION)


if __name__ == "__main__":
    unittest.main()
