#!/usr/bin/env python3
"""Select tool self-tests; actual repository validation always runs in ci.sh."""

import argparse
import fnmatch
import os
from pathlib import Path
import subprocess
import sys


FONT_INPUTS = ("components/wisp_ui/src/strings_*.cpp", "components/wisp_ui/fonts/*")

TESTS = {
    "check_layering_test": (),
    "pack_assets_test": ("web/*", "tests/net/fixtures/*"),
    "wisp_scene_test": ("tests/smoke/*",),
    "check_i18n_test": ("web/app/i18n/*",),
    "gen_cjk_font_test": FONT_INPUTS,
    "gen_vector_font_test": (*FONT_INPUTS, "tools/gen_cjk_font*"),
    "pack_font_image_test": ("components/wisp_ui/fonts/*", "tools/gen_vector_font*"),
    "pack_glyph_image_test": ("components/wisp_ui/fonts/*", "tools/gen_cjk_font*"),
    "gen_agent_docs_test": ("docs/scene-api.md", "scenes/examples/*"),
    "gen_icon_font_test": ("components/wisp_ui/include/wisp/ui/symbols.hpp", "docs/scene-api.md"),
}


def select_tests(paths):
    if paths is None:
        return list(TESTS)
    selected = set()
    known_tools = {f"tools/{stem}.py" for name in TESTS
                   for stem in (name, name.removesuffix("_test"))}
    for path in paths:
        matches = {
            name for name, inputs in TESTS.items()
            if path in (f"tools/{name}.py", f"tools/{name.removesuffix('_test')}.py")
            or any(fnmatch.fnmatchcase(path, pattern) for pattern in inputs)
        }
        # Unknown tooling and build/dependency changes can alter shared inputs.
        if (path.startswith(("cmake/", ".github/workflows/", "third_party/"))
                or path.endswith(("CMakeLists.txt", "dependencies.lock"))
                or (path.startswith("tools/") and path not in known_tools)):
            return list(TESTS)
        selected.update(matches)
    return [name for name in TESTS if name in selected]


def changed_paths(base):
    if not base:
        return None
    try:
        # Include staged and unstaged local edits; a PR's base need not be its parent.
        merge_base = subprocess.check_output(
            ["git", "merge-base", base, "HEAD"], text=True, stderr=subprocess.DEVNULL).strip()
        changed = subprocess.check_output(
            ["git", "diff", "--no-renames", "--name-only", "-z", merge_base],
            stderr=subprocess.DEVNULL)
        untracked = subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard", "-z", "tools/"],
            stderr=subprocess.DEVNULL)
        return (changed + untracked).decode().strip("\0").split("\0")
    except (subprocess.CalledProcessError, UnicodeError):
        print("Tool test baseline unavailable; running all tool self-tests.", file=sys.stderr)
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="Git revision to compare with HEAD and local edits")
    parser.add_argument("--full", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    os.chdir(root)
    for name in select_tests(None if args.full else changed_paths(args.base)):
        print(name)


if __name__ == "__main__":
    main()
