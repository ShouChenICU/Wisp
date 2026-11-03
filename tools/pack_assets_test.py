#!/usr/bin/env python3
"""Tests for tools/pack_assets.py: packs tests/net/fixtures/web (and small
scratch trees) by running the script as a subprocess, exactly as CMake and
the firmware build do, and checks the WispPack bytes and the failure modes.

Run directly: python3 tools/pack_assets_test.py
"""

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
SCRIPT = TOOLS / "pack_assets.py"
FIXTURES = ROOT / "tests" / "net" / "fixtures" / "web"

HEADER = struct.Struct("<4sHHII")
RECORD_TAIL = struct.Struct("<BII32s")


def run_pack(src, out, *extra):
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(src), "-o", str(out), *extra],
        capture_output=True,
        text=True,
    )


def parse_table(blob):
    magic, version, count, table_bytes, total_bytes = HEADER.unpack_from(blob, 0)
    pos = HEADER.size
    records = []
    for _ in range(count):
        (path_len,) = struct.unpack_from("<H", blob, pos)
        pos += 2
        path = blob[pos : pos + path_len].decode()
        pos += path_len
        mime_len = blob[pos]
        pos += 1
        mime = blob[pos : pos + mime_len].decode()
        pos += mime_len
        flags, offset, length, digest = RECORD_TAIL.unpack_from(blob, pos)
        pos += RECORD_TAIL.size
        records.append((path, mime, flags, offset, length, digest))
    return magic, version, count, table_bytes, total_bytes, pos, records


class PackAssetsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def pack_fixtures(self, name="out.wpak", *extra):
        out = self.tmp / name
        proc = run_pack(FIXTURES, out, *extra)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return out.read_bytes()

    def test_roundtrip_header(self):
        import gzip
        import hashlib

        blob = self.pack_fixtures()
        magic, version, count, table_bytes, total_bytes, table_end, records = parse_table(blob)
        self.assertEqual(magic, b"WPAK")
        self.assertEqual(version, 1)
        self.assertEqual(count, 4)
        self.assertEqual(table_end, HEADER.size + table_bytes)
        self.assertEqual(total_bytes, len(blob))
        by_path = {r[0]: r for r in records}
        self.assertEqual(
            sorted(by_path), ["/app/main.js", "/app/util.js", "/index.html", "/logo.png"]
        )
        for path, mime, flags, offset, length, digest in records:
            with self.subTest(path=path):
                self.assertEqual(offset % 4, 0)
                self.assertGreaterEqual(offset, table_end)
                self.assertLessEqual(offset + length, total_bytes)
                data = blob[offset : offset + length]
                self.assertEqual(hashlib.sha256(data).digest(), digest)
                source = (FIXTURES / path.lstrip("/")).read_bytes()
                if flags & 1:
                    self.assertEqual(gzip.decompress(data), source)
                else:
                    self.assertEqual(data, source)
        self.assertEqual(by_path["/index.html"][1], "text/html; charset=utf-8")
        self.assertEqual(by_path["/index.html"][2] & 1, 1)
        self.assertEqual(by_path["/logo.png"][1], "image/png")
        self.assertEqual(by_path["/logo.png"][2] & 1, 0)

    def test_reproducible(self):
        first = self.pack_fixtures("a.wpak")
        second = self.pack_fixtures("b.wpak")
        self.assertEqual(first, second)

    def test_missing_import_fails(self):
        src = self.tmp / "src"
        src.mkdir()
        (src / "main.js").write_text("import { x } from './nope.js';\n")
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("nope.js", proc.stdout + proc.stderr)

    def test_missing_dynamic_import_fails(self):
        src = self.tmp / "src"
        (src / "app").mkdir(parents=True)
        (src / "app" / "a.js").write_text("const m = await import('../gone.mjs');\n")
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("gone.mjs", proc.stdout + proc.stderr)

    def test_html_reference_outside_pack_fails(self):
        src = self.tmp / "src"
        src.mkdir()
        (src / "a.js").write_text("export const a = 1;\n")
        (src / "page.html").write_text(
            '<link rel="stylesheet" href="/gone.css">\n'
            '<script type="module" src="./a.js"></script>\n'
        )
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("gone.css", proc.stdout + proc.stderr)
        self.assertNotIn("a.js", proc.stdout + proc.stderr)

    def test_html_reference_to_another_host_fails(self):
        src = self.tmp / "src"
        src.mkdir()
        (src / "page.html").write_text('<script src="https://cdn.example.com/x.js"></script>\n')
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cdn.example.com", proc.stdout + proc.stderr)

    def test_unquoted_html_reference_is_checked(self):
        src = self.tmp / "src"
        src.mkdir()
        (src / "a.js").write_text("export const a = 1;\n")
        (src / "page.html").write_text(
            "<link rel=stylesheet href=/gone.css>\n"
            "<script type=module src=./a.js></script>\n"
        )
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("gone.css", proc.stdout + proc.stderr)
        self.assertNotIn("a.js", proc.stdout + proc.stderr)

    def test_protocol_relative_html_reference_fails(self):
        for attr in ('"//cdn.example.com/x.js"', "//cdn.example.com/x.js"):
            with self.subTest(attr=attr):
                src = self.tmp / "src"
                shutil.rmtree(src, ignore_errors=True)
                src.mkdir()
                (src / "page.html").write_text(f"<script src={attr}></script>\n")
                proc = run_pack(src, self.tmp / "out.wpak")
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn("//cdn.example.com/x.js", proc.stdout + proc.stderr)

    def test_html_data_and_local_references_pass(self):
        src = self.tmp / "src"
        (src / "app").mkdir(parents=True)
        (src / "app" / "a.js").write_text("export const a = 1;\n")
        (src / "page.html").write_text(
            '<link rel="icon" href="data:image/svg+xml,%3Csvg%3E">\n'
            '<script type="module" src="/app/a.js?v=1"></script>\n'
            '<a href="https://example.com/">plain links are not loaded</a>\n'
        )
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_partition_overflow_fails(self):
        csv = self.tmp / "partitions.csv"
        csv.write_text(
            "# Name, Type, SubType, Offset, Size\n"
            "nvs,    data, nvs,     0x9000, 0x6000\n"
            "assets, data, spiffs,  0x10000, 0x100\n"
        )
        proc = run_pack(FIXTURES, self.tmp / "out.wpak", "--partitions", str(csv))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("partition", proc.stdout + proc.stderr)

    def test_partition_that_fits_passes(self):
        csv = self.tmp / "partitions.csv"
        csv.write_text("assets, data, spiffs, 0x10000, 1M\n")
        self.pack_fixtures("out.wpak", "--partitions", str(csv))

    def test_budget_overflow_fails(self):
        src = self.tmp / "src"
        src.mkdir()
        # Random hex barely compresses, so 4 KiB of it stays well over 1 KiB.
        import os

        (src / "big.txt").write_text(os.urandom(4096).hex())
        proc = run_pack(src, self.tmp / "out.wpak", "--budget-kb", "1")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("budget", proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
