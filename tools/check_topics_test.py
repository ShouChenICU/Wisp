#!/usr/bin/env python3
"""Self-test for tools/check_topics.py: builds small scratch trees under a
tempdir and asserts the checker catches registry/doc drift and topic string
literals in code, and stays silent on comments, raw strings, prefixes and
patterns. The scanner cases matter most: a naive comment stripper would hide
code after a URL string or a digit separator.

Run directly: python3 tools/check_topics_test.py
"""

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_topics  # noqa: E402

TOPICS_HPP = "components/wisp_core/include/wisp/core/topics.hpp"

HEADER = """#pragma once
inline constexpr char kButtonPrefix[] = "button.";
inline constexpr char kPowerState[] = "power.state";
inline constexpr char kNetWifiState[] = "net.wifi.state", kNetApState[] = "net.ap.state";
inline constexpr char kStorageSdInserted[] = "storage.sd.inserted";
inline constexpr char kStorageSdRemoved[] = "storage.sd.removed";

inline constexpr Topic kAll[] = {
    {"button.<name>.down", Kind::InputEdge, false},
    {"button.<name>.up", Kind::InputEdge, false},
    {kPowerState, Kind::CellMirror, true},
    {kNetWifiState, Kind::CellMirror, true},
    {kNetApState, Kind::CellMirror, true},
    {kStorageSdInserted, Kind::CellMirror, true},
    {kStorageSdRemoved, Kind::CellMirror, true},
};
"""

DOC = """# Architecture

## Event topics

Intro text.

| Topic | Publisher | Notes |
|---|---|---|
| `button.<name>.down` / `.up` | `core::ButtonService` | edges |
| `power.state` | `app::PowerManager` | levels |
| `net.wifi.state` | `net::WifiService` | state |
| `net.ap.state` | `net::WifiService` | hotspot |
| `storage.sd.inserted` / `storage.sd.removed` | `core::StorageManager` | sd |

Trailing paragraph with `not.a.topic` in it.
"""


def write(root, rel_path, text):
    path = root / rel_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


class CheckTopicsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        write(self.root, TOPICS_HPP, HEADER)
        write(self.root, "docs/architecture.md", DOC)

    def tearDown(self):
        self._tmp.cleanup()

    def code(self, text, rel="components/x/src/a.cpp"):
        write(self.root, rel, text)
        return check_topics.check(self.root)

    def test_matching_registry_and_table_pass(self):
        self.assertEqual(check_topics.check(self.root), [])

    def test_registered_topic_without_a_row_fails(self):
        write(self.root, "docs/architecture.md",
              DOC.replace("| `net.ap.state` | `net::WifiService` | hotspot |\n", ""))
        errors = check_topics.check(self.root)
        self.assertEqual(
            errors, ['docs/architecture.md: topic "net.ap.state" is registered but has no row'])

    def test_row_without_a_registered_topic_fails(self):
        write(self.root, "docs/architecture.md",
              DOC.replace("| `power.state`", "| `power.extra` | x | y |\n| `power.state`"))
        errors = check_topics.check(self.root)
        self.assertEqual(len(errors), 1)
        self.assertIn('docs/architecture.md:10: topic "power.extra" has a row but is not '
                      'registered', errors)

    def test_suffix_pieces_expand(self):
        # `.up` replaces the last segment; a full name after " / " stands alone.
        self.assertEqual(check_topics.check(self.root), [])
        write(self.root, "docs/architecture.md", DOC.replace("`.up`", "`.long`"))
        errors = check_topics.check(self.root)
        self.assertEqual(len(errors), 2)
        self.assertTrue(any('"button.<name>.up" is registered' in e for e in errors))
        self.assertTrue(any('"button.<name>.long" has a row' in e for e in errors))

    def test_literal_topic_in_a_publish_call_fails(self):
        errors = self.code('void f() {\n    bus.publish({"power.state", 1});\n}\n')
        self.assertEqual(len(errors), 1)
        self.assertIn("components/x/src/a.cpp:2:", errors[0])
        self.assertIn('"power.state"', errors[0])
        self.assertIn("kPowerState", errors[0])

    def test_name_for_constant_follows_kname_rule(self):
        errors = self.code('auto a = "storage.sd.inserted";\n')
        self.assertIn("kStorageSdInserted", errors[0])

    def test_literal_in_a_comment_or_raw_string_is_ignored(self):
        text = (
            '// publish("power.state")\n'
            '/* publish("power.state")\n   "power.state" */\n'
            'const char* a = R"(power.state)";\n'
            'const char* b = R"x(\n"power.state"\n)x";\n'
            'const char* c = u8R"(\n"power.state"\n)";\n'
        )
        self.assertEqual(self.code(text), [])

    def test_prefix_literals_are_allowed(self):
        self.assertEqual(self.code('bus.subscribe("net.", cb);\nauto s = "net.wifi.state.x";\n'), [])

    def test_patterns_and_the_registry_header_are_not_flagged(self):
        self.assertEqual(self.code('auto s = "button.<name>.down";\n'), [])
        # topics.hpp itself spells every name.
        self.assertEqual(check_topics.check(self.root), [])

    def test_tests_directory_is_not_scanned_but_simulator_is(self):
        self.assertEqual(self.code('auto s = "power.state";\n', "tests/core/a_test.cpp"), [])
        errors = self.code('auto s = "power.state";\n', "simulator/src/main.cpp")
        self.assertEqual(len(errors), 1)

    def test_literal_after_a_slash_slash_string_on_the_same_line_is_caught(self):
        errors = self.code('f("http://x", "power.state");\n')
        self.assertEqual(len(errors), 1)

    def test_literal_inside_a_comment_after_a_string_is_ignored(self):
        self.assertEqual(self.code('f("http://x"); // "power.state"\n'), [])

    def test_literal_after_a_string_containing_block_comment_start_is_caught(self):
        errors = self.code('const char* a = "/*";\nbus.publish({"power.state", 1});\n')
        self.assertEqual(len(errors), 1)
        self.assertIn(":2:", errors[0])

    def test_literal_after_a_digit_separator_is_caught(self):
        errors = self.code("int n = 20'000;\nbus.publish({\"power.state\", 1});\n")
        self.assertEqual(len(errors), 1)
        self.assertIn(":2:", errors[0])

    def test_char_literals_do_not_open_strings(self):
        errors = self.code("char q = '\"';\nchar e = '\\'';\nauto s = \"power.state\";\n")
        self.assertEqual(len(errors), 1)
        self.assertIn(":3:", errors[0])

    def test_escapes_inside_strings_are_honoured(self):
        self.assertEqual(self.code('auto s = "a\\"power.state\\"b";\n'), [])
        self.assertEqual(len(self.code('auto s = "a\\\\"; auto t = "power.state";\n')), 1)

    def hpp(self, text):
        write(self.root, TOPICS_HPP, text)
        return check_topics.check(self.root)

    def test_undefined_constant_in_kall_is_reported(self):
        errors = self.hpp(HEADER.replace("{kPowerState,", "{kUndefined, Kind::CellMirror, true},\n    {kPowerState,"))
        self.assertIn(f'{TOPICS_HPP}:11: kAll names undefined constant "kUndefined"', errors)

    def test_commented_out_entry_is_not_registered(self):
        errors = self.hpp(HEADER.replace("    {kPowerState,", "    // {kOld, Kind::CellMirror, true},\n    /* {kOld2, Kind::X, true}, */\n    {kPowerState,"))
        self.assertEqual(errors, [])

    def test_comment_with_closing_brace_does_not_truncate_kall(self):
        text = HEADER.replace("    {kPowerState,", "    // };\n    {kPowerState,")
        text = "// kAll[] is the list\n" + text
        self.assertEqual(self.hpp(text), [])

    def test_malformed_topic_cell_is_reported_and_documents_nothing(self):
        write(self.root, "docs/architecture.md", DOC.replace("`power.state`", "power.state"))
        errors = check_topics.check(self.root)
        self.assertIn('docs/architecture.md:10: malformed topic cell "power.state"', errors)
        self.assertIn('docs/architecture.md: topic "power.state" is registered but has no row',
                      errors)

    def test_backslash_newline_in_a_string_keeps_line_numbers(self):
        errors = self.code('const char* a = "x\\\ny";\nauto s = "power.state";\n')
        self.assertEqual(len(errors), 1)
        self.assertIn(":3:", errors[0])

    def outside(self, doc, row):
        write(self.root, "docs/architecture.md", doc)
        line = doc.splitlines().index(row) + 1
        return f"docs/architecture.md:{line}: topic row outside the topic table"

    def test_topic_row_under_a_sub_heading_is_reported(self):
        row = "| `bogus.one` / `.two` | x | y |"
        doc = DOC + "\n### Sub\n\n| Topic | P | N |\n|---|---|---|\n" + row + "\n"
        expected = self.outside(doc, row)
        self.assertEqual(check_topics.check(self.root), [expected])

    def test_other_tables_under_a_sub_heading_are_accepted(self):
        doc = DOC + (
            "\n### State cells and edge inboxes\n\n"
            "| Path | Delivery | Lost when |\n|---|---|---|\n"
            "| Cell subscription | Coalesced | Never |\n"
            "| Edge inbox | Every edge, in push order | The dispatcher stalls for `N` edges |\n"
            "| `x` | not dotted | y |\n")
        write(self.root, "docs/architecture.md", doc)
        self.assertEqual(check_topics.check(self.root), [])

    def test_topic_row_after_a_blank_line_is_reported_once(self):
        row = "| `bogus.two` | x | y |"
        doc = DOC.replace("\nTrailing", "\n" + row + "\n\nTrailing")
        expected = self.outside(doc, row)
        self.assertEqual(check_topics.check(self.root), [expected])

    def test_scan_stops_at_the_next_section(self):
        doc = DOC + "\n## Other\n\n| T |\n|---|\n| `bogus.three` |\n"
        write(self.root, "docs/architecture.md", doc)
        self.assertEqual(check_topics.check(self.root), [])

    def test_constant_missing_from_kall_is_reported_but_prefixes_are_not(self):
        errors = self.hpp(HEADER.replace("inline constexpr Topic",
                          'inline constexpr char kLost[] = "lost.topic";\ninline constexpr Topic'))
        self.assertTrue(any("constant kLost is not in kAll" in e for e in errors))
        self.assertFalse(any("kButtonPrefix" in e for e in errors))

    def test_duplicate_names_in_kall_are_reported(self):
        errors = self.hpp(HEADER.replace("    {kPowerState,", "    {kPowerState, Kind::CellMirror, true},\n    {kPowerState,"))
        self.assertTrue(any('topic "power.state" is registered twice' in e for e in errors))

    def test_literal_for_a_constantless_entry_says_to_register_one(self):
        text = HEADER.replace("    {kPowerState,", '    {"raw.topic", Kind::Notification, true},\n    {kPowerState,')
        write(self.root, TOPICS_HPP, text)
        write(self.root, "docs/architecture.md", DOC.replace("| `power.state`", "| `raw.topic` | a | b |\n| `power.state`"))
        errors = self.code('auto s = "raw.topic";\n')
        self.assertEqual(len(errors), 1)
        self.assertIn("register a constant for it in topics.hpp", errors[0])
        self.assertNotIn("?", errors[0])

    def test_missing_files_are_reported_not_raised(self):
        (self.root / "docs/architecture.md").unlink()
        errors = check_topics.check(self.root)
        self.assertEqual(len(errors), 1)
        self.assertTrue(errors[0].startswith("docs/architecture.md:"))
        (self.root / TOPICS_HPP).unlink()
        errors = check_topics.check(self.root)
        self.assertTrue(any(e.startswith(TOPICS_HPP + ":") for e in errors))

    def test_main_is_silent_on_success_and_exits_1_on_failure(self):
        self.assertEqual(check_topics.main(["--root", str(self.root)]), 0)
        write(self.root, "components/x/src/a.cpp", 'auto s = "power.state";\n')
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_topics.main(["--root", str(self.root)]), 1)
        self.assertIn("components/x/src/a.cpp:1:", out.getvalue())

    def test_repository_is_clean(self):
        repo = pathlib.Path(__file__).resolve().parent.parent
        self.assertEqual(check_topics.check(repo), [])


if __name__ == "__main__":
    unittest.main()
