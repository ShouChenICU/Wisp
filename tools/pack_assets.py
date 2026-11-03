#!/usr/bin/env python3
"""Packs a directory of web assets into a WispPack (spec 2.3), the read-only
format the `assets` partition holds and net::AssetPack
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

Checks, each failing with a non-zero exit and a message on stderr:
  - every relative/absolute module specifier in a .js/.mjs file
    (`import ... from '...'`, `export ... from '...'`, `import '...'`,
    `import('...')`) resolves to a file inside the pack;
  - every <script src> and <link href> in an .html file is a data: URL or
    resolves to a file inside the pack -- never another host, since a
    phone on the setup hotspot has no internet (spec 7.1);
  - --partitions: the pack fits the board's `assets` partition;
  - --budget-kb: the gzipped files together fit the budget.

Only the Python standard library is used.

Usage: pack_assets.py <src_dir> -o <out.wpak> [--partitions <csv>] [--budget-kb N]
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
HTML_REF_RE = re.compile(
    r"""<(?:script|link)\b[^>]*?\b(?:src|href)\s*=\s*(['"])([^'"]*)\1""", re.IGNORECASE
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
            ref = match.group(2).strip()
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


def encode(path, data):
    """Returns (mime, flags, stored_bytes) for one file."""
    suffix = suffix_of(path)
    mime = MIME_TYPES.get(suffix, DEFAULT_MIME)
    if suffix in GZIP_SUFFIXES:
        return mime, FLAG_GZIP, gzip.compress(data, compresslevel=9, mtime=0)
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


def parse_size(text):
    """Parses an ESP-IDF partition-table size: decimal, 0x hex, or with a
    K/M suffix (e.g. "1M", "64K")."""
    text = text.strip()
    mult = 1
    if text[-1:].upper() == "K":
        mult, text = 1024, text[:-1]
    elif text[-1:].upper() == "M":
        mult, text = 1024 * 1024, text[:-1]
    return int(text, 0) * mult


def assets_partition_size(csv_path):
    for raw in pathlib.Path(csv_path).read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if cols[0] == "assets":
            if len(cols) < 5 or not cols[4]:
                raise PackError(f"{csv_path}: assets partition has no size")
            return parse_size(cols[4])
    raise PackError(f"{csv_path}: no assets partition")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Pack web assets into a WispPack.")
    parser.add_argument("src_dir")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--partitions", help="board partitions.csv; pack must fit `assets`")
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
        if args.partitions:
            limit = assets_partition_size(args.partitions)
            if len(blob) > limit:
                raise PackError(
                    f"pack is {len(blob)} bytes, larger than the assets partition "
                    f"({limit} bytes) in {args.partitions}"
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
