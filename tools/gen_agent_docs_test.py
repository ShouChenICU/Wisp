#!/usr/bin/env python3
"""Tests for tools/gen_agent_docs.py: runs the script as a subprocess, as
tools/ci.sh does, on a scratch examples tree, and checks the committed
output against the real one.

Run directly: python3 tools/gen_agent_docs_test.py
"""

import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
import zlib

TOOLS = pathlib.Path(__file__).resolve().parent
ROOT = TOOLS.parent
SCRIPT = TOOLS / "gen_agent_docs.py"
sys.path.insert(0, str(TOOLS))
import gen_agent_docs  # noqa: E402

def run(*args):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True
    )


class GenAgentDocsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)
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
        self.scene_api = self.dir / "scene-api.md"
        self.scene_api.write_text("# Scene API\n\n## wisp.store\n\nKeys. 键\n", encoding="utf-8")
        self.out = self.dir / "docs_data.cpp"

    def tearDown(self):
        self._tmp.cleanup()

    def args(self, *extra):
        return (*extra, "--examples", self.examples, "--scene-api", self.scene_api,
                "--output", self.out)

    def generate(self):
        r = run(*self.args())
        self.assertEqual(r.returncode, 0, r.stderr)
        return self.out.read_text(encoding="utf-8")

    def topics(self, text):
        """The example topics as a dict, inflated and cut by the table."""
        _, examples = gen_agent_docs.read_streams(text)
        data = examples.encode("utf-8")
        out = {}
        for topic, offset, size in re.findall(r'\{"([^"]+)", (\d+), (\d+)\}', text):
            out[topic] = data[int(offset):int(offset) + int(size)].decode("utf-8")
        return out

    def test_one_topic_per_example(self):
        topics = self.topics(self.generate())
        self.assertEqual(list(topics), ["examples", "example/clock", "example/counter"])
        self.assertIn("`clock`: 时钟", topics["examples"])
        self.assertIn("`counter`: 计数器", topics["examples"])
        counter = topics["example/counter"]
        self.assertIn("### scene.json", counter)
        self.assertIn("### main.lua", counter)
        self.assertIn("### lib/util.lua", counter)
        self.assertLess(counter.index("scene.json"), counter.index("lib/util.lua"))

    def test_scene_api_is_stored_whole(self):
        text = self.generate()
        scene_api, _ = gen_agent_docs.read_streams(text)
        self.assertEqual(scene_api, self.scene_api.read_text(encoding="utf-8"))
        size = len(scene_api.encode("utf-8"))
        self.assertIn(f"return {{kSceneApi, {size}}};", text)

    def test_check_mode_detects_stale_output(self):
        self.generate()
        ok = run(*self.args("--check"))
        self.assertEqual(ok.returncode, 0, ok.stderr)
        (self.examples / "clock" / "main.lua").write_text("return ui.screen {}\n", encoding="utf-8")
        stale = run(*self.args("--check"))
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn("gen_agent_docs.py", stale.stderr)
        self.generate()
        self.scene_api.write_text("# Scene API\n", encoding="utf-8")
        self.assertNotEqual(run(*self.args("--check")).returncode, 0)
        self.generate()
        # A size in the table that does not match the text.
        text = self.out.read_text(encoding="utf-8")
        self.out.write_text(text.replace('{"examples", 0, ', '{"examples", 0, 1'), encoding="utf-8")
        self.assertNotEqual(run(*self.args("--check")).returncode, 0)
        missing = self.dir / "none.cpp"
        absent = run("--check", "--examples", self.examples, "--scene-api", self.scene_api,
                     "--output", missing)
        self.assertNotEqual(absent.returncode, 0)
        self.assertFalse(missing.exists())

    def test_check_reads_the_text_not_the_compressed_bytes(self):
        # Another zlib (here: a stored block) gives other bytes for the same text; that
        # is not stale.
        text = self.generate()
        api = self.scene_api.read_text(encoding="utf-8").encode("utf-8")
        c = zlib.compressobj(0, zlib.DEFLATED, -15)
        other = c.compress(api) + c.flush()
        self.assertNotEqual(other, gen_agent_docs.read_array(text, "kSceneApi"))
        entries = gen_agent_docs.example_topics(self.examples)
        examples = gen_agent_docs.read_array(text, "kExamples")
        self.out.write_text(
            gen_agent_docs.render(api.decode("utf-8"), entries, other, examples), encoding="utf-8")
        ok = run(*self.args("--check"))
        self.assertEqual(ok.returncode, 0, ok.stderr)

    def test_output_is_deterministic(self):
        first = self.generate()
        second = self.generate()
        self.assertEqual(first, second)
        # Files touched in another order make no difference either.
        (self.examples / "counter" / "main.lua").write_text(
            "return ui.screen {}\n", encoding="utf-8"
        )
        self.assertEqual(first, self.generate())

    def test_committed_output_is_current(self):
        r = run("--check")
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
