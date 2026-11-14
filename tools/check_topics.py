#!/usr/bin/env python3
"""Check the EventBus topic registry (spec 3.7 of the state-cells design).

Two rules:
  1. The names registered in `kAll` of components/wisp_core/include/wisp/core/
     topics.hpp equal the first-column names of the table under
     "## Event topics" in docs/architecture.md. A cell is split on " / ";
     a piece starting with "." replaces the last dotted segment of the
     previous name ("`button.<id>.down` / `.up`" gives "button.<id>.up").
  2. In components/ and simulator/ (except topics.hpp itself) no ordinary
     string literal equals a registered topic name; code uses the constants.
     Registered names containing '<' are patterns (the button topics are
     built per device) and are not checked. Only an exact match is flagged:
     prefixes such as "net." stay legal for subscriptions.

Prints "path:line: message" lines and exits 1 when any rule fails; silent
on success. Standard library only.
"""

import argparse
import pathlib
import re
import sys

TOPICS_HPP = "components/wisp_core/include/wisp/core/topics.hpp"
ARCH_DOC = "docs/architecture.md"
SCAN_DIRS = ("components", "simulator")
SUFFIXES = {".cpp", ".hpp"}

_CONST_RE = re.compile(r'\b(k\w+)\s*\[\s*\]\s*=\s*"([^"]*)"')
_ENTRY_RE = re.compile(r'\{\s*(k\w+|"[^"]*")\s*,\s*Kind::')
_RAW_PREFIXES = {"", "u8", "u", "U", "L"}


def load_registry(root):
    """Return the registered topics as a list of (name, constant or None)."""
    text = (root / TOPICS_HPP).read_text()
    constants = {m.group(1): m.group(2) for m in _CONST_RE.finditer(text)}
    start = text.find("kAll[]")
    body = text[start:text.find("};", start)] if start >= 0 else ""
    topics = []
    for m in _ENTRY_RE.finditer(body):
        ref = m.group(1)
        if ref.startswith('"'):
            topics.append((ref[1:-1], None))
        elif ref in constants:
            topics.append((constants[ref], ref))
    return topics


def doc_rows(root):
    """Return the (name, line) of every topic named in the table's first column."""
    rows = []
    in_section = False
    for number, line in enumerate((root / ARCH_DOC).read_text().splitlines(), 1):
        if line.startswith("## "):
            in_section = line.strip() == "## Event topics"
            continue
        if not in_section or not line.startswith("|"):
            continue
        cell = line.split("|")[1].strip()
        if cell == "Topic" or set(cell) <= {"-", " ", ":"}:
            continue
        previous = None
        for piece in cell.split(" / "):
            name = piece.strip().strip("`")
            if name.startswith(".") and previous:
                name = previous.rsplit(".", 1)[0] + name
            rows.append((name, number))
            previous = name
    return rows


def check_docs(root, topics):
    errors = []
    registered = {name for name, _ in topics}
    rows = doc_rows(root)
    documented = {name for name, _ in rows}
    for name, _ in topics:
        if name not in documented:
            errors.append(f'{ARCH_DOC}: topic "{name}" is registered but has no row')
    for name, number in rows:
        if name not in registered:
            errors.append(f'{ARCH_DOC}:{number}: topic "{name}" has a row but is not registered')
    return errors


def string_literals(text):
    """Yield (line, value) of every ordinary string literal in C++ source.

    A small state machine rather than a regex: a comment stripper that does
    not know strings lets "http://x" blank the rest of a line and "/*" blank
    later lines. Raw strings and char literals are skipped; a char literal
    ends at the newline at the latest, which also absorbs C++14 digit
    separators such as 20'000.
    """
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            line += text.count("\n", i, end)
            i = end
        elif c == "'":
            j = i
            while j > 0 and (text[j - 1].isalnum() or text[j - 1] == "_"):
                j -= 1
            if j < i and text[j].isdigit():
                i += 1  # digit separator, not a char literal
                continue
            i += 1
            while i < n and text[i] not in "'\n":
                i += 2 if text[i] == "\\" else 1
            if i < n and text[i] == "'":
                i += 1
        elif c == '"':
            j = i
            while j > 0 and (text[j - 1].isalnum() or text[j - 1] == "_"):
                j -= 1
            token = text[j:i]
            if token.endswith("R") and token[:-1] in _RAW_PREFIXES and "(" in text[i:i + 20]:
                open_paren = text.index("(", i)
                closer = ")" + text[i + 1:open_paren] + '"'
                end = text.find(closer, open_paren)
                end = n if end < 0 else end + len(closer)
                line += text.count("\n", i, end)
                i = end
                continue
            start_line = line
            i += 1
            value = []
            while i < n and text[i] not in '"\n':
                if text[i] == "\\" and i + 1 < n:
                    value.append(text[i:i + 2])
                    i += 2
                else:
                    value.append(text[i])
                    i += 1
            if i < n and text[i] == '"':
                yield start_line, "".join(value)
                i += 1
        else:
            i += 1


def check_literals(root, topics):
    errors = []
    constants = {name: const for name, const in topics if "<" not in name and const}
    names = {name: const for name, const in constants.items()}
    for name, _ in topics:
        if "<" not in name and name not in names:
            names[name] = "?"
    skip = root / TOPICS_HPP
    for top in SCAN_DIRS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SUFFIXES or path == skip or not path.is_file():
                continue
            rel = path.relative_to(root).as_posix()
            for line, value in string_literals(path.read_text(errors="replace")):
                if value in names:
                    errors.append(f'{rel}:{line}: topic literal "{value}"; '
                                  f'use wisp::core::topics::{names[value]}')
    return errors


def check(root):
    topics = load_registry(root)
    return check_docs(root, topics) + check_literals(root, topics)


def main(argv=None):
    default_root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=default_root)
    args = parser.parse_args(argv)
    errors = check(args.root)
    for line in errors:
        print(line)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
