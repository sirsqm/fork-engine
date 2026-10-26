#!/usr/bin/env python3
"""Generates the engine's built-in bitmap font.

Kept in the repository so the atlas and its metrics can be regenerated
deterministically instead of being opaque blobs, and so the build needs no font
file, no rasterizer, and no network: the PNG is written with `struct` + `zlib`
(as `gen_breakout_assets.py` and `make_checker_asset.py` do) and the metrics as
plain JSON with the standard library.

The glyphs are a compact 5x7 pixel font, which is the classic legible size for a
HUD and is small enough to be written out by hand and reviewed. A real font
file and a rasterizer (stb_truetype) are the follow-on if crisp large-size text
is ever needed; this gets a legible score on screen today (ADR-0083).

The metrics are in em units: the glyph box is 1.0 em tall and 1.0 em is
`kEmPixels` atlas pixels, so a label's `size` is the single scale factor.

Outputs (relative to the repository root):

    examples/breakout/assets/fonts/hud.png      the glyph atlas
    examples/breakout/assets/fonts/hud.fefont   the metrics

Run:  python3 tools/gen_font_atlas.py
"""
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "examples" / "breakout" / "assets" / "fonts"

# The atlas is a grid of equal cells. 16 columns x 6 rows covers the printable
# ASCII range (0x20..0x7E) with room to spare, and keeps the atlas a power of
# two per axis so it uploads cleanly.
COLUMNS = 16
ROWS = 6
FIRST_CODE = 0x20
LAST_CODE = 0x7E

# One cell is GLYPH_W x GLYPH_H pixels, with a one-pixel margin so a linear
# filter never samples a neighbouring glyph at the edges.
GLYPH_W = 6
GLYPH_H = 8
CELL_W = GLYPH_W + 2
CELL_H = GLYPH_H + 2

# One em in atlas pixels. The glyph box is 7 px tall (GLYPH_H - 1 margin row),
# so this makes the em match the drawn height and keeps the metrics readable:
# an advance of 6 px is 0.75 em.
EM_PIXELS = 8.0

# A 5x7 font. Each entry is seven rows of five characters, '#' for a lit pixel
# and '.' for an unlit one. Only the printable ASCII range is defined; a byte
# with no entry draws nothing (the component treats a missing glyph as a gap).
GLYPHS = {
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "!": [".#...", ".#...", ".#...", ".#...", ".....", ".#...", "....."],
    '"': [".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."],
    "#": [".#.#.", "#####", ".#.#.", ".#.#.", "#####", ".#.#.", "....."],
    "$": ["..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."],
    "%": ["##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"],
    "&": [".##..", "#..#.", ".##..", "#.#.#", "#..#.", ".##.#", "....."],
    "'": [".#...", ".#...", ".....", ".....", ".....", ".....", "....."],
    "(": ["..#..", ".#...", ".#...", ".#...", ".#...", ".#...", "..#.."],
    ")": ["#....", ".#...", ".#...", ".#...", ".#...", ".#...", "#...."],
    "*": [".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    ",": [".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    "/": ["....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
    ";": [".....", ".##..", ".##..", ".....", ".##..", ".##..", ".#..."],
    "<": ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."],
    "=": [".....", ".....", "#####", ".....", "#####", ".....", "....."],
    ">": [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "@": [".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".###."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "[": [".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."],
    "\\": ["#....", ".#...", ".#...", "..#..", "...#.", "...#.", "....#"],
    "]": [".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."],
    "^": ["..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."],
    "_": [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
    "`": [".#...", "..#..", ".....", ".....", ".....", ".....", "....."],
    "{": ["..##.", ".#...", ".#...", "##...", ".#...", ".#...", "..##."],
    "|": ["..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "}": [".##..", "...#.", "...#.", "...##", "...#.", "...#.", ".##.."],
    "~": [".....", ".....", ".##.#", "#..#.", ".....", ".....", "....."],
}

# The advance for every glyph, in atlas pixels. A monospaced font is the right
# first cut: it makes the layout trivially correct and the metrics reviewable,
# and a proportional font is a metrics change, not a component change.
ADVANCE_PIXELS = 6.0


def chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def write_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    """Writes an 8-bit RGBA PNG. `pixels` is width*height*4 bytes, row-major."""
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    # One filter byte (0 = none) per scanline.
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(pixels[y * width * 4 : (y + 1) * width * 4])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    atlas_w = COLUMNS * CELL_W
    atlas_h = ROWS * CELL_H
    pixels = bytearray(atlas_w * atlas_h * 4)  # transparent by default

    glyphs = [None] * (LAST_CODE + 1)
    for code in range(FIRST_CODE, LAST_CODE + 1):
        char = chr(code)
        rows = GLYPHS.get(char)
        if rows is None:
            # No entry: the byte draws nothing. Recording it as null keeps the
            # array dense and indexed by byte, so a lookup is a bounds check.
            continue

        index = code - FIRST_CODE
        col = index % COLUMNS
        row = index // COLUMNS
        origin_x = col * CELL_W + 1  # +1 for the margin
        origin_y = row * CELL_H + 1

        for gy, line in enumerate(rows):
            for gx, mark in enumerate(line):
                if mark != "#":
                    continue
                px = origin_x + gx
                py = origin_y + gy
                offset = (py * atlas_w + px) * 4
                pixels[offset : offset + 4] = b"\xff\xff\xff\xff"

        # A glyph with no ink (a space) is recorded with a zero-area cell, which
        # the component reads as "advance but draw nothing". That keeps a blank
        # from costing a transparent quad and makes "one instance per inked
        # glyph" exactly true.
        inked = any(mark == "#" for line in rows for mark in line)
        u0 = origin_x / atlas_w if inked else 0.0
        v0 = origin_y / atlas_h if inked else 0.0
        u1 = (origin_x + GLYPH_W) / atlas_w if inked else 0.0
        v1 = (origin_y + GLYPH_H) / atlas_h if inked else 0.0
        glyphs[code] = {
            "u0": u0,
            "v0": v0,
            "u1": u1,
            "v1": v1,
            "advance": ADVANCE_PIXELS / EM_PIXELS,
            "bearing_x": 0.0,
            # The glyph box sits on the baseline: its top is one em above the
            # origin, so the bearing is (em height - glyph height) / em.
            "bearing_y": (EM_PIXELS - GLYPH_H) / EM_PIXELS,
            "width": GLYPH_W / EM_PIXELS,
            "height": GLYPH_H / EM_PIXELS,
        }

    metrics = {
        # The name of the atlas this font draws from, relative to the repository
        # root, exactly as the game loads it. The FontLoader resolves it through
        # the texture scheme.
        "atlas": "examples/breakout/assets/fonts/hud.png",
        "line_height": (GLYPH_H + 2) / EM_PIXELS,
        "glyphs": glyphs,
    }

    write_png(ROOT / "hud.png", atlas_w, atlas_h, bytes(pixels))
    (ROOT / "hud.fefont").write_text(json.dumps(metrics, indent=1, sort_keys=False) + "\n")
    print(f"wrote {ROOT / 'hud.png'} ({atlas_w}x{atlas_h}) and {ROOT / 'hud.fefont'}")


if __name__ == "__main__":
    main()
