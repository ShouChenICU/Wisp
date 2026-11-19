#!/usr/bin/env python3
"""Check that every registry component in firmware/dependencies.lock is pinned.

A component that only arrives through another component's version range
("~0.7.1", "^1", "0.*") is re-resolved to the newest matching release by
any fresh configure (CI's clean checkout), which rewrites the lock. So
every registry ("service") component in the lock must be a dependency of
firmware/main/idf_component.yml, pinned to exactly the version the lock
holds (AGENTS.md section 5, "Dependencies"). The IDF itself comes from the
toolchain and may keep its range.

Offline and dependency-free: the manifest and the lock are plain,
machine-written YAML, read here line by line.
"""

import argparse
from pathlib import Path
import re
import sys

MANIFEST = "firmware/main/idf_component.yml"
LOCK = "firmware/dependencies.lock"

# A range: an operator prefix, a wildcard or a list. "==1.0.2" is exact,
# and so is a "~1" revision suffix ("1.0.1~1").
_RANGE = re.compile(r"^(?!==)[~^<>=!]|[*,| ]")


def _exact(spec):
    """The version an exact spec names, or None for a range."""
    if spec is None or _RANGE.search(spec):
        return None
    return spec[2:] if spec.startswith("==") else spec


def _unquote(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    return value


def _indent(line):
    return len(line) - len(line.lstrip(" "))


def parse_manifest(text):
    """{name: version spec} under the manifest's top-level `dependencies:`."""
    deps = {}
    inside = False
    current = None
    for raw in text.splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = _indent(raw)
        line = raw.strip()
        if indent == 0:
            inside = line == "dependencies:"
            current = None
            continue
        if not inside:
            continue
        key, _, value = line.partition(":")
        if indent == 2:
            current = _unquote(key)
            deps[current] = _unquote(value) if value.strip() else None
        elif indent == 4 and current is not None and key == "version":
            deps[current] = _unquote(value)
    return deps


def parse_lock(text):
    """{name: (source type, version)} for each entry of the lock's `dependencies:`."""
    entries = {}
    inside = False
    current = None
    in_source = False
    for raw in text.splitlines():
        if not raw.strip():
            continue
        indent = _indent(raw)
        line = raw.strip()
        if indent == 0:
            inside = line == "dependencies:"
            current = None
            continue
        if not inside:
            continue
        key, _, value = line.partition(":")
        if indent == 2:
            current = _unquote(key)
            entries[current] = [None, None]
            in_source = False
        elif current is None:
            continue
        elif indent == 4:
            in_source = key == "source"
            if key == "version":
                entries[current][1] = _unquote(value)
        elif indent == 6 and in_source and key == "type":
            entries[current][0] = _unquote(value)
    return {name: tuple(v) for name, v in entries.items()}


def check(manifest, lock):
    """The problems, one message each; empty when everything is pinned."""
    errors = []
    for name, spec in sorted(manifest.items()):
        if name != "idf" and _exact(spec) is None:
            errors.append(f'{name}: "{spec}" in {MANIFEST} is not an exact version')
    for name, (source, version) in sorted(lock.items()):
        if source != "service":
            continue
        if name not in manifest:
            errors.append(
                f'{name} {version} is locked but not pinned in {MANIFEST}; add '
                f'`{name}: "{version}"` so a fresh resolve cannot move it')
        elif _exact(manifest[name]) not in (None, version):
            errors.append(
                f'{name}: {MANIFEST} asks for "{manifest[name]}" but the lock holds {version}')
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=Path(__file__).resolve().parent.parent, type=Path)
    args = parser.parse_args(argv)
    manifest = parse_manifest((args.root / MANIFEST).read_text())
    lock = parse_lock((args.root / LOCK).read_text())
    errors = check(manifest, lock)
    for error in errors:
        print(f"check_deps: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
