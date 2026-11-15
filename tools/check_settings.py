#!/usr/bin/env python3
"""Keep setting defaults and ranges in the settings schema only
(settings-schema spec section 4, as refined by the plan's decision 5).

The rows come from the WISP_SETTINGS list of
components/wisp_core/include/wisp/core/settings_schema.hpp: one `X(...)` row
per line, 16 comma-separated fields (Id, name, C++ type, SettingType, ns,
key, min, max, def_int, def_text, int_choices, enum_values, unit, flags,
label, scene_name). Every non-blank line of the macro body must be exactly
one row, and the rows parsed must equal the X( calls in the body: a row the
pattern cannot read is an error, never silently unguarded. The arrays a row
names (choices, enum codes) are read from the same header. A row added
there is covered without touching this script.

Scanned: components/, simulator/, boards/, drivers/, firmware/main/
(.cpp .hpp .c .h) and web/app/ (.js); not the schema header itself, nor any
tests/ or third_party/ directory. Comments are scanned too: a default restated in a comment drifts
as surely as one in code. A finding is:
  1. Raw store access: a row's ns as a string literal in a `Settings(` call
     or as the first argument of a backend `get(`/`set(`/`erase(`, or a
     row's key as the first argument of `get_int(`/`get_bool(`/`get_string(`,
     or of a `set(` with a second argument.
  2. On a line holding a token of an Int row, an integer literal equal to
     that row's default, min or max; never 0 or 1, nor 100 on a Percent row
     (full scale is not a setting fact).
  3. The HostList default text or any of its entries, anywhere; an Enum
     row's default code assigned (`= "zh"`, `{"zh"}`, `?? 'zh'`, `|| 'zh'`)
     on a line holding that row's token. Comparisons and returns stay legal.
Identifiers are split into lower-case words (snake, camel and SCREAMING
case); a row's tokens are its name, key, label and scene name as word
sequences, plus the name without a trailing s/min/pct word when at least two
words remain (screen_off, utc_offset; not turn). A token matches when its
words appear in order inside one identifier. A comment-only line (// or a
/* */ block) also holds the tokens of the code line its comment documents.

A line ending in `// settings-literal-ok: <reason>` is skipped; an empty
reason is itself a finding. Prints "path:line: message" per finding and then
"settings-literal-ok markers: <n>"; exits 1 on any finding. Standard library
only.
"""

import argparse
import dataclasses
import functools
import pathlib
import re
import sys

SCHEMA_HPP = "components/wisp_core/include/wisp/core/settings_schema.hpp"
CODE_DIRS = ("components", "simulator", "boards", "drivers", "firmware/main")
CODE_SUFFIXES = {".cpp", ".hpp", ".c", ".h"}
WEB_DIR = "web/app"
WEB_SUFFIXES = {".js"}
SKIP_PARTS = {"tests", "third_party"}
FIELDS = 16
MARKER = "settings-literal-ok:"

