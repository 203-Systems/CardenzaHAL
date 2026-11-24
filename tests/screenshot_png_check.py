#!/usr/bin/env python3
"""The screenshot PNG encoder (screenshot/png.h) against a reference decoder: Python's zlib and the PNG
specification's unfiltering, written here independently of the encoder.

    screenshot_png_check.py <screenshot_png_tool>

Runs the tool (it writes test frames as PNG and as raw RGB565), decodes every PNG and compares each pixel with
the RGB565 value widened as png.h says (r8 = r5 << 3 | r5 >> 2, ...). Also checks every chunk's CRC, the zlib
stream's Adler-32 (zlib does), that nothing follows IEND, and that the PNG is smaller than the raw pixels where it
should be (flat UI art)."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def chunks(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "signature"
    at = 8
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        crc = struct.unpack(">I", data[at + 8 + length:at + 12 + length])[0]
        assert zlib.crc32(kind + body) & 0xFFFFFFFF == crc, f"CRC of {kind}"
        yield kind, body
        at += 12 + length
        if kind == b"IEND":
            assert at == len(data), "bytes after IEND"
            return
    raise AssertionError("no IEND")


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode(data):
    width = height = depth = colour = None
    palette = None
    idat = b""
    for kind, body in chunks(data):
        if kind == b"IHDR":
            width, height, depth, colour, method, filt, interlace = struct.unpack(">IIBBBBB", body)
            assert (method, filt, interlace) == (0, 0, 0)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    if colour == 2:
        bpp, row_bytes = 3, width * 3
    else:
        assert colour == 3 and palette, "colour type"
        bpp, row_bytes = 1, (width * depth + 7) // 8
    assert len(raw) == height * (row_bytes + 1), "decompressed size"
    pixels = []
    prev = bytearray(row_bytes)
    for y in range(height):
        f = raw[y * (row_bytes + 1)]
        line = bytearray(raw[y * (row_bytes + 1) + 1:(y + 1) * (row_bytes + 1)])
        for i in range(row_bytes):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                line[i] = (line[i] + paeth(a, b, c)) & 255
            else:
                assert f == 0, f"filter {f}"
        prev = line
        if colour == 2:
            pixels += [tuple(line[x * 3:x * 3 + 3]) for x in range(width)]
        else:
            for x in range(width):
                bit = x * depth
                index = (line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)
                pixels.append(palette[index])
    return width, height, pixels


def widen(p):
    r, g, b = p >> 11, (p >> 5) & 63, p & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def main():
    tool = sys.argv[1]
    with tempfile.TemporaryDirectory() as folder:
        out = subprocess.run([tool, folder], capture_output=True, text=True)
        print(out.stdout, end="")
        if out.returncode != 0:
            print(out.stderr, end="")
            sys.exit("the tool failed")
        checked = 0
        for png in sorted(pathlib.Path(folder).glob("*.png")):
            raw = png.with_suffix(".rgb565").read_bytes()
            expected = [widen(v) for (v,) in struct.iter_unpack("<H", raw)]
            data = png.read_bytes()
            width, height, pixels = decode(data)
            assert width * height == len(expected), f"{png.name}: size"
            assert pixels == expected, f"{png.name}: pixels differ"
            if png.stem in ("black", "white", "two", "four", "ui"):
                assert len(data) < 3000, f"{png.name}: {len(data)} bytes for flat art"
            checked += 1
        print(f"{checked} PNGs decode to their pixels")


if __name__ == "__main__":
    main()
