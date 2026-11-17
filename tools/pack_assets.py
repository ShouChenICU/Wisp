#!/usr/bin/env python3
"""Packs a directory of web assets into a WispPack (spec 2.3), the read-only
format linked into the application image and net::AssetPack
(components/wisp_net/src/asset_pack.cpp) parses. Both sides must agree on
this layout; everything is little-endian:

  header (16 bytes): b"WPAK", u16 version=1, u16 count, u32 table_bytes,
                     u32 total_bytes
  table (table_bytes, `count` records back to back):
                     u16 path_len, path (UTF-8, starts with "/"),
                     u8 mime_len, mime, u8 flags (bit0 = gzip),
                     u32 offset, u32 length, 32-byte sha256
  data:              each file's stored bytes at `offset` (measured from the
                     start of the header), 4-byte aligned, zero padded

The sha256 is over the stored (possibly gzipped) bytes, because that is what
is sent on the wire and what the ETag has to identify.

Text formats are gzipped with mtime=0 and a fixed level so the same input
always produces byte-identical output (firmware images stay reproducible);
already-compressed formats are stored as-is. Files are ordered by path.

Before that, .js/.mjs and .css files lose what only their readers need
(minify()): leading indentation, blank lines and whole-line // comments in
JavaScript, comments, indentation and blank lines in CSS. Lines inside
template literals, strings and block comments are kept as they are, and no
two lines are joined, so automatic semicolon insertion sees the same code.
HTML is stored as written.

Checks, each failing with a non-zero exit and a message on stderr:
  - every relative/absolute module specifier in a .js/.mjs file
    (`import ... from '...'`, `export ... from '...'`, `import '...'`,
    `import('...')`) resolves to a file inside the pack;
  - every <script src> and <link href> in an .html file is a data: URL or
    resolves to a file inside the pack -- never another host, since a
    phone on the setup hotspot has no internet (spec 7.1);
  - the whole pack is at most MAX_PACK_BYTES (256 KiB), the room the
    application image keeps for it (spec 2.3);
  - --budget-kb: the gzipped files together fit the budget.

Only the Python standard library is used.

Usage: pack_assets.py <src_dir> -o <out.wpak> [--budget-kb N]
"""

import argparse
import gzip
import hashlib
import pathlib
import posixpath
import re
import struct
import sys

MAGIC = b"WPAK"
VERSION = 1
HEADER = struct.Struct("<4sHHII")
# The pack is linked into the application image (spec 2.3); this much of
# the image is set aside for it.
MAX_PACK_BYTES = 256 * 1024
ALIGN = 4
FLAG_GZIP = 0x01

GZIP_SUFFIXES = {"html", "js", "mjs", "css", "svg", "json", "txt"}

MIME_TYPES = {
    "html": "text/html; charset=utf-8",
    "js": "text/javascript; charset=utf-8",
    "mjs": "text/javascript; charset=utf-8",
    "css": "text/css; charset=utf-8",
    "svg": "image/svg+xml",
    "json": "application/json",
    "txt": "text/plain; charset=utf-8",
    "png": "image/png",
    "jpg": "image/jpeg",
    "jpeg": "image/jpeg",
    "gif": "image/gif",
    "webp": "image/webp",
    "ico": "image/x-icon",
    "woff2": "font/woff2",
    "wasm": "application/wasm",
}
DEFAULT_MIME = "application/octet-stream"

# Module specifiers in static imports/re-exports, bare side-effect imports
# and dynamic import() calls. Template-literal or computed specifiers can't
# be checked statically and are deliberately not matched.
IMPORT_RES = (
    re.compile(r"""\b(?:import|export)\b[^'";]*?\bfrom\s*(['"])([^'"\n]+)\1"""),
    re.compile(r"""\bimport\s*(['"])([^'"\n]+)\1"""),
    re.compile(r"""\bimport\s*\(\s*(['"])([^'"\n]+)\1\s*\)"""),
)

# <script src="..."> and <link href="..."> in HTML pages.
# The value may be double-quoted, single-quoted or unquoted (HTML allows
# all three); exactly one of the three groups matches.
HTML_REF_RE = re.compile(
    r"""<(?:script|link)\b[^>]*?\b(?:src|href)\s*=\s*"""
    r"""(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+))""",
    re.IGNORECASE,
)
URL_SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


class PackError(Exception):
    pass


def suffix_of(path):
    return path.rsplit(".", 1)[-1].lower() if "." in path.rsplit("/", 1)[-1] else ""


def collect(src_dir):
    """Returns [(pack_path, bytes)] for every regular file under src_dir,
    sorted by pack path so the output never depends on directory order."""
    files = []
    for p in src_dir.rglob("*"):
        if p.is_file():
            files.append(("/" + p.relative_to(src_dir).as_posix(), p.read_bytes()))
    files.sort(key=lambda f: f[0])
    return files


