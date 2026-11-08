#!/usr/bin/env python3
"""Tests for tools/gen_icon_font.py's Wisp icon table and the header it
writes. Offline: lv_font_conv is not run and no LVGL source is needed.

Run directly: python3 tools/gen_icon_font_test.py
"""

import pathlib
import re
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import gen_icon_font  # noqa: E402


class Icons(unittest.TestCase):
    def test_table_is_valid(self):
        self.assertEqual(gen_icon_font.validate_icons(gen_icon_font.WISP_ICONS), [])
        self.assertIn(("microphone", 0xF130), gen_icon_font.WISP_ICONS)

    def test_validate_finds_problems(self):
        problems = gen_icon_font.validate_icons(
            [("a", 0xF130), ("a", 0xF131), ("b", 0xF131), ("Bad", 0xF132), ("c", 0x41)]
        )
        self.assertEqual(len(problems), 4, problems)

    def test_subset_48_names_are_unique_and_known(self):
        subset = gen_icon_font.SUBSET_48
        self.assertEqual(len(subset), len(set(subset)))
        self.assertLessEqual(len(subset), 15)
        wisp = {name for name, _ in gen_icon_font.WISP_ICONS}
        for name in subset:
            self.assertTrue(name.startswith("LV_SYMBOL_") or name in wisp, name)
        self.assertIn("microphone", subset)

    def test_scene_doc_lists_subset_48(self):
        # docs/scene-api.md (from schema.cpp's schema_markdown()) names the
        # icons that have 48 pixels; scene names are the LV_SYMBOL_ ones
        # in lower case.
        doc = (gen_icon_font.ROOT / "docs/scene-api.md").read_text(encoding="utf-8")
        start = doc.index("Only these have 48 pixels")
        listed = re.findall(r"`(\w+)`", doc[start : doc.index(".", start + 30)])
        want = [n.removeprefix("LV_SYMBOL_").lower() for n in gen_icon_font.SUBSET_48]
        self.assertEqual(listed, want)

    def test_utf8_escape(self):
        self.assertEqual(gen_icon_font.utf8_escape(0xF130), "\\xEF\\x84\\xB0")

    def test_header_lists_every_icon(self):
        text = gen_icon_font.header_text()
        for name, cp in gen_icon_font.WISP_ICONS:
            macro = gen_icon_font.macro(name)
            self.assertIn(f'#define {macro}', text)
            self.assertIn(f'{{"{name}", {macro}}},', text)
            self.assertIn(f"/* U+{cp:04X} */", text)

    def test_committed_header_is_current(self):
        self.assertEqual(
            gen_icon_font.HEADER.read_text(encoding="utf-8"), gen_icon_font.header_text()
        )


if __name__ == "__main__":
    unittest.main()
