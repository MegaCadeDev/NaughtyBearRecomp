#!/usr/bin/env python3
"""Convert a binary PPM (as written by NB_SCREENSHOT) to PNG using only the standard library."""
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
width, height = map(int, parts[1].split())
pixels = parts[3]
rows = b"".join(b"\x00" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))

def chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
