#!/usr/bin/env python3
"""Packs a TrueType font into the image the fonts partition holds
(vector-font spec 3.2): a 32-byte header followed by the TTF unchanged.
Everything is little-endian:

  offset 0   4 bytes  magic b"WFNT"
  offset 4   u16      format version, 1
  offset 6   u16      header length, 32 (the TTF starts here)
  offset 8   u32      TTF length in bytes
  offset 12  u32      CRC-32 of the TTF (zlib.crc32, same as wisp::crc32)
  offset 16  16 bytes reserved, zero

The firmware's open_font_image() checks this header before the TTF ever
reaches stb_truetype, which does no bounds checking of its own. The output
depends only on the input bytes, so images are byte-identical across builds.

Fails with a non-zero exit and a message on stderr when the input is not a
glyf (TrueType) font -- CFF outlines (sfnt tag "OTTO") are rejected, since
Tiny TTF only rasterises glyf -- or when the image is larger than
--max-bytes (the fonts partition's size).

Usage: pack_font_image.py <ttf> -o <out> [--max-bytes N]
"""

import argparse
import pathlib
import struct
import sys
import zlib

MAGIC = b"WFNT"
VERSION = 1
HEADER = struct.Struct("<4sHHII16x")
TRUETYPE_TAG = b"\x00\x01\x00\x00"
CFF_TAG = b"OTTO"


class PackError(Exception):
    pass


def pack(ttf_bytes):
    """Returns the fonts partition image for `ttf_bytes`; raises PackError
    when it is not a TrueType (glyf) font."""
    tag = ttf_bytes[:4]
    if tag == CFF_TAG:
        raise PackError("CFF fonts are not supported; convert to TrueType")
    if tag != TRUETYPE_TAG:
        raise PackError(f"not a TrueType font (sfnt version {tag.hex()}, expected 00010000)")
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, len(ttf_bytes), zlib.crc32(ttf_bytes))
    return header + ttf_bytes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ttf", type=pathlib.Path)
    parser.add_argument("-o", "--output", type=pathlib.Path, required=True)
    # Decimal or 0x-prefixed: partition_table_get_partition_info() reports hex.
    parser.add_argument("--max-bytes", type=lambda s: int(s, 0), default=None)
    args = parser.parse_args(argv)

    try:
        image = pack(args.ttf.read_bytes())
        if args.max_bytes is not None and len(image) > args.max_bytes:
            raise PackError(f"font image is {len(image)} bytes, the partition holds {args.max_bytes}")
    except PackError as e:
        print(f"pack_font_image: {e}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    tmp = args.output.with_name(args.output.name + ".tmp")
    tmp.write_bytes(image)
    tmp.replace(args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
