#!/usr/bin/env python3
"""Tests for tools/gen_agent_docs.py: runs the script as a subprocess, as
tools/ci.sh does, on a scratch scene-api.md and examples tree, and checks
the committed output against the real ones.

Run directly: python3 tools/gen_agent_docs_test.py
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
SCRIPT = TOOLS / "gen_agent_docs.py"

DOC = """# Scene API

<!-- Generated; do not edit. -->

Intro text.

## Widgets

The widget table.

### list, canvas

Notes on lists.

## Screen shape

Use row_span.

## wisp.store

Store text.

## wisp.http

Get things.

## Hooks

Hook text.

## wisp.http.download and deep standby

Download text.
"""


def run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True
    )


class GenAgentDocsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)
        self.doc = self.dir / "scene-api.md"
        self.doc.write_text(DOC, encoding="utf-8")
        self.examples = self.dir / "examples"
        counter = self.examples / "counter"
        (counter / "lib").mkdir(parents=True)
        (counter / "scene.json").write_text(
            '{"id": "counter", "name": "计数器", "version": "1.0.0", "schema_version": 1}\n',
            encoding="utf-8",
        )
        (counter / "main.lua").write_text("return ui.screen {}\n", encoding="utf-8")
        (counter / "lib" / "util.lua").write_text("return {}\n", encoding="utf-8")
        clock = self.examples / "clock"
        clock.mkdir()
        (clock / "scene.json").write_text(
            '{"id": "clock", "name": "时钟", "version": "1.0.0", "schema_version": 1}\n',
            encoding="utf-8",
        )
        (clock / "main.lua").write_text("return ui.screen { ui.label {} }\n", encoding="utf-8")
        self.out = self.dir / "docs_data.cpp"

    def tearDown(self):
        self._tmp.cleanup()

    def generate(self):
        r = run("--doc", self.doc, "--examples", self.examples, "--output", self.out)
        self.assertEqual(r.returncode, 0, r.stderr)
        return self.out.read_text(encoding="utf-8")

    def topics(self, text):
        """The {topic, text} table as a dict, read back from the C++ file."""
        out = {}
        marker = '{"'
        pos = 0
        while True:
            start = text.find(marker, pos)
            if start < 0:
                return out
            end = text.find('"', start + 2)
            topic = text[start + 2 : end]
            body_start = text.find('R"wispdoc(', end) + len('R"wispdoc(')
            body_end = text.find(')wispdoc"', body_start)
            out[topic] = text[body_start:body_end]
            pos = body_end

    def test_splits_modules_by_heading(self):
        topics = self.topics(self.generate())
        self.assertEqual(
            list(topics),
            ["lua", "lua/http", "lua/store", "screen", "examples", "example/clock",
             "example/counter"],
        )
        self.assertIn("Intro text.", topics["lua"])
        self.assertIn("## Widgets", topics["lua"])
        self.assertIn("### list, canvas", topics["lua"])
        self.assertIn("## Hooks", topics["lua"])
        self.assertNotIn("Generated", topics["lua"])
        self.assertNotIn("Store text", topics["lua"])
        self.assertIn("Store text.", topics["lua/store"])
        # Both wisp.http sections, in document order.
        http = topics["lua/http"]
        self.assertLess(http.index("Get things."), http.index("Download text."))
        self.assertIn("row_span", topics["screen"])
        self.assertNotIn("row_span", topics["lua"])
        self.assertIn("`clock`: 时钟", topics["examples"])
        self.assertIn("`counter`: 计数器", topics["examples"])
        counter = topics["example/counter"]
        self.assertIn("### scene.json", counter)
        self.assertIn("### main.lua", counter)
        self.assertIn("### lib/util.lua", counter)
        self.assertLess(counter.index("scene.json"), counter.index("lib/util.lua"))

    def test_check_mode_detects_stale_output(self):
        self.generate()
        ok = run("--check", "--doc", self.doc, "--examples", self.examples, "--output", self.out)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.doc.write_text(DOC + "\n## wisp.timer\n\nTimers.\n", encoding="utf-8")
        stale = run("--check", "--doc", self.doc, "--examples", self.examples, "--output", self.out)
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("gen_agent_docs.py", stale.stderr)
        missing = self.dir / "none.cpp"
        absent = run("--check", "--doc", self.doc, "--examples", self.examples, "--output", missing)
        self.assertNotEqual(absent.returncode, 0)
        self.assertFalse(missing.exists())

    def test_output_is_deterministic(self):
        first = self.generate()
        second = self.generate()
        self.assertEqual(first, second)
        # Files touched in another order make no difference either.
        (self.examples / "counter" / "main.lua").write_text(
            "return ui.screen {}\n", encoding="utf-8"
        )
        self.assertEqual(first, self.generate())

    def test_refuses_text_that_would_end_the_raw_string(self):
        self.doc.write_text(DOC + '\n## wisp.log\n\nx )wispdoc" y\n', encoding="utf-8")
        r = run("--doc", self.doc, "--examples", self.examples, "--output", self.out)
        self.assertNotEqual(r.returncode, 0)

    def test_committed_output_is_current(self):
        r = run("--check")
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
