#!/usr/bin/env python3
"""Tests for tools/check_i18n.py: runs the script as a subprocess, as
tools/ci.sh does, on scratch dictionary trees and on the real web/app/i18n.

Run directly: python3 tools/check_i18n_test.py
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
SCRIPT = TOOLS / "check_i18n.py"


def run_check(directory):
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(directory)], capture_output=True, text=True
    )


def dictionary(entries):
    lines = ["// comment", "export default {"]
    lines += [f"  {key}: {value}," for key, value in entries]
    lines.append("};")
    return "\n".join(lines) + "\n"


class CheckI18nTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)
        # index.js is code, not a dictionary, and must be ignored.
        (self.dir / "index.js").write_text("import x from './en.js';\n")

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, lang, entries):
        (self.dir / f"{lang}.js").write_text(dictionary(entries), encoding="utf-8")

    def test_matching_dictionaries_pass(self):
        self.write("en", [("'a.b'", "'Hello {name}'"), ("'c'", '"It\'s"')])
        self.write("zh", [("'a.b'", "'你好 {name}'"), ("'c'", "'是'")])
        result = run_check(self.dir)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 keys", result.stdout)

    def test_missing_and_extra_keys_fail(self):
        self.write("en", [("'a'", "'A'"), ("'b'", "'B'")])
        self.write("ja", [("'a'", "'あ'"), ("'z'", "'ぜ'")])
        result = run_check(self.dir)
        self.assertEqual(result.returncode, 1)
        self.assertIn("ja.js: missing key 'b'", result.stderr)
        self.assertIn("ja.js: key 'z' is not in en.js", result.stderr)

    def test_placeholder_mismatch_fails(self):
        self.write("en", [("'a'", "'{count} lines'")])
        self.write("zh", [("'a'", "'{n} 行'")])
        result = run_check(self.dir)
        self.assertEqual(result.returncode, 1)
        self.assertIn("placeholders", result.stderr)

    def test_duplicate_and_empty_fail(self):
        self.write("en", [("'a'", "'A'"), ("'a'", "'B'"), ("'b'", "''")])
        result = run_check(self.dir)
        self.assertEqual(result.returncode, 1)
        self.assertIn("duplicate key 'a'", result.stderr)
        self.assertIn("empty text for 'b'", result.stderr)

    def test_multi_line_entry_fails(self):
        (self.dir / "en.js").write_text("export default {\n  'a':\n    'A',\n};\n")
        result = run_check(self.dir)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not a single-line", result.stderr)

    def test_repository_dictionaries_pass(self):
        result = run_check(ROOT / "web" / "app" / "i18n")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
