#!/usr/bin/env python3
"""Self-test for tools/check_settings.py: builds small scratch trees under a
tempdir, each with a fixture settings_schema.hpp in the real row format, and
asserts the checker catches restated setting defaults and ranges (including
the patterns the settings schema replaced) and stays silent on numbers that
are not next to a setting, on full scale, on comparisons and on marked lines.

Run directly: python3 tools/check_settings_test.py
"""

import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_settings  # noqa: E402

SCHEMA_HPP = "components/wisp_core/include/wisp/core/settings_schema.hpp"

HEADER = """#pragma once
// tools/check_settings.py parses the list one `X(` row per line.
enum class LangCode : uint8_t { Zh, Ja, En };
inline constexpr std::string_view kLangCodeValues[] = {"zh", "ja", "en"};  // index = LangCode
inline constexpr int32_t kScreenOffChoices[] = {0, 15, 30, 60, 120, 300, 600};
inline constexpr int32_t kUtcOffsetChoices[] = {
    -720, -660, 0,
    480,  840};
inline constexpr std::span<const int32_t> kNoIntChoices{};
inline constexpr std::span<const std::string_view> kNoEnumValues{};
// clang-format off
#define WISP_SETTINGS(X) \\
X(Brightness,    brightness,      int,              Int,      "display", "brightness",   10,   255,  200, "",                            kNoIntChoices,     kNoEnumValues,   Level,            kSettingAgent | kSettingWeb,                     "brightness",    "") \\
X(Volume,        volume,          int,              Int,      "audio",   "volume",       0,    100,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          kSettingAgent | kSettingWeb,                     "volume",        "") \\
X(ScreenOffS,    screen_off_s,    int,              Int,      "power",   "screen_off_s", 15,   3600, 60,  "",                            kScreenOffChoices, kNoEnumValues,   Seconds,          kSettingAgent | kSettingZeroNever,               "screen_off",    "") \\
X(Language,      language,        LangCode,         Enum,     "sys",     "lang",         0,    0,    0,   "",                            kNoIntChoices,     kLangCodeValues, None,             kSettingAgent | kSettingScene,                   "lang_label",    "lang") \\
X(UtcOffsetMin,  utc_offset_min,  int,              Int,      "sys",     "tz_min",       -720, 840,  480, "",                            kUtcOffsetChoices, kNoEnumValues,   UtcOffsetMinutes, kSettingAgent | kSettingScene,                   "tz_label",      "utc_offset") \\
X(NtpServers,    ntp_servers,     FixedString<127>, HostList, "sys",     "ntp",          2,    63,   0,   "pool.ntp.org,ntp.aliyun.com", kNoIntChoices,     kNoEnumValues,   None,             kSettingWeb,                                     "time_servers",  "") \\
X(NotifyVol,     notify_vol,      int,              Int,      "audio",   "notify_vol",   0,    100,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          kSettingWeb,                                     "notify_vol",    "") \\
X(Time24h,       time_24h,        bool,             Bool,     "sys",     "time_24h",     0,    1,    1,   "",                            kNoIntChoices,     kNoEnumValues,   None,             kSettingWeb,                                     "time_24h",      "time_24h") \\
X(AgentTurnMin,  agent_turn_min,  int,              Int,      "agent",   "turn_min",     2,    60,   15,  "",                            kNoIntChoices,     kNoEnumValues,   Minutes,          kSettingWeb,                                     "agent_minutes", "")
// clang-format on
"""

EXTRA_ROW = (
    "X(LowBattPct,    low_batt_pct,    int,              Int,      \"power\",   \"low_batt_pct\", "
    "5,    50,   15,  \"\",                            kNoIntChoices,     kNoEnumValues,   "
    "Percent,          0,                                               \"\",              \"\")"
)


def write(root, rel_path, text):
    path = root / rel_path
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


class CheckSettingsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        write(self.root, SCHEMA_HPP, HEADER)

    def tearDown(self):
        self._tmp.cleanup()

    def code(self, text, rel="components/x/src/a.cpp"):
        """Check the tree with `text` at `rel`; assert_one_finding removes
        the file again, so one test can try several snippets."""
        write(self.root, rel, text)
        return check_settings.check(self.root)

    def assert_one_finding(self, text, rel="components/x/src/a.cpp", line=1):
        errors = self.code(text, rel)
        (self.root / rel).unlink()
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith(f"{rel}:{line}: "), errors[0])
        return errors[0]

    def test_rows_are_parsed_with_quoted_commas(self):
        rows = check_settings.load_rows(self.root)
        self.assertEqual([r.name for r in rows][:3], ["brightness", "volume", "screen_off_s"])
        ntp = next(r for r in rows if r.name == "ntp_servers")
        self.assertEqual(ntp.def_text, "pool.ntp.org,ntp.aliyun.com")
        tz = next(r for r in rows if r.name == "utc_offset_min")
        self.assertEqual((tz.min, tz.max, tz.def_int), (-720, 840, 480))
        self.assertEqual(tz.int_choices, [-720, -660, 0, 480, 840])
        lang = next(r for r in rows if r.name == "language")
        self.assertEqual(lang.enum_values, ["zh", "ja", "en"])

    def test_clean_tree_passes(self):
        self.assertEqual(self.code("int f() { return 3; }\n"), [])

    def schema_error(self, header):
        write(self.root, SCHEMA_HPP, header)
        errors = check_settings.check(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertTrue(errors[0].startswith(SCHEMA_HPP + ":"), errors[0])
        return errors[0]

    def test_row_split_over_two_lines_is_an_error(self):
        split = HEADER.replace('"audio",   "volume",       0,', '"audio",   \\\n    "volume", 0,')
        self.assertNotEqual(split, HEADER)
        self.schema_error(split)
        with self.assertRaises(check_settings.SchemaError):
            check_settings.load_rows(self.root)

    def test_row_with_spaced_x_is_parsed_not_dropped(self):
        write(self.root, SCHEMA_HPP, HEADER.replace("X(Volume,", " X (Volume,"))
        self.assertIn("volume", [r.name for r in check_settings.load_rows(self.root)])
        self.assert_one_finding("uint8_t volume = 60;\n")

    def test_two_rows_on_one_line_are_an_error(self):
        joined = HEADER.replace('"volume",        "") \\\nX(', '"volume",        "") X(')
        self.assertNotEqual(joined, HEADER)
        self.schema_error(joined)

    def test_other_text_in_the_macro_body_is_an_error(self):
        self.schema_error(HEADER.replace("X(Volume,", "Y(Volume,"))

    # Rule 1: raw store access.

    def test_raw_store_read_fails(self):
        message = self.assert_one_finding('int b = settings.get_int("brightness", 200);\n')
        self.assertIn("brightness", message)

    def test_raw_store_read_without_a_default_fails(self):
        self.assert_one_finding('auto v = s.get_bool("time_24h");\n')

    def test_settings_namespace_and_backend_access_fail(self):
        self.assert_one_finding('core::Settings audio(backend, "audio");\n')
        self.assert_one_finding('auto r = backend_.get("sys", "lang");\n', rel="components/x/b.cpp")

    def test_raw_store_write_by_key_fails(self):
        self.assert_one_finding('s.set("brightness", Value(200));\n')
        self.assert_one_finding('settings_->set("tz_min", Value(int64_t{60}));\n')
        # A one-argument set() of the same text is not a store write.
        self.assertEqual(self.code('flags.set("brightness");\n'), [])

    def test_namespace_word_in_other_calls_passes(self):
        self.assertEqual(self.code('auto a = obj.get("audio");\nlog("display");\n'), [])

    # Rule 2: an Int row's default or range next to its token.

    def test_default_constant_fails(self):
        self.assert_one_finding("constexpr int kDefaultUtcOffsetMin = 480;\n")

    def test_range_and_negative_minimum_fail(self):
        self.assert_one_finding("if (utc_offset < -720) return;\n")
        self.assert_one_finding("constexpr int kMaxBrightness = 255u;\n", rel="components/x/b.cpp")

    def test_number_without_a_token_passes(self):
        self.assertEqual(self.code("int x = 480;\n"), [])

    def test_number_of_another_row_passes(self):
        # 480 is utc_offset_min's default, not brightness's.
        self.assertEqual(self.code("int brightness_scale = 480;\n"), [])

    def test_single_word_suffix_strip_is_not_a_token(self):
        # agent_turn_min's stripped name is agent_turn; plain "turn" is not
        # one of its tokens.
        self.assertEqual(self.code('auto s = std::string("turn") + std::to_string(2);\n'), [])
        self.assertEqual(self.code("int turn = 15;\n"), [])
        self.assert_one_finding("int agent_turn = 15;\n", rel="components/x/b.cpp")

    def test_token_inside_a_longer_identifier_and_screaming_case(self):
        self.assert_one_finding("#define SCREEN_OFF_DEFAULT 60\n")
        self.assert_one_finding("int screenOffSeconds = 15;\n", rel="components/x/b.cpp")

    def test_zero_one_hex_and_floats_pass(self):
        self.assertEqual(self.code("brightness = 0; brightness = 1; brightness = 0xC8;\n"
                                   "float brightness_f = 200.5;\n"), [])

    def test_percent_full_scale_passes(self):
        self.assertEqual(self.code("volume_ = std::min<uint8_t>(volume, 100);\n"), [])

    def test_percent_default_fails(self):
        self.assert_one_finding("constexpr int kDefaultVolume = 60;\n")

    def test_comments_are_scanned(self):
        self.assert_one_finding("// Brightness starts at 200 out of 255.\n")

    def test_comment_counts_as_next_to_the_line_it_documents(self):
        text = ("struct Deps {\n"
                "    // Minutes east of UTC, a cached value (never Settings, which is\n"
                "    // flash). Empty: 480.\n"
                "    std::function<int()> utc_offset_min;\n"
                "};\n")
        self.assert_one_finding(text, line=3)

    def test_star_line_outside_a_block_comment_is_code(self):
        text = ("*out = 480;\n"
                "int utc_offset_min = offset();\n")
        self.assertEqual(self.code(text), [])
        text = ("/* Minutes east of UTC.\n"
                " * Empty: 480.\n"
                " */\n"
                "int utc_offset_min = offset();\n")
        self.assert_one_finding(text, rel="components/x/b.cpp", line=2)

    def test_comment_does_not_borrow_tokens_from_its_own_block(self):
        text = ("// The agent's turn limits (agent_turn_min) apply from the next turn.\n"
                "// See spec 2 for the boot order.\n"
                "#include <cstdint>\n")
        self.assertEqual(self.code(text), [])

    def test_choice_list_next_to_its_key_fails(self):
        self.assert_one_finding("const SCREEN_OFF = [0, 15, 30, 60, 120, 300, 600];\n",
                                rel="web/app/pages/system.js")

    def test_bool_and_hostlist_ranges_are_not_matched(self):
        # time_24h's 1 and ntp's 2/63 are not values.
        self.assertEqual(self.code("int ntp_tries = 2; int ntp_len = 63; bool time_24h = 1;\n"),
                         [])

    # Rule 3: default texts.

    def test_hostlist_default_anywhere_fails(self):
        self.assert_one_finding('const char* servers = "pool.ntp.org";\n')
        self.assert_one_finding("// tries ntp.aliyun.com first\n", rel="components/x/b.cpp")
        self.assert_one_finding("x = 'pool.ntp.org,ntp.aliyun.com';\n", rel="web/app/c.js")

    def test_enum_default_assignment_fails_comparison_passes(self):
        self.assert_one_finding('std::string lang = "zh";\n')
        self.assert_one_finding('const char* language{"zh"};\n', rel="components/x/b.cpp")
        self.assertEqual(self.code('if (code == "zh") return Lang::Zh;\nreturn "zh";\n',
                                   rel="components/x/c.cpp"), [])
        self.assertEqual(self.code('std::string lang = "en";\nauto zh = "zh";\n',
                                   rel="components/x/d.cpp"), [])

    def test_js_fallback_fails(self):
        self.assert_one_finding("const v = settings.notify_vol ?? 60;\n", rel="web/app/pages/s.js")
        self.assert_one_finding("const l = state.lang ?? 'zh';\n", rel="web/app/pages/t.js")

    # Markers.

    def test_marker_with_reason_passes(self):
        text = "int glow_brightness = 200;  // settings-literal-ok: the glow's own level\n"
        self.assertEqual(self.code(text), [])
        self.assertEqual(check_settings.count_markers(self.root), 1)

    def test_marker_without_reason_fails(self):
        message = self.assert_one_finding("int brightness = 200;  // settings-literal-ok:\n")
        self.assertIn("reason", message)
        message = self.assert_one_finding("int x = 3;  // settings-literal-ok:   \n",
                                          rel="components/x/b.cpp")
        self.assertIn("reason", message)

    def test_marker_in_javascript(self):
        text = "const brightness = 200;  // settings-literal-ok: a CSS filter level\n"
        self.assertEqual(self.code(text, rel="web/app/a.js"), [])

    # Scope.

    def test_tests_dir_and_schema_header_not_scanned(self):
        text = 'int b = s.get_int("brightness", 200);\n'
        self.assertEqual(self.code(text, rel="tests/core/a_test.cpp"), [])
        self.assertEqual(self.code(text, rel="components/x/tests/a_test.cpp"), [])
        self.assertEqual(self.code(text, rel="components/x/third_party/a.c"), [])
        self.assertEqual(self.code(text, rel="components/x/src/a.py"), [])
        self.assertEqual(self.code(text, rel="web/app/a.css"), [])
        self.assertEqual(check_settings.check(self.root), [])
        for top in ("simulator/src/a.cpp", "boards/b/board.cpp", "drivers/d/d.c",
                    "firmware/main/main.cpp", "web/app/x.js"):
            errors = self.code(text, rel=top)
            self.assertTrue(any(e.startswith(top + ":") for e in errors), (top, errors))
            (self.root / top).unlink()

    def test_new_row_is_picked_up(self):
        text = "constexpr int kLowBattThreshold = 15;\n"
        self.assertEqual(self.code(text), [])
        write(self.root, SCHEMA_HPP, HEADER.replace(
            '"agent_minutes", "")\n',
            '"agent_minutes", "") \\\n' + EXTRA_ROW + "\n"))
        errors = check_settings.check(self.root)
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("low_batt_pct", errors[0])

    def test_unparsable_schema_is_reported_not_raised(self):
        write(self.root, SCHEMA_HPP, HEADER.replace('"display", "brightness",   10,', '"display",'))
        errors = check_settings.check(self.root)
        self.assertTrue(errors and errors[0].startswith(SCHEMA_HPP + ":"), errors)
        (self.root / SCHEMA_HPP).unlink()
        errors = check_settings.check(self.root)
        self.assertTrue(errors and errors[0].startswith(SCHEMA_HPP + ":"), errors)

    def test_main_prints_marker_count_and_exits_1_on_failure(self):
        write(self.root, "components/x/a.cpp",
              "int glow_brightness = 200;  // settings-literal-ok: own level\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_settings.main(["--root", str(self.root)]), 0)
        self.assertEqual(out.getvalue(), "settings-literal-ok markers: 1\n")
        write(self.root, "components/x/b.cpp", "int kDefaultBrightness = 200;\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(check_settings.main(["--root", str(self.root)]), 1)
        self.assertIn("components/x/b.cpp:1:", out.getvalue())

    def test_repository_is_clean(self):
        repo = pathlib.Path(__file__).resolve().parent.parent
        self.assertEqual(check_settings.check(repo), [])
        self.assertGreater(len(check_settings.load_rows(repo)), 10)


if __name__ == "__main__":
    unittest.main()