_ROW_RE = re.compile(r"^\s*X\s*\((.*)\)\s*\\?\s*$")
_DEFINE_RE = re.compile(r"^\s*#\s*define\s+WISP_SETTINGS\s*\(\s*X\s*\)")
_X_CALL_RE = re.compile(r"\bX\s*\(")
_ARRAY_RE = re.compile(r"\b(k\w+)\s*\[\s*\]\s*=\s*\{([^}]*)\}")
_INT_RE = re.compile(r"-?\d+")
_STR_RE = re.compile(r'"([^"]*)"')
_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_WORD_RE = re.compile(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+")
_NUMBER_RE = re.compile(r"(?<!\w)(?<!\d\.)(-?)(\d+)[uUlL]{0,3}(?!\w|\.\d)")
_MARKER_RE = re.compile(r"//\s*" + re.escape(MARKER) + r"(.*)$")
_TYPED_GET_RE = re.compile(r"\bget_(?:int|bool|string)\s*\(\s*\"([^\"]*)\"")
_KEY_SET_RE = re.compile(r"\bset\s*\(\s*\"([^\"]*)\"\s*,")
_SETTINGS_CALL_RE = re.compile(r"\bSettings\b\s*(?:\w+\s*)?[({]([^;]*)")
_BACKEND_RE = re.compile(r"(?:\.|->)\s*(?:get|set|erase)\s*\(\s*\"([^\"]*)\"\s*,")


class SchemaError(Exception):
    """The schema header is missing or a row does not parse."""


@dataclasses.dataclass
class Row:
    name: str
    type: str
    ns: str
    key: str
    min: int
    max: int
    def_int: int
    def_text: str
    int_choices: list
    enum_values: list
    unit: str
    label: str
    scene_name: str
    line: int
    tokens: list = dataclasses.field(default_factory=list)


@functools.lru_cache(maxsize=None)
def words(identifier):
    """The lower-case words of an identifier, as a tuple."""
    out = []
    for part in identifier.split("_"):
        out.extend(w.lower() for w in _WORD_RE.findall(part))
    return tuple(out)


def _split_fields(text):
    """Split on commas outside string literals."""
    fields, current, quoted = [], [], False
    for c in text:
        if c == '"':
            quoted = not quoted
        if c == "," and not quoted:
            fields.append("".join(current).strip())
            current = []
        else:
            current.append(c)
    fields.append("".join(current).strip())
    return fields


def _unquote(field):
    if len(field) < 2 or field[0] != '"' or field[-1] != '"':
        raise ValueError(f"expected a string literal, got {field!r}")
    return field[1:-1]


def _tokens(row):
    tokens = {words(t) for t in (row.name, row.key, row.label, row.scene_name) if t}
    name = words(row.name)
    if len(name) >= 3 and name[-1] in ("s", "min", "pct"):
        tokens.add(name[:-1])
    return sorted(t for t in tokens if t)


def load_rows(root):
    """Parse the WISP_SETTINGS rows of the schema header under `root`."""
    path = pathlib.Path(root) / SCHEMA_HPP
    try:
        text = path.read_text()
    except OSError as exc:
        raise SchemaError(f"{SCHEMA_HPP}: cannot read: {exc.strerror}") from exc
    arrays = {m.group(1): m.group(2) for m in _ARRAY_RE.finditer(text)}

    def int_array(name, line):
        if name == "kNoIntChoices":
            return []
        if name not in arrays:
            raise SchemaError(f"{SCHEMA_HPP}:{line}: unknown choices array {name}")
        return [int(v) for v in _INT_RE.findall(arrays[name])]

    def str_array(name, line):
        if name == "kNoEnumValues":
            return []
        if name not in arrays:
            raise SchemaError(f"{SCHEMA_HPP}:{line}: unknown values array {name}")
        return _STR_RE.findall(arrays[name])

    # The macro body: from the line after `#define WISP_SETTINGS(X) \` to
    # the first line without a trailing backslash. Every non-blank line in
    # it must be exactly one row, so a row split over two lines or spelled
    # in a way the pattern misses is an error rather than silently unguarded.
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if _DEFINE_RE.match(line)), None)
    if start is None:
        raise SchemaError(f"{SCHEMA_HPP}: no #define WISP_SETTINGS(X) found")
    body = []
    i = start
    while lines[i].rstrip().endswith("\\") and i + 1 < len(lines):
        i += 1
        body.append((i + 1, lines[i]))
    rows = []
    for number, line in body:
        if not line.strip() or line.strip() == "\\":
            continue
        m = _ROW_RE.match(line)
        if not m or len(_X_CALL_RE.findall(line)) != 1:
            raise SchemaError(f"{SCHEMA_HPP}:{number}: not exactly one X(...) settings row "
                              f"on this line (one row per line, tools/check_settings.py)")
        f = _split_fields(m.group(1))
        if len(f) != FIELDS:
            raise SchemaError(f"{SCHEMA_HPP}:{number}: a settings row has {len(f)} fields, "
                              f"expected {FIELDS}")
        try:
            row = Row(name=f[1], type=f[3], ns=_unquote(f[4]), key=_unquote(f[5]),
                      min=int(f[6]), max=int(f[7]), def_int=int(f[8]),
                      def_text=_unquote(f[9]), int_choices=int_array(f[10], number),
                      enum_values=str_array(f[11], number), unit=f[12],
                      label=_unquote(f[14]), scene_name=_unquote(f[15]), line=number)
        except ValueError as exc:
            raise SchemaError(f"{SCHEMA_HPP}:{number}: cannot parse a settings row: {exc}") \
                from exc
        row.tokens = _tokens(row)
        rows.append(row)
    if not rows:
        raise SchemaError(f"{SCHEMA_HPP}: no X(...) settings rows found")
    calls = sum(len(_X_CALL_RE.findall(line)) for _n, line in body)
    if calls != len(rows):
        raise SchemaError(f"{SCHEMA_HPP}:{start + 1}: WISP_SETTINGS has {calls} X( rows, "
                          f"parsed {len(rows)}")
    return rows


