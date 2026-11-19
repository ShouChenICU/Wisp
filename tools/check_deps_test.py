#!/usr/bin/env python3
"""Tests for tools/check_deps.py."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_deps  # noqa: E402

MANIFEST = """\
# comment
dependencies:
  idf: ">=6.0.3,<6.1"
  lvgl/lvgl: "9.5.0"
  # an inline comment line
  espressif/usb: "1.6.0"
  espressif/mapped:
    version: "2.0.0"
    rules:
      - if: "target in [esp32s3]"
"""

LOCK = """\
dependencies:
  espressif/mapped:
    component_hash: abc
    dependencies: []
    source:
      registry_url: https://components.espressif.com/
      type: service
    version: 2.0.0
  espressif/usb:
    component_hash: def
    dependencies:
    - name: idf
      require: private
      version: '>=5.5.3'
    source:
      registry_url: https://components.espressif.com/
      type: service
    version: 1.6.0
  idf:
    source:
      type: idf
    version: 6.0.3
  lvgl/lvgl:
    component_hash: 123
    dependencies: []
    source:
      registry_url: https://components.espressif.com/
      type: service
    version: 9.5.0
direct_dependencies:
- espressif/mapped
- espressif/usb
- idf
- lvgl/lvgl
manifest_hash: 0
target: esp32s3
version: 3.0.0
"""

TRANSITIVE = """\
  espressif/cmake_utilities:
    component_hash: 999
    dependencies: []
    source:
      registry_url: https://components.espressif.com/
      type: service
    version: 0.5.3
"""


class ParseTest(unittest.TestCase):
    def test_manifest(self):
        self.assertEqual(
            check_deps.parse_manifest(MANIFEST),
            {"idf": ">=6.0.3,<6.1", "lvgl/lvgl": "9.5.0", "espressif/usb": "1.6.0",
             "espressif/mapped": "2.0.0"})

    def test_lock(self):
        self.assertEqual(
            check_deps.parse_lock(LOCK),
            {"espressif/mapped": ("service", "2.0.0"), "espressif/usb": ("service", "1.6.0"),
             "idf": ("idf", "6.0.3"), "lvgl/lvgl": ("service", "9.5.0")})


class CheckTest(unittest.TestCase):
    def check(self, manifest, lock):
        return check_deps.check(check_deps.parse_manifest(manifest), check_deps.parse_lock(lock))

    def test_all_pinned_is_clean(self):
        self.assertEqual(self.check(MANIFEST, LOCK), [])

    def test_transitive_range_dependency_fails(self):
        lock = LOCK.replace("direct_dependencies:", TRANSITIVE.rstrip("\n") + "\ndirect_dependencies:")
        errors = self.check(MANIFEST, lock)
        self.assertEqual(len(errors), 1)
        self.assertIn("espressif/cmake_utilities", errors[0])
        self.assertIn('"0.5.3"', errors[0])

    def test_range_in_manifest_fails(self):
        errors = self.check(MANIFEST.replace('lvgl/lvgl: "9.5.0"', 'lvgl/lvgl: "~9.5.0"'), LOCK)
        self.assertEqual(len(errors), 1)
        self.assertIn("lvgl/lvgl", errors[0])

    def test_pin_disagreeing_with_lock_fails(self):
        errors = self.check(MANIFEST.replace('espressif/usb: "1.6.0"', 'espressif/usb: "1.7.0"'), LOCK)
        self.assertEqual(len(errors), 1)
        self.assertIn("espressif/usb", errors[0])
        self.assertIn("1.6.0", errors[0])

    def test_exact_spellings_are_exact(self):
        manifest = MANIFEST.replace('"9.5.0"', '"==9.5.0"')
        self.assertEqual(self.check(manifest, LOCK), [])
        manifest = MANIFEST.replace('espressif/usb: "1.6.0"', 'espressif/usb: "1.6.0~1"')
        lock = LOCK.replace("version: 1.6.0", "version: 1.6.0~1")
        self.assertEqual(self.check(manifest, lock), [])

    def test_idf_range_is_allowed(self):
        # The IDF comes from the toolchain image, not the registry.
        self.assertEqual(self.check(MANIFEST, LOCK), [])


class CliTest(unittest.TestCase):
    def test_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "firmware/main").mkdir(parents=True)
            (root / "firmware/main/idf_component.yml").write_text(MANIFEST)
            (root / "firmware/dependencies.lock").write_text(LOCK)
            tool = Path(__file__).resolve().parent / "check_deps.py"
            ok = subprocess.run([sys.executable, str(tool), "--root", tmp],
                                capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            (root / "firmware/dependencies.lock").write_text(
                LOCK.replace("direct_dependencies:", TRANSITIVE.rstrip("\n") + "\ndirect_dependencies:"))
            bad = subprocess.run([sys.executable, str(tool), "--root", tmp],
                                 capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1)
            self.assertIn("espressif/cmake_utilities", bad.stderr)


if __name__ == "__main__":
    unittest.main()
