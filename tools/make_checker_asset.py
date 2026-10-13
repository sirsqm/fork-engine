#!/usr/bin/env python3
"""Generates the example's checkerboard texture.

Kept in the repository so the binary asset can be regenerated deterministically
instead of being an opaque blob. Produces a 64x64 RGBA PNG with a two-tone
checker pattern, written to assets/textures/checker.png.
"""
import struct
import zlib
from pathlib import Path

SIZE = 64
CELL = 8
COLOR_A = (232, 232, 240, 255)
COLOR_B = (70, 90, 160, 255)
COLOR_C = (200, 90, 70, 255)


def pixel(x: int, y: int) -> tuple:
    cx, cy = x // CELL, y // CELL
    if (cx + cy) % 2 == 0:
        return COLOR_A
    # Mark the origin cell so the sprite's orientation is visible on screen.
    if cx == 0 and cy == 0:
        return COLOR_C
    return COLOR_B


def build_raw() -> bytes:
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)  # filter type 0 (None) for each scanline
        for x in range(SIZE):
            raw.extend(pixel(x, y))
    return bytes(raw)


def chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def main() -> None:
    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)  # 8-bit RGBA
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(build_raw(), 9))
    png += chunk(b"IEND", b"")

    out = Path(__file__).resolve().parent.parent / "assets" / "textures" / "checker.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(png)
    print(f"wrote {out} ({len(png)} bytes)")


if __name__ == "__main__":
    main()
