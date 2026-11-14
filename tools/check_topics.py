#!/usr/bin/env python3
"""Check the EventBus topic registry (spec 3.7 of the state-cells design).

Two rules:
  1. The names registered in `kAll` of components/wisp_core/include/wisp/core/
     topics.hpp equal the first-column names of the table under
     "## Event topics" in docs/architecture.md. A cell is split on " / ";
     a piece starting with "." replaces the last dotted segment of the
     previous name ("`button.<name>.down` / `.up`" gives "button.<name>.up").
  2. In components/ and simulator/ (except topics.hpp itself) no ordinary
     string literal equals a registered topic name; code uses the constants.
     Registered names containing '<' are patterns (the button topics are
     built per device) and are not checked. Only an exact match is flagged:
     prefixes such as "net." stay legal for subscriptions.
Also checked: a name in kAll that is not a defined constant, a name
registered twice, and a constant of topics.hpp that is absent from kAll.
A constant counts as a topic name when its value contains a '.' and does
not end with '.'; that leaves out the button prefix ("button.") and the
edge names ("down", "up", ...), which are parts of a topic, not topics.
The doc table is the first table under the "## Event topics" heading; a
heading closes it. A table row after the table's first break (a blank line)
in the same section is reported rather than skipped.
Each first-column piece must be exactly one backticked name.

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
_KALL_RE = re.compile(r"\bkAll\s*\[\s*\]\s*=\s*\{")
_ENTRY_RE = re.compile(r'\{\s*(k\w+|"[^"]*")\s*,\s*Kind::')
_PIECE_RE = re.compile(r"^`([^`\s]+)`$")
_RAW_PREFIXES = {"", "u8", "u", "U", "L"}


def _word_start(text, i):
    while i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_"):
        i -= 1
    return i


def _tokens(text):
    """Yield ("comment", start, end, line, None) and ("string", start, end,
    line, value) for the comments and ordinary string literals of C++ source.

    A small state machine rather than a regex: a comment stripper that does
    not know strings lets "http://x" blank the rest of a line and "/*" blank
    later lines. Raw strings and char literals are skipped; a char literal
    ends at the newline at the latest, which also absorbs C++14 digit
    separators such as 20'000. `line` is the line the token starts on.
    """
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            yield "comment", i, end, line, None
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            yield "comment", i, end, line, None
            line += text.count("\n", i, end)
            i = end
        elif c == "'":
            j = _word_start(text, i)
            if j < i and text[j].isdigit():
                i += 1  # digit separator, not a char literal
                continue
            i += 1
            while i < n and text[i] not in "'\n":
                if text[i] == "\\" and text[i + 1:i + 2] == "\n":
                    line += 1
                i += 2 if text[i] == "\\" else 1
            if i < n and text[i] == "'":
                i += 1
        elif c == '"':
            token = text[_word_start(text, i):i]
            if token.endswith("R") and token[:-1] in _RAW_PREFIXES and "(" in text[i:i + 20]:
                open_paren = text.index("(", i)
                closer = ")" + text[i + 1:open_paren] + '"'
                end = text.find(closer, open_paren)
                end = n if end < 0 else end + len(closer)
                line += text.count("\n", i, end)
                i = end
                continue
            start, start_line = i, line
            i += 1
            value = []
            while i < n and text[i] not in '"\n':
                if text[i] == "\\" and i + 1 < n:
                    if text[i + 1] == "\n":
                        line += 1
                    value.append(text[i:i + 2])
                    i += 2
                else:
                    value.append(text[i])
                    i += 1
            if i < n and text[i] == '"':
                i += 1
                yield "string", start, i, start_line, "".join(value)
        else:
            i += 1


def string_literals(text):
    """Yield (line, value) of every ordinary string literal in C++ source."""
    for kind, _start, _end, line, value in _tokens(text):
        if kind == "string":
            yield line, value


def blank_comments(text):
    """Replace every comment by spaces, keeping newlines and offsets."""
    out = list(text)
    for kind, start, end, _line, _ in _tokens(text):
        if kind == "comment":
            for k in range(start, end):
                if out[k] != "\n":
                    out[k] = " "
    return "".join(out)


def load_registry(root):
    """Return (entries, errors); entries are (name, constant or None, line)."""
    path = root / TOPICS_HPP
    errors = []
    try:
        text = blank_comments(path.read_text())
    except OSError as exc:
        return [], [f"{TOPICS_HPP}: cannot read: {exc.strerror}"]

    def line_of(offset):
        return text.count("\n", 0, offset) + 1

    constants = {}
    for m in _CONST_RE.finditer(text):
        constants[m.group(1)] = (m.group(2), line_of(m.start()))
    start = _KALL_RE.search(text)
    if not start:
        return [], [f"{TOPICS_HPP}: no kAll array found"]
    body_end = text.find("};", start.end())
    body_end = len(text) if body_end < 0 else body_end
    entries, in_kall = [], set()
    for m in _ENTRY_RE.finditer(text, start.end(), body_end):
        ref, line = m.group(1), line_of(m.start())
        if ref.startswith('"'):
            entries.append((ref[1:-1], None, line))
        elif ref in constants:
            in_kall.add(ref)
            entries.append((constants[ref][0], ref, line))
        else:
            errors.append(f'{TOPICS_HPP}:{line}: kAll names undefined constant "{ref}"')
    seen = set()
    for name, _const, line in entries:
        if name in seen:
            errors.append(f'{TOPICS_HPP}:{line}: topic "{name}" is registered twice')
        seen.add(name)
    registered = {name for name, _c, _l in entries}
    for const, (value, line) in constants.items():
        is_topic = "." in value and not value.endswith(".")
        if is_topic and const not in in_kall and value not in registered:
            errors.append(f"{TOPICS_HPP}:{line}: constant {const} is not in kAll")
    return entries, errors


def doc_rows(root):
    """Return (rows, errors); rows are (name, line) of the table's first column."""
    try:
        lines = (root / ARCH_DOC).read_text().splitlines()
    except OSError as exc:
        return [], [f"{ARCH_DOC}: cannot read: {exc.strerror}"]
    rows, errors = [], []
    in_section = started = ended = False
    for number, line in enumerate(lines, 1):
        if line.startswith("#"):
            if started:
                break
            in_section = line.strip() == "## Event topics"
            continue
        if not in_section:
            continue
        if not line.startswith("|"):
            if started:
                ended = True
            continue
        if ended:
            # A blank line inside the table would otherwise hide the rows
            # after it from the comparison with the registry.
            errors.append(f"{ARCH_DOC}:{number}: topic table continues after a break; "
                          "keep it one contiguous table")
            break
        started = True
        cell = line.split("|")[1].strip()
        if cell == "Topic" or set(cell) <= {"-", " ", ":"}:
            continue
        names = []
        for piece in cell.split(" / "):
            m = _PIECE_RE.match(piece.strip())
            if not m:
                names = None
                break
            name = m.group(1)
            if name.startswith(".") and names:
                name = names[-1].rsplit(".", 1)[0] + name
            names.append(name)
        if names is None:
            errors.append(f'{ARCH_DOC}:{number}: malformed topic cell "{cell}"')
        else:
            rows.extend((name, number) for name in names)
    return rows, errors


def check_docs(root, entries):
    rows, errors = doc_rows(root)
    if errors and not rows and any("cannot read" in e for e in errors):
        return errors
    registered = {name for name, _c, _l in entries}
    documented = {name for name, _ in rows}
    for name, _c, _l in entries:
        if name not in documented:
            errors.append(f'{ARCH_DOC}: topic "{name}" is registered but has no row')
    for name, number in rows:
        if name not in registered:
            errors.append(f'{ARCH_DOC}:{number}: topic "{name}" has a row but is not registered')
    return errors


def check_literals(root, entries):
    errors = []
    constant_of = {name: const for name, const, _l in entries if "<" not in name}
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
                if value in constant_of:
                    const = constant_of[value]
                    hint = (f"use wisp::core::topics::{const}" if const
                            else "register a constant for it in topics.hpp")
                    errors.append(f'{rel}:{line}: topic literal "{value}"; {hint}')
    return errors


def check(root):
    entries, errors = load_registry(root)
    if not entries and errors:
        return errors
    return errors + check_docs(root, entries) + check_literals(root, entries)


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