class _Matcher:
    def __init__(self, rows):
        self.int_rows = [r for r in rows if r.type == "Int"]
        self.enum_rows = [r for r in rows if r.type == "Enum" and r.enum_values]
        self.namespaces = {r.ns for r in rows}
        self.keys = {r.key: r for r in rows}
        # Token first word -> (token, row), for the rows rule 2 and 3 look at.
        self.by_first = {}
        for r in self.int_rows + self.enum_rows:
            for t in r.tokens:
                self.by_first.setdefault(t[0], []).append((t, r))
        first_words = sorted({w for w in self.by_first}, key=len, reverse=True)
        self.prefilter = re.compile("|".join(re.escape(w) for w in first_words), re.I)
        self.values = {}
        for r in self.int_rows:
            vals = {r.def_int, r.min, r.max} - {0, 1}
            if r.unit == "Percent":
                vals.discard(100)
            self.values[r.name] = vals
        self.host_texts = []
        for r in rows:
            if r.type == "HostList" and r.def_text:
                entries = [e for e in r.def_text.split(",") if e]
                self.host_texts.append((r, [r.def_text] + entries))
        self.enum_res = {}
        for r in self.enum_rows:
            code = re.escape(r.enum_values[r.def_int])
            q = r"""["']""" + code + r"""["']"""
            self.enum_res[r.name] = re.compile(
                r"(?<![=!<>])=\s*" + q + r"|\{\s*" + q + r"\s*\}|(?:\?\?|\|\|)\s*" + q)

    def rows_on(self, line):
        """The rows whose token appears inside an identifier of `line` (a
        line's context, _contexts())."""
        found = {}
        for ident in _IDENT_RE.findall(line):
            w = words(ident)
            for i, first in enumerate(w):
                for token, row in self.by_first.get(first, ()):
                    if w[i:i + len(token)] == token:
                        found[row.name] = row
        return found.values()

    def findings(self, line, context):
        reasons = []
        for m in _TYPED_GET_RE.finditer(line):
            if m.group(1) in self.keys:
                reasons.append(f'raw store read of setting "{self.keys[m.group(1)].name}"')
        for m in _KEY_SET_RE.finditer(line):
            if m.group(1) in self.keys:
                reasons.append(f'raw store write of setting "{self.keys[m.group(1)].name}"')
        raw_ns = [m.group(1) for m in _BACKEND_RE.finditer(line)]
        for m in _SETTINGS_CALL_RE.finditer(line):
            raw_ns += _STR_RE.findall(m.group(1))
        for ns in raw_ns:
            if ns in self.namespaces:
                reasons.append(f'raw store access to settings namespace "{ns}"')
        for row, texts in self.host_texts:
            for t in texts:
                if t in line:
                    reasons.append(f'"{t}" restates the default of setting "{row.name}"')
                    break
        if self.prefilter.search(context):
            numbers = None
            for row in self.rows_on(context):
                if row.type == "Enum":
                    if self.enum_res[row.name].search(line):
                        code = row.enum_values[row.def_int]
                        reasons.append(f'"{code}" restates the default of setting "{row.name}"')
                    continue
                if numbers is None:
                    numbers = {int(m.group(1) + m.group(2)) for m in _NUMBER_RE.finditer(line)}
                hit = sorted(numbers & self.values[row.name])
                if hit:
                    what = ", ".join(str(v) for v in hit)
                    reasons.append(f'{what} restates the default or range of setting "{row.name}"')
        return reasons


def _files(root):
    for top, suffixes in [(d, CODE_SUFFIXES) for d in CODE_DIRS] + [(WEB_DIR, WEB_SUFFIXES)]:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in suffixes or not path.is_file():
                continue
            rel = path.relative_to(root)
            if SKIP_PARTS & set(rel.parts) or rel.as_posix() == SCHEMA_HPP:
                continue
            yield rel.as_posix(), path


def _comment_lines(lines):
    """Per line, whether it is comment only: it starts with // or /*, or
    lies inside a /* ... */ block. A line starting with * outside a block
    is code (`*out = 60;`)."""
    out, in_block = [], False
    for line in lines:
        stripped = line.lstrip()
        if in_block:
            out.append(True)
            in_block = "*/" not in line
            continue
        if stripped.startswith("/*"):
            out.append(True)
            in_block = "*/" not in stripped[2:]
            continue
        out.append(stripped.startswith("//"))
        # A block opened after code on this line (`x; /* ...`) runs on.
        opened = line.rfind("/*")
        if opened >= 0 and "*/" not in line[opened + 2:] and "//" not in line[:opened]:
            in_block = True
    return out


def _contexts(lines):
    """Per line, the text whose tokens count for it: the line itself, plus
    for a comment-only line the code line its comment block documents, so
    "Empty: 480." above `utc_offset_min` counts as next to it. Not the whole
    block: long header comments name many settings and many numbers."""
    contexts = list(lines)
    comment = _comment_lines(lines)
    documented = ""
    for k in range(len(lines) - 1, -1, -1):
        if comment[k]:
            contexts[k] = lines[k] + "\n" + documented
        else:
            documented = lines[k]
    return contexts


def _scan(root):
    """Return (findings, marker count)."""
    root = pathlib.Path(root)
    try:
        matcher = _Matcher(load_rows(root))
    except SchemaError as exc:
        return [str(exc)], 0
    errors, markers = [], 0
    hint = "; read it from the settings schema (wisp/core/settings_schema.hpp)"
    for rel, path in _files(root):
        lines = path.read_text(errors="replace").splitlines()
        contexts = _contexts(lines)
        for number, line in enumerate(lines, 1):
            marker = _MARKER_RE.search(line)
            if marker:
                if marker.group(1).strip():
                    markers += 1
                else:
                    errors.append(f"{rel}:{number}: settings-literal-ok marker without a reason")
                continue
            reasons = matcher.findings(line, contexts[number - 1])
            if reasons:
                errors.append(f"{rel}:{number}: " + "; ".join(reasons) + hint)
    return errors, markers


def check(root):
    return _scan(root)[0]


def count_markers(root):
    return _scan(root)[1]


def main(argv=None):
    default_root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=default_root)
    args = parser.parse_args(argv)
    errors, markers = _scan(args.root)
    for line in errors:
        print(line)
    print(f"settings-literal-ok markers: {markers}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
