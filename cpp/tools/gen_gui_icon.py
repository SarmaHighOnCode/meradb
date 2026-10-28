#!/usr/bin/env python3
"""cpp/tools/gen_gui_icon.py -- draws the MeraDB Workbench icon and writes cpp/resources/meradb_gui.ico.

usage: gen_gui_icon.py [OUTPUT.ico]

The mark is the one in the window's top bar: a light rounded tile with a dark "M" (monochrome). The .ico holds 16, 32
and 48 pixel images as uncompressed 32-bit bitmaps (the program also uses these for the window icon) and a 256 pixel
PNG. Only the Python standard library is used; the result is checked in, so the build does not run this script.
"""
import os
import struct
import sys
import zlib

TILE = (237, 237, 237)    # #EDEDED
MARK = (10, 10, 10)       # #0A0A0A
M_POINTS = [(5, 15), (5, 5), (10, 11), (15, 5), (15, 15)]   # in a 20 x 20 box, like the top bar's mark
STROKE = 2.0              # 20-unit box
RADIUS = 5.0
SAMPLES = 4               # per axis


def distance_to_segment(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * dx, ay + t * dy
    return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5


def inside_tile(x, y):
    """x, y in the 20-unit box; a rounded square covering the whole box."""
    cx = min(max(x, RADIUS), 20 - RADIUS)
    cy = min(max(y, RADIUS), 20 - RADIUS)
    return (x - cx) ** 2 + (y - cy) ** 2 <= RADIUS * RADIUS


def on_mark(x, y):
    return any(distance_to_segment(x, y, M_POINTS[i], M_POINTS[i + 1]) <= STROKE / 2 for i in range(len(M_POINTS) - 1))


def render(size):
    """RGBA rows, top to bottom."""
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            tile = mark = 0
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    x = (px + (sx + 0.5) / SAMPLES) * 20.0 / size
                    y = (py + (sy + 0.5) / SAMPLES) * 20.0 / size
                    if inside_tile(x, y):
                        tile += 1
                        if on_mark(x, y):
                            mark += 1
            total = SAMPLES * SAMPLES
            if tile == 0:
                row.append((0, 0, 0, 0))
                continue
            m = mark / float(tile)
            r = round(TILE[0] * (1 - m) + MARK[0] * m)
            g = round(TILE[1] * (1 - m) + MARK[1] * m)
            b = round(TILE[2] * (1 - m) + MARK[2] * m)
            row.append((r, g, b, round(255 * tile / total)))
        rows.append(row)
    return rows


def bmp_entry(size, rows):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b""
    for row in reversed(rows):   # bottom-up
        pixels += b"".join(struct.pack("<BBBB", b, g, r, a) for (r, g, b, a) in row)
    mask_stride = ((size + 31) // 32) * 4
    return header + pixels + b"\x00" * (mask_stride * size)


def png_entry(size, rows):
    raw = b"".join(b"\x00" + b"".join(struct.pack("<BBBB", *p) for p in row) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "meradb_gui.ico")
    images = []
    for size in (16, 32, 48, 256):
        rows = render(size)
        images.append((size, png_entry(size, rows) if size == 256 else bmp_entry(size, rows)))
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, blob in images:
        data += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for _size, blob in images:
        data += blob
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (out, len(data)))


if __name__ == "__main__":
    main()
