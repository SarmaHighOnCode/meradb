#!/usr/bin/env python3
"""cpp/tests/gui_screenshot.py -- meradb_gui draws its window with a hidden window and writes a PNG.

usage: gui_screenshot.py PATH/TO/meradb_gui

For each theme: runs `meradb_gui --local --screenshot out.png --theme T --size 960x600 --script "SHURU;"` with a
temporary data folder and checks that the PNG exists, has the requested size, is not one flat colour, and that the
two themes look the part (a light window is bright, a dark one dark) and differ from each other.

Exit code 77 (ctest: skipped) when meradb_gui cannot open a window: no display, or no OpenGL 3 (exit code 3).
Never touches the real per-user data folder.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

WIDTH, HEIGHT = 960, 600
NO_DISPLAY = 3


def read_png(path):
    """Decodes an 8-bit, non-interlaced RGB or RGBA PNG: (width, height, channels, rows of bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos = 8
    header = None
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ValueError("no IHDR chunk")
    width, height, depth, color, _compression, _filter, interlace = header
    if depth != 8 or color not in (2, 6) or interlace != 0:
        raise ValueError("unsupported PNG format %r" % (header,))
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 255
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                predictor = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + predictor) & 255
        elif kind != 0:
            raise ValueError("bad PNG filter %d" % kind)
        rows.append(line)
        previous = line
    return width, height, channels, rows


def analyse(path):
    width, height, channels, rows = read_png(path)
    colours = set()
    total = 0
    for y in range(0, height, 2):
        row = rows[y]
        for x in range(0, width, 2):
            i = x * channels
            colours.add((row[i], row[i + 1], row[i + 2]))
            total += row[i] + row[i + 1] + row[i + 2]
    samples = len(range(0, height, 2)) * len(range(0, width, 2))
    brightness = total / (3.0 * samples)
    return width, height, len(colours), brightness


def region_mean(path, x0, y0, x1, y1):
    """Mean brightness (0..255) of the pixel rectangle [x0,x1) x [y0,y1) of the PNG (rows top to bottom)."""
    _width, _height, channels, rows = read_png(path)
    total = count = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = x * channels
            total += rows[y][i] + rows[y][i + 1] + rows[y][i + 2]
            count += 3
    return total / float(count)


def check_upright(path, theme):
    """The logo mark sits in the top bar's left corner (x 16..36, y 14..34): a vertically flipped picture has the
    mark at the bottom instead. The mark is drawn in the text colour, so it is bright on dark and dark on light."""
    mark = region_mean(path, 18, 16, 34, 32)
    mirrored = region_mean(path, 18, HEIGHT - 32, 34, HEIGHT - 16)
    if theme == "dark":
        return mark > 150 and mirrored < 60
    return mark < 100 and mirrored > 200


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    work = tempfile.mkdtemp(prefix="meradb_gui_shot_")
    failures = []
    results = {}
    try:
        env = dict(os.environ)
        for name in ("MERADB_HOST", "MERADB_PORT", "MERADB_PASSWORD", "MERADB_USER"):
            env.pop(name, None)
        for theme in ("dark", "light"):
            data = os.path.join(work, "data_" + theme)
            os.makedirs(data)
            env["MERADB_DATA"] = data
            png = os.path.join(work, "out_" + theme + ".png")
            command = [exe, "--local", "-D", data, "--screenshot", png, "--theme", theme,
                       "--size", "%dx%d" % (WIDTH, HEIGHT), "--script", "SHURU;"]
            try:
                done = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
            except subprocess.TimeoutExpired:
                failures.append("%s: meradb_gui did not finish within 120 seconds" % theme)
                continue
            if done.returncode == NO_DISPLAY:
                print("SKIP: no display or no OpenGL 3 here: " + done.stderr.decode("utf-8", "replace").strip())
                return 77
            if done.returncode != 0:
                failures.append("%s: exit code %d: %s" % (theme, done.returncode, done.stderr.decode("utf-8", "replace")))
                continue
            if not os.path.isfile(png):
                failures.append("%s: no PNG was written" % theme)
                continue
            try:
                width, height, colours, brightness = analyse(png)
            except Exception as error:  # noqa: BLE001 -- any decoding problem is a failure of this check
                failures.append("%s: unreadable PNG: %s" % (theme, error))
                continue
            results[theme] = brightness
            print("%s: %dx%d, %d distinct colours, mean brightness %.1f" % (theme, width, height, colours, brightness))
            if (width, height) != (WIDTH, HEIGHT):
                failures.append("%s: size is %dx%d, expected %dx%d" % (theme, width, height, WIDTH, HEIGHT))
            if not check_upright(png, theme):
                failures.append("%s: the picture is not upright (logo mark not in the top-left corner)" % theme)
            if colours < 8:
                failures.append("%s: the picture is (nearly) one flat colour (%d colours)" % (theme, colours))
            if theme == "dark" and brightness > 60:
                failures.append("dark: too bright (%.1f)" % brightness)
            if theme == "light" and brightness < 200:
                failures.append("light: too dark (%.1f)" % brightness)
        if len(results) == 2 and abs(results["light"] - results["dark"]) < 100:
            failures.append("the two themes look alike")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for failure in failures:
        print("FAIL: " + failure)
    if failures:
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
