#!/usr/bin/env python3
"""Tests for tools/gen_cjk_font.py's character set and for the committed
font matching it. Offline: nothing is downloaded and lv_font_conv is not
run.

Run directly: python3 tools/gen_cjk_font_test.py
"""

import pathlib
import re
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import gen_cjk_font  # noqa: E402


class Charset(unittest.TestCase):
    def test_source_sets(self):
        self.assertEqual(len(gen_cjk_font.gb2312_chars()), 7445)
        self.assertEqual(len(gen_cjk_font.jis_x0208_chars(range(16, 48))), 2965)
        symbols = gen_cjk_font.jis_x0208_chars(range(1, 9))
        for c in "あンー・〜＼":
            self.assertIn(c, symbols)

    def test_charset_is_sorted_unique_and_covers_every_set(self):
        chars, _ = gen_cjk_font.build_charset()
        self.assertEqual(chars, sorted(set(chars), key=ord))
        have = set(chars)
        for c in "A~啊齄あヶ亜腕検読電〿～":
            self.assertIn(c, have)
        self.assertTrue(gen_cjk_font.ascii_chars() <= have)
        self.assertFalse(gen_cjk_font.ABSENT_FROM_SOURCE & have)

    def test_string_table_characters_are_folded_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            table = pathlib.Path(tmp) / "strings_xx.cpp"
            table.write_text(
                '// "✓ in a comment is ignored"\n'
                '/* "✗ so is this" */\n'
                'case StrId::A:\n    return "한글 \\"q\\" é http://ø";  // "ignored ✔"\n',
                encoding="utf-8",
            )
            chars, extras = gen_cjk_font.build_charset([table])
        # é is already in GB2312 (its pinyin row); ø (after a "//" inside the
        # literal) and the Hangul are new.
        self.assertEqual("".join(extras), "ø글한")
        self.assertIn("é", chars)
        for c in "✓✗✔":
            self.assertNotIn(c, chars)


class CommittedFont(unittest.TestCase):
    """The checked-in font must be what the script would produce now: same
    character set, same source and tool versions in its header."""

    def setUp(self):
        self.text = gen_cjk_font.OUTPUT.read_text(encoding="utf-8")

    def test_glyphs_match_the_charset(self):
        produced = {int(cp, 16) for cp in re.findall(r"/\* U\+([0-9A-F]+) ", self.text)}
        chars, _ = gen_cjk_font.build_charset()
        self.assertEqual(produced, {ord(c) for c in chars})

    def test_header_names_the_pinned_inputs(self):
        head = self.text[:4000]
        self.assertIn(gen_cjk_font.FONT_SHA256, head)
        self.assertIn(gen_cjk_font.FONT_URL, head)
        self.assertIn(gen_cjk_font.LV_FONT_CONV, head)
        self.assertIn("SIL Open Font License", head)
        self.assertIn("const lv_font_t lv_font_cjk_16 = {", self.text)


if __name__ == "__main__":
    unittest.main()
