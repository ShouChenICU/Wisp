#!/usr/bin/env python3
"""Enforces Wisp's portable-layer layering rule (AGENTS.md section 3,
global-constraints.md):

  - The portable layers (wisp_base, wisp_os, wisp_hal, wisp_core,
    wisp_ui, wisp_app) must never #include a platform header: anything
    starting with esp_, freertos/, driver/, hal/ (ESP-IDF's, as
    opposed to wisp_hal's own wisp/hal/ headers), soc/ or SDL, or
    exactly sdkconfig.h.
  - wisp_os and wisp_hal additionally must never depend "upward" on
    wisp_core, wisp_ui or wisp_app (no reverse dependencies).

Prints one "path:line: forbidden include <x>" line per violation and
exits 1 if any were found, else exits 0 with no output.

Usage: python3 tools/check_layering.py [--root <repo-root>]
"""

import argparse
import pathlib
import re
import sys

# The portable components to scan, in AGENTS.md's layering order.
COMPONENTS = ("wisp_base", "wisp_os", "wisp_hal", "wisp_core", "wisp_ui", "wisp_app")
SOURCE_SUFFIXES = (".hpp", ".cpp", ".h", ".c")

# components/wisp_ui/fonts/ holds a ported third-party font (see
# AGENTS.md section 6 / docs/references.md) and isn't Wisp's own code,
# so it's excluded from the scan.
SKIPPED_DIR_NAMES = ("fonts",)

FORBIDDEN_PREFIXES = ("esp_", "freertos/", "driver/", "hal/", "soc/", "SDL")
FORBIDDEN_EXACT = ("sdkconfig.h",)

REVERSE_DEP_COMPONENTS = ("wisp_os", "wisp_hal")
REVERSE_DEP_PREFIXES = ("wisp/core/", "wisp/ui/", "wisp/app/")

INCLUDE_RE = re.compile(r'#include\s*[<"]([^">]+)[">]')


def iter_source_files(root, component):
    """Yields every source file under components/<component>/, skipping
    SKIPPED_DIR_NAMES subdirectories, in a stable (sorted) order."""
    comp_dir = root / "components" / component
    if not comp_dir.is_dir():
        return
    for path in sorted(comp_dir.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        rel_dirs = path.relative_to(comp_dir).parts[:-1]
        if any(part in SKIPPED_DIR_NAMES for part in rel_dirs):
            continue
        yield path


def is_forbidden_platform_include(include_path):
    return include_path in FORBIDDEN_EXACT or any(
        include_path.startswith(prefix) for prefix in FORBIDDEN_PREFIXES
    )


def is_forbidden_reverse_dep(include_path):
    return any(include_path.startswith(prefix) for prefix in REVERSE_DEP_PREFIXES)


def check_file(path, component, errors):
    text = path.read_text(encoding="utf-8", errors="replace")
    for lineno, line in enumerate(text.splitlines(), start=1):
        match = INCLUDE_RE.search(line)
        if not match:
            continue
        include_path = match.group(1)
        forbidden = is_forbidden_platform_include(include_path)
        if not forbidden and component in REVERSE_DEP_COMPONENTS:
            forbidden = is_forbidden_reverse_dep(include_path)
        if forbidden:
            errors.append(f"{path}:{lineno}: forbidden include <{include_path}>")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = pathlib.Path(__file__).resolve().parent.parent
    parser.add_argument(
        "--root",
        default=str(default_root),
        help="repository root to scan (default: %(default)s)",
    )
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root).resolve()

    errors = []
    for component in COMPONENTS:
        for path in iter_source_files(root, component):
            check_file(path, component, errors)

    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
