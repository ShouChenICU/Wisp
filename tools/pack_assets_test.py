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
sys.path.insert(0, str(TOOLS))
import pack_assets  # noqa: E402
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
                    self.assertEqual(gzip.decompress(data), pack_assets.minify(path, source))
                else:
                    self.assertEqual(data, source)
        self.assertEqual(by_path["/index.html"][1], "text/html; charset=utf-8")
        self.assertEqual(by_path["/index.html"][2] & 1, 1)
        self.assertEqual(by_path["/logo.png"][1], "image/png")
        self.assertEqual(by_path["/logo.png"][2] & 1, 0)

    def test_js_loses_indentation_blank_lines_and_comment_lines(self):
        src = (
            "// leading comment\n"
            "import { a } from './a.js';\n"
            "\n"
            "export function f(x) {\n"
            "    // inside\n"
            "    const u = 'http://x'; // trailing comment stays\n"
            "\t  return x / 2;\n"
            "}\n"
            "/** block comments\n"
            "    stay as they are */\n"
        )
        self.assertEqual(
            pack_assets.minify_js(src),
            "import { a } from './a.js';\n"
            "export function f(x) {\n"
            "const u = 'http://x'; // trailing comment stays\n"
            "return x / 2;\n"
            "}\n"
            "/** block comments\n"
            "    stay as they are */",
        )

    def test_js_template_literals_keep_every_line(self):
        src = (
            "const t = html`\n"
            "  <pre>\n"
            "    // not a comment\n"
            "\n"
            "  ${items.map((i) => html`\n"
            "      <li>${i}</li>`)}\n"
            "  ${x ? `\n"
            "    a` : y}\n"
            "  </pre>`;\n"
            "  // after\n"
            "  done();\n"
        )
        self.assertEqual(
            pack_assets.minify_js(src),
            "const t = html`\n"
            "  <pre>\n"
            "    // not a comment\n"
            "\n"
            "  ${items.map((i) => html`\n"
            "      <li>${i}</li>`)}\n"
            "  ${x ? `\n"
            "    a` : y}\n"
            "  </pre>`;\n"
            "done();",
        )

    def test_js_strings_regexes_and_comments_do_not_open_templates(self):
        src = (
            "const a = '`'; const b = \"`\"; /* ` */ // `\n"
            "  const r = /[`/]`/g; const d = n / 2 / 1;\n"
            "  const k = s.replace(/\\//g, '');\n"
            "  if (ok) return /`/.test(s);\n"
            "  next();\n"
        )
        self.assertEqual(
            pack_assets.minify_js(src),
            "const a = '`'; const b = \"`\"; /* ` */ // `\n"
            "const r = /[`/]`/g; const d = n / 2 / 1;\n"
            "const k = s.replace(/\\//g, '');\n"
            "if (ok) return /`/.test(s);\n"
            "next();",
        )

    def test_js_string_continued_over_a_line_keeps_that_line(self):
        src = "const s = 'a\\\n    b';\n    x();\n"
        self.assertEqual(pack_assets.minify_js(src), "const s = 'a\\\n    b';\nx();")

    def test_js_regex_after_a_paren_does_not_unpin_templates(self):
        # The scanner reads this regex as a division and its backtick as the
        # start of a template; it must not then rewrite the real template.
        src = (
            "if (ok) /`/.test(s);\n"
            "const t = html`\n"
            "  <pre>\n"
            "    // x\n"
            "  </pre>`;\n"
            "  done();\n"
        )
        out = pack_assets.minify_js(src)
        start = src.index("html`")
        self.assertIn(src[start:src.index("`;", start) + 2], out)
        self.assertEqual(out, src)

    def test_js_division_after_postfix_increment(self):
        src = "a = b++ / 2;\n  c = --d;\n  e();\n"
        self.assertEqual(pack_assets.minify_js(src), "a = b++ / 2;\nc = --d;\ne();")

    def test_js_division_read_as_a_regex_leaves_the_file_as_is(self):
        # A '/' after '}' is taken for a regex; at the newline the scanner
        # knows it lost track.
        src = "x = {}\n/ 2;\n  y();\n"
        self.assertEqual(pack_assets.minify_js(src), src)

    def test_js_that_ends_inside_a_string_or_template_is_left_as_is(self):
        for src in ("  a();\nconst s = 'open \\\n", "  a();\nconst t = `\n  x\n"):
            with self.subTest(src=src):
                self.assertEqual(pack_assets.minify_js(src), src)

    def test_css_loses_comments_indentation_and_blank_lines(self):
        # A comment between two tokens that would otherwise touch stays, empty.
        src = (
            "/* tokens */\n"
            ".a {\n"
            "  color: red; /* why */\n"
            "\n"
            "  content: \"/* kept */\";\n"
            "  background: url('a/*b*/c.png');\n"
            "}\n"
            ".b/* between */.c { margin: 0 }\n"
        )
        self.assertEqual(
            pack_assets.minify_css(src),
            ".a {\n"
            "color: red;\n"
            "content: \"/* kept */\";\n"
            "background: url('a/*b*/c.png');\n"
            "}\n"
            ".b/**/.c { margin: 0 }",
        )

    def test_only_js_and_css_are_minified(self):
        html = b"<p>\n  <b>x</b>\n</p>\n"
        self.assertEqual(pack_assets.minify("/index.html", html), html)
        self.assertEqual(pack_assets.minify("/a.json", b'{\n  "a": 1\n}\n'), b'{\n  "a": 1\n}\n')
        self.assertEqual(pack_assets.minify("/a.mjs", b"  x();\n"), b"x();")
        self.assertEqual(pack_assets.minify("/a.css", b"  a{}\n"), b"a{}")

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

    def test_pack_over_256_kib_fails(self):
        src = self.tmp / "src"
        src.mkdir()
        # Already-compressed formats are stored as-is.
        import os

        (src / "big.png").write_bytes(os.urandom(256 * 1024))
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("256 KiB", proc.stdout + proc.stderr)
        self.assertFalse((self.tmp / "out.wpak").exists())

    def test_pack_under_256_kib_passes(self):
        src = self.tmp / "src"
        src.mkdir()
        import os

        (src / "big.png").write_bytes(os.urandom(200 * 1024))
        proc = run_pack(src, self.tmp / "out.wpak")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

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
