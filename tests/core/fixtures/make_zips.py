#!/usr/bin/env python3
"""Builds the zip archives tests/core/zip_test.cpp reads.

Usage: make_zips.py <output dir>

ok.zip and the entry-count archive come from the standard library's
zipfile, so the reader is checked against an independent writer. The
hostile archives need fields zipfile refuses to produce (invalid UTF-8
names, symlink modes, encryption flags, wrong CRCs), so they are laid out
by hand with build_zip() below. Each hostile archive holds exactly one bad
entry; zip_test.cpp knows the expected error code and the path the message
must name.
"""

import os
import struct
import sys
import zipfile
import zlib

S_IFREG = 0o100644
S_IFLNK = 0o120777


def deflate_raw(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def build_zip(entries):
    """entries: dicts with name (bytes), data (bytes) and optional method,
    flags, mode, crc, size (declared uncompressed size) overrides."""
    body = bytearray()
    central = bytearray()
    for e in entries:
        name = e["name"]
        data = e["data"]
        method = e.get("method", 0)
        stored = deflate_raw(data) if method == 8 else data
        crc = e.get("crc", zlib.crc32(data))
        size = e.get("size", len(data))
        flags = e.get("flags", 0)
        mode = e.get("mode", S_IFREG)
        offset = len(body)
        body += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, 0, 0x21, crc,
                            len(stored), size, len(name), 0)
        body += name + stored
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, (3 << 8) | 20, 20, flags,
                               method, 0, 0x21, crc, len(stored), size, len(name), 0, 0, 0,
                               0, mode << 16, offset)
        central += name
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(central),
                       len(body), 0)
    return bytes(body + central + eocd)


def write(out_dir, name, blob):
    with open(os.path.join(out_dir, name), "wb") as f:
        f.write(blob)


def make_ok(path):
    text = b"".join(b"line %d of the scene source\n" % i for i in range(400))
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(zipfile.ZipInfo("assets/"), b"")
        z.writestr("manifest.json", b'{"id":"demo"}', compress_type=zipfile.ZIP_STORED)
        z.writestr("main.lua", text, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("assets/empty.txt", b"", compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("场景/说明.txt", "你好".encode(),
                   compress_type=zipfile.ZIP_DEFLATED)


# (file name, entry name bytes, overrides)
UNSAFE = [
    ("dotdot.zip", b"../x", {}),
    ("abs.zip", b"/abs", {}),
    ("backslash.zip", b"a\\b", {}),
    ("dot.zip", b"a/./b", {}),
    ("empty_seg.zip", b"a//b", {}),
    ("drive.zip", b"C:x", {}),
    ("symlink.zip", b"link", {"mode": S_IFLNK, "data": b"/etc/passwd"}),
    ("encrypted.zip", b"secret.txt", {"flags": 1}),
    ("method12.zip", b"bz.txt", {"method": 12}),
    ("long.zip", b"p" * 129, {}),
    ("bad_utf8.zip", b"bad\xff\xfe.txt", {}),
]


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    for file_name, entry_name, overrides in UNSAFE:
        entry = {"name": entry_name, "data": b"payload"}
        entry.update(overrides)
        write(out_dir, file_name, build_zip([entry]))
    write(out_dir, "duplicate.zip",
          build_zip([{"name": b"dup.txt", "data": b"one"}, {"name": b"dup.txt", "data": b"two"}]))

    # Real, valid deflate data whose declared sizes sum to one byte over the
    # 512 KB default; it compresses to a few hundred bytes.
    write(out_dir, "oversize.zip",
          build_zip([{"name": b"a.bin", "data": bytes(256 * 1024), "method": 8},
                     {"name": b"b.bin", "data": bytes(256 * 1024 + 1), "method": 8}]))

    with zipfile.ZipFile(os.path.join(out_dir, "many.zip"), "w") as z:
        for i in range(257):
            z.writestr("f%03d.txt" % i, b"x")

    write(out_dir, "bad_crc.zip",
          build_zip([{"name": b"stored.txt", "data": b"stored body", "crc": 0x12345678},
                     {"name": b"deflated.txt", "data": b"deflated body " * 20, "method": 8,
                      "crc": 0x12345678}]))

    # Last: the build uses ok.zip as the output stamp, so it only exists
    # once every other archive has been written.
    make_ok(os.path.join(out_dir, "ok.zip"))


if __name__ == "__main__":
    main()
