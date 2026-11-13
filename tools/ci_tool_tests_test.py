#!/usr/bin/env python3
"""Check that selective CI preserves coverage when inputs or its rules change."""

import unittest

from ci_tool_tests import TESTS, changed_paths, select_tests


class SelectionTest(unittest.TestCase):
    def test_missing_baseline_and_unknown_tool_run_everything(self):
        for paths in (None, ["tools/new_generator.py"], ["tools/gen_cjk_font_new.py"],
                      ["cmake/dependencies.cmake"],
                      ["tools/ci.sh"], [".github/workflows/ci.yml"]):
            with self.subTest(paths=paths):
                self.assertEqual(select_tests(paths), list(TESTS))

    def test_absent_baseline_does_not_guess_a_parent(self):
        self.assertIsNone(changed_paths(None))

    def test_tool_and_its_test_select_the_same_suite(self):
        for name in TESTS:
            with self.subTest(name=name):
                self.assertIn(name, select_tests([f"tools/{name}.py"]))
                self.assertIn(name, select_tests([f"tools/{name.removesuffix('_test')}.py"]))

    def test_font_inputs_select_consumers_but_docs_skip_tool_self_tests(self):
        selected = select_tests(["components/wisp_ui/src/strings_zh.cpp"])
        self.assertIn("gen_cjk_font_test", selected)
        self.assertIn("gen_vector_font_test", selected)
        self.assertIn("gen_agent_docs_test", select_tests(["docs/scene-api.md"]))
        self.assertEqual(select_tests(["README.md"]), [])
        self.assertEqual(select_tests(["components/wisp_ui/src/system_layer.cpp"]), [])


if __name__ == "__main__":
    unittest.main()
