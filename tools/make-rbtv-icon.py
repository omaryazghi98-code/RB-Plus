#!/usr/bin/env python3
"""Generate a small RBTV+ launcher icon using only the Python standard library."""
from __future__ import annotations

import math
import struct
import sys
import zlib
from pathlib import Path

SIZE = 512
SCALE = 2
WORK = SIZE * SCALE
ORANGE = (255, 178, 0, 255)
NAVY = (20, 32, 61, 255)
TRANSPARENT_ORANGE = (255, 178, 0, 0)
GLYPHS = (
    ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),  # R
    ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),  # B
)


def chunk(kind: bytes, payload: bytes) -> bytes:
    data = kind + payload
    return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)


def rounded_icon() -> bytearray:
    pixels = bytearray(WORK * WORK * 4)
    radius = 112
    for y in range(WORK):
        if y < radius:
            dy = radius - y
            inset = radius - math.sqrt(max(0, radius * radius - dy * dy))
        elif y >= WORK - radius:
            dy = y - (WORK - radius - 1)
            inset = radius - math.sqrt(max(0, radius * radius - dy * dy))
        else:
            inset = 0
        left = max(0, min(WORK, int(math.ceil(inset))))
        right = max(left, min(WORK, int(math.floor(WORK - inset))))
        row = bytearray(TRANSPARENT_ORANGE) * WORK
        row[left:right] = bytearray(ORANGE) * (right - left)
        at = y * WORK * 4
        pixels[at:at + WORK * 4] = row
    return pixels


def draw_glyph(pixels: bytearray, pattern: tuple[str, ...], left: int, top: int) -> None:
    # A connected 5x7 block mark stays legible at launcher-icon size.
    cell = 78
    gap = 0
    for row_index, row in enumerate(pattern):
        for col_index, bit in enumerate(row):
            if bit != "1":
                continue
            x0 = left + col_index * (cell + gap)
            y0 = top + row_index * (cell + gap)
            for y in range(y0, y0 + cell):
                start = (y * WORK + x0) * 4
                end = (y * WORK + x0 + cell) * 4
                pixels[start:end] = bytearray(NAVY) * cell


def make_icon() -> bytes:
    pixels = rounded_icon()
    cell = 78
    glyph_w = 5 * cell
    glyph_h = 7 * cell
    gap = 18
    left = (WORK - (glyph_w * 2 + gap)) // 2
    top = (WORK - glyph_h) // 2
    draw_glyph(pixels, GLYPHS[0], left, top)
    draw_glyph(pixels, GLYPHS[1], left + glyph_w + gap, top)

    # Box-filter 2x2 samples to antialias the rounded outer corners.
    output = bytearray()
    for y in range(SIZE):
        output.append(0)  # PNG filter: None
        row0 = ((y * SCALE) * WORK) * 4
        row1 = row0 + WORK * 4
        for x in range(SIZE):
            i0 = row0 + x * SCALE * 4
            i1 = i0 + 4
            j0 = row1 + x * SCALE * 4
            j1 = j0 + 4
            for channel in range(4):
                output.append((pixels[i0 + channel] + pixels[i1 + channel] +
                               pixels[j0 + channel] + pixels[j1 + channel] + 2) // 4)
    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(output), 9)) + chunk(b"IEND", b"")


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: make-rbtv-icon.py OUTPUT.png", file=sys.stderr)
        return 2
    destination = Path(sys.argv[1])
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(make_icon())
    print(f"Generated RBTV+ launcher icon: {destination} ({SIZE}x{SIZE} RGBA)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