def check_imports(files):
    present = {path for path, _ in files}
    missing = []
    for path, data in files:
        if suffix_of(path) not in ("js", "mjs"):
            continue
        text = data.decode("utf-8", errors="replace")
        for regex in IMPORT_RES:
            for match in regex.finditer(text):
                spec = match.group(2)
                if spec.startswith(("./", "../")):
                    target = posixpath.normpath(posixpath.join(posixpath.dirname(path), spec))
                elif spec.startswith("/") and not spec.startswith("//"):
                    target = posixpath.normpath(spec)
                else:
                    continue  # bare or URL specifier: not ours to resolve
                if target not in present:
                    missing.append(f"{path}: import '{spec}' does not resolve inside the pack")
    if missing:
        raise PackError("\n".join(sorted(set(missing))))


def check_html_refs(files):
    present = {path for path, _ in files}
    problems = []
    for path, data in files:
        if suffix_of(path) != "html":
            continue
        text = data.decode("utf-8", errors="replace")
        for match in HTML_REF_RE.finditer(text):
            ref = next(g for g in match.groups() if g is not None).strip()
            if not ref or ref.startswith("#") or ref.lower().startswith("data:"):
                continue
            if ref.startswith("//") or URL_SCHEME_RE.match(ref):
                problems.append(f"{path}: '{ref}' is not inside the pack")
                continue
            ref = ref.split("#", 1)[0].split("?", 1)[0]
            if ref.startswith("/"):
                target = posixpath.normpath(ref)
            else:
                target = posixpath.normpath(posixpath.join(posixpath.dirname(path), ref))
            if target not in present:
                problems.append(f"{path}: '{ref}' does not resolve inside the pack")
    if problems:
        raise PackError("\n".join(sorted(set(problems))))


# Characters after which a '/' in JavaScript starts a regular expression
# rather than dividing, and the keywords that do the same. A '}' is taken
# as the end of a block; dividing an object literal is not something the
# web UI does.
REGEX_AFTER_CHARS = set("(,=:[!&|?{};+-*%<>~^")
REGEX_AFTER_WORDS = {"return", "typeof", "case", "do", "else", "in", "of", "new", "delete",
                     "void", "throw", "yield", "await", "instanceof"}


def is_word_char(c):
    return c.isalnum() or c in "_$"


def js_code_lines(text):
    """For each line of `text` (split on "\n"), whether it starts in plain
    code: not inside a string, template literal, comment or regex."""
    starts = [True]
    mode = "code"  # code, sq, dq, tpl, line, block, regex, class
    subs = []  # open ${...} in templates: the brace depth inside each
    last = ""  # the last code token: a punctuator, a word, or "x" for a value
    i = 0
    n = len(text)

    def skip_escape(i):
        # The escaped character may be a newline: a line that starts inside
        # the string or template.
        if i + 1 < n and text[i + 1] == "\n":
            starts.append(False)
        return i + 2

    while i < n:
        c = text[i]
        if c == "\n" and mode not in ("sq", "dq"):
            if mode == "line":
                mode = "code"
            starts.append(mode == "code")
            i += 1
            continue
        if mode == "code":
            if c in "'\"":
                mode = "sq" if c == "'" else "dq"
            elif c == "`":
                mode = "tpl"
            elif text.startswith("//", i):
                mode = "line"
                i += 1
            elif text.startswith("/*", i):
                mode = "block"
                i += 1
            elif c == "/":
                if last == "" or last in REGEX_AFTER_CHARS or last in REGEX_AFTER_WORDS:
                    mode = "regex"
                else:
                    last = "/"
            elif c == "{":
                if subs:
                    subs[-1] += 1
                last = c
            elif c == "}":
                if subs and subs[-1] == 0:
                    subs.pop()
                    mode = "tpl"
                else:
                    if subs:
                        subs[-1] -= 1
                    last = c
            elif is_word_char(c):
                j = i
                while j < n and is_word_char(text[j]):
                    j += 1
                word = text[i:j]
                last = word if word in REGEX_AFTER_WORDS else "x"
                i = j
                continue
            elif not c.isspace():
                last = "x" if c in ")]" else c
            i += 1
        elif mode in ("sq", "dq"):
            if c == "\\":
                i = skip_escape(i)
                continue
            if c == "\n":
                # An unterminated string: JavaScript ends it here anyway.
                mode = "code"
                starts.append(True)
            elif c == ("'" if mode == "sq" else '"'):
                mode = "code"
                last = "x"
            i += 1
        elif mode == "tpl":
            if c == "\\":
                i = skip_escape(i)
                continue
            if c == "`":
                mode = "code"
                last = "x"
            elif text.startswith("${", i):
                subs.append(0)
                mode = "code"
                last = "{"
                i += 1
            i += 1
        elif mode == "block":
            if text.startswith("*/", i):
                mode = "code"
                i += 1
            i += 1
        elif mode in ("regex", "class"):
            if c == "\\":
                i = skip_escape(i)
                continue
            if mode == "class" and c == "]":
                mode = "regex"
            elif mode == "regex" and c == "[":
                mode = "class"
            elif mode == "regex" and c == "/":
                mode = "code"
                last = "x"
            i += 1
        else:  # line comment
            i += 1
    return starts


