#!/usr/bin/env python3
"""Checks the admin UI's translation dictionaries (web/app/i18n/*.js, every
file except index.js) against each other:

  - each file is `export default { ... };` with exactly one
    `'key': 'text',` entry per line (comments and blank lines allowed), so
    this script can parse it without a JavaScript engine;
  - no key appears twice in a file and no text is empty;
  - every file has the same key set as the reference (en.js);
  - for each key, every translation uses the same {placeholder} names as
    the reference, so interpolation never leaves a "{name}" behind.

Exits non-zero with one message per problem on stderr. Only the Python
standard library is used.

Usage: check_i18n.py [<dir>]   (default: web/app/i18n under the repo root)
"""

import pathlib
import re
import sys

REFERENCE = "en"
# A JS string literal in single or double quotes, with backslash escapes.
_STR = r"""(?:'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")"""
ENTRY = re.compile(r"^\s*(?P<key>" + _STR + r")\s*:\s*(?P<value>" + _STR + r")\s*,?\s*$")
PLACEHOLDER = re.compile(r"\{(\w+)\}")


def _unquote(literal):
    body = literal[1:-1]
    return re.sub(r"\\(.)", lambda m: m.group(1), body)


def parse(path):
    """Returns ({key: text}, [problems]) for one dictionary file."""
    entries = {}
    problems = []
    inside = False
    closed = False
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        where = f"{path.name}:{lineno}"
        if not stripped or stripped.startswith("//"):
            continue
        if not inside:
            if stripped == "export default {":
                inside = True
            else:
                problems.append(f"{where}: expected 'export default {{' before any entry")
            continue
        if stripped == "};":
            closed = True
            inside = False
            continue
        if closed:
            problems.append(f"{where}: content after the closing '}};'")
            continue
        m = ENTRY.match(line)
        if not m:
            problems.append(f"{where}: not a single-line 'key': 'text' entry")
            continue
        key = _unquote(m.group("key"))
        value = _unquote(m.group("value"))
        if key in entries:
            problems.append(f"{where}: duplicate key {key!r}")
        if not value.strip():
            problems.append(f"{where}: empty text for {key!r}")
        entries[key] = value
    if not closed:
        problems.append(f"{path.name}: missing 'export default {{ ... }};'")
    return entries, problems


def check(directory):
    """Returns the list of problems found in `directory`'s dictionaries."""
    files = sorted(p for p in directory.glob("*.js") if p.name != "index.js")
    if not files:
        return [f"{directory}: no dictionaries found"]
    dicts = {}
    problems = []
    for path in files:
        entries, found = parse(path)
        dicts[path.stem] = entries
        problems.extend(found)
    if REFERENCE not in dicts:
        return problems + [f"{directory}: no {REFERENCE}.js reference dictionary"]
    ref = dicts[REFERENCE]
    for lang, entries in sorted(dicts.items()):
        if lang == REFERENCE:
            continue
        for key in sorted(set(ref) - set(entries)):
            problems.append(f"{lang}.js: missing key {key!r}")
        for key in sorted(set(entries) - set(ref)):
            problems.append(f"{lang}.js: key {key!r} is not in {REFERENCE}.js")
        for key in sorted(set(ref) & set(entries)):
            want = set(PLACEHOLDER.findall(ref[key]))
            got = set(PLACEHOLDER.findall(entries[key]))
            if want != got:
                problems.append(
                    f"{lang}.js: {key!r} uses placeholders {sorted(got)}, "
                    f"{REFERENCE}.js uses {sorted(want)}"
                )
    return problems


def main(argv):
    root = pathlib.Path(__file__).resolve().parent.parent
    directory = pathlib.Path(argv[1]) if len(argv) > 1 else root / "web" / "app" / "i18n"
    problems = check(directory)
    for p in problems:
        print(f"check_i18n: {p}", file=sys.stderr)
    if problems:
        return 1
    count = len(parse(directory / f"{REFERENCE}.js")[0])
    print(f"check_i18n: {count} keys, dictionaries agree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