def minify_js(text):
    lines = text.split("\n")
    out = []
    for line, code in zip(lines, js_code_lines(text)):
        if code:
            line = line.lstrip(" \t")
            if not line or line.startswith("//"):
                continue
        out.append(line)
    return "\n".join(out)


# A removed CSS comment between two of these would join the tokens on
# either side of it (".a/**/.b" is not ".a.b"), so there it stays, empty.
CSS_SEPARATORS = set(" \t\n{};:,>+~()")


def minify_css(text):
    out = []
    starts = [True]
    quote = ""
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1])
                if text[i + 1] == "\n":
                    starts.append(False)
                i += 2
                continue
            if c == quote or c == "\n":
                quote = ""
                if c == "\n":
                    starts.append(True)
            i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            before = out[-1] if out else "\n"
            after = text[end] if end < n else "\n"
            if before not in CSS_SEPARATORS and after not in CSS_SEPARATORS:
                out.append("/**/")
            i = end
        else:
            if c in "'\"":
                quote = c
            elif c == "\n":
                starts.append(True)
            out.append(c)
            i += 1
    lines = []
    for line, code in zip("".join(out).split("\n"), starts):
        if code:
            line = line.strip(" \t")
            if not line:
                continue
        lines.append(line)
    return "\n".join(lines)


def minify(path, data):
    """The bytes of `path` as they are stored: see the module docstring."""
    suffix = suffix_of(path)
    if suffix not in ("js", "mjs", "css"):
        return data
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return data
    text = minify_css(text) if suffix == "css" else minify_js(text)
    return text.encode("utf-8")


def encode(path, data):
    """Returns (mime, flags, stored_bytes) for one file."""
    suffix = suffix_of(path)
    mime = MIME_TYPES.get(suffix, DEFAULT_MIME)
    if suffix in GZIP_SUFFIXES:
        return mime, FLAG_GZIP, gzip.compress(minify(path, data), compresslevel=9, mtime=0)
    return mime, 0, data


def align(n):
    return (n + ALIGN - 1) & ~(ALIGN - 1)


def build_pack(files):
    """Returns (pack_bytes, gzip_total) for `files` ([(pack_path, bytes)])."""
    if len(files) > 0xFFFF:
        raise PackError(f"too many files ({len(files)}, max 65535)")
    encoded = []
    for path, data in files:
        path_bytes = path.encode("utf-8")
        if len(path_bytes) > 0xFFFF:
            raise PackError(f"{path}: path too long")
        mime, flags, stored = encode(path, data)
        encoded.append((path_bytes, mime.encode("ascii"), flags, stored))

    table_bytes = sum(2 + len(p) + 1 + len(m) + 1 + 4 + 4 + 32 for p, m, _, _ in encoded)
    offset = align(HEADER.size + table_bytes)
    table = bytearray()
    layout = []
    for path_bytes, mime_bytes, flags, stored in encoded:
        layout.append((offset, stored))
        table += struct.pack("<H", len(path_bytes)) + path_bytes
        table += struct.pack("<B", len(mime_bytes)) + mime_bytes
        table += struct.pack("<BII", flags, offset, len(stored))
        table += hashlib.sha256(stored).digest()
        offset = align(offset + len(stored))
    total = offset
    if total > 0xFFFFFFFF:
        raise PackError("pack larger than 4 GiB")

    out = bytearray(total)
    out[: HEADER.size] = HEADER.pack(MAGIC, VERSION, len(encoded), table_bytes, total)
    out[HEADER.size : HEADER.size + table_bytes] = table
    for off, stored in layout:
        out[off : off + len(stored)] = stored
    gzip_total = sum(len(s) for _, _, f, s in encoded if f & FLAG_GZIP)
    return bytes(out), gzip_total


def main(argv=None):
    parser = argparse.ArgumentParser(description="Pack web assets into a WispPack.")
    parser.add_argument("src_dir")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--budget-kb", type=int, help="max total size of gzipped files (KiB)")
    args = parser.parse_args(argv)

    src = pathlib.Path(args.src_dir)
    try:
        if not src.is_dir():
            raise PackError(f"{src}: not a directory")
        files = collect(src)
        check_imports(files)
        check_html_refs(files)
        blob, gzip_total = build_pack(files)
        if len(blob) > MAX_PACK_BYTES:
            raise PackError(
                f"pack is {len(blob)} bytes, over the {MAX_PACK_BYTES // 1024} KiB "
                "the application image keeps for it"
            )
        if args.budget_kb is not None and gzip_total > args.budget_kb * 1024:
            raise PackError(
                f"gzipped files total {gzip_total} bytes, over the {args.budget_kb} KiB budget"
            )
    except (PackError, OSError, ValueError) as exc:
        print(f"pack_assets: error: {exc}", file=sys.stderr)
        return 1

    out = pathlib.Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    tmp.write_bytes(blob)
    tmp.replace(out)
    print(f"pack_assets: {len(files)} files, {len(blob)} bytes "
          f"({gzip_total} gzipped) -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
