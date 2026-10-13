#!/usr/bin/env python3
"""Generates the breakout example's textures and its one sound.

Kept in the repository so the binary assets can be regenerated
deterministically instead of being opaque blobs, and so the build needs no
network and no image library: the PNGs are written with `struct` + `zlib` (as
`make_checker_asset.py` does) and the WAV with the standard-library `wave`
module.

Outputs (relative to the repository root):

    examples/breakout/assets/textures/paddle.png
    examples/breakout/assets/textures/ball.png
    examples/breakout/assets/textures/brick.png
    examples/breakout/assets/textures/wall.png
    examples/breakout/assets/audio/beep.wav

Run:  python3 tools/gen_breakout_assets.py
"""
import math
import struct
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "examples" / "breakout"


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
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None) for each scanline
        raw.extend(pixels[y * stride : (y + 1) * stride])
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    print(f"wrote {path} ({len(png)} bytes)")


def blank(width: int, height: int) -> bytearray:
    return bytearray(width * height * 4)


def put(buf: bytearray, width: int, x: int, y: int, rgba) -> None:
    i = (y * width + x) * 4
    buf[i : i + 4] = bytes(rgba)


def rounded_box(width: int, height: int, radius: int, fill, edge=None) -> bytes:
    """A rectangle with rounded corners, optional 1px edge colour."""
    buf = blank(width, height)
    edge = edge if edge is not None else fill
    for y in range(height):
        for x in range(width):
            # Distance outside the inset rectangle that defines the corner arcs.
            dx = max(radius - x, x - (width - 1 - radius), 0)
            dy = max(radius - y, y - (height - 1 - radius), 0)
            inside = dx * dx + dy * dy <= radius * radius
            if not inside:
                continue
            on_edge = x == 0 or y == 0 or x == width - 1 or y == height - 1
            on_edge = on_edge or (dx * dx + dy * dy > (radius - 1) * (radius - 1) and (dx or dy))
            put(buf, width, x, y, edge if on_edge else fill)
    return bytes(buf)


def disc(width: int, height: int, fill, edge) -> bytes:
    """An anti-aliased filled circle that touches the texture edges."""
    buf = blank(width, height)
    cx, cy = (width - 1) / 2.0, (height - 1) / 2.0
    radius = min(width, height) / 2.0
    inner = radius - 1.5  # where the edge ring starts
    for y in range(height):
        for x in range(width):
            d = math.hypot(x - cx, y - cy)
            if d <= inner:
                put(buf, width, x, y, fill)
            elif d <= radius:
                put(buf, width, x, y, edge)
    return bytes(buf)


def write_beep(path: Path) -> None:
    """A short 16-bit PCM mono beep with a linear attack/decay, so it has no
    clicks at either end. Deliberately tiny: it is a brick-break cue, not music."""
    sample_rate = 44100
    duration = 0.11
    freq = 880.0
    frames = int(sample_rate * duration)
    attack = int(sample_rate * 0.005)
    release = int(sample_rate * 0.05)
    data = bytearray()
    for n in range(frames):
        env = 1.0
        if n < attack:
            env = n / attack
        elif n > frames - release:
            env = max(0.0, (frames - n) / release)
        value = int(0.6 * env * 32767.0 * math.sin(2.0 * math.pi * freq * n / sample_rate))
        data += struct.pack("<h", value)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(bytes(data))
    print(f"wrote {path} ({len(data)} bytes of PCM)")


def main() -> None:
    write_png(
        ROOT / "assets" / "textures" / "paddle.png",
        96,
        28,
        rounded_box(96, 28, 8, (90, 210, 235, 255), (40, 150, 190, 255)),
    )
    write_png(
        ROOT / "assets" / "textures" / "ball.png",
        28,
        28,
        disc(28, 28, (245, 245, 250, 255), (150, 170, 210, 255)),
    )
    write_png(
        ROOT / "assets" / "textures" / "brick.png",
        56,
        28,
        rounded_box(56, 28, 5, (225, 120, 70, 255), (150, 60, 40, 255)),
    )
    # The walls are tinted at draw time, so a small flat texture is enough.
    write_png(
        ROOT / "assets" / "textures" / "wall.png",
        16,
        16,
        rounded_box(16, 16, 2, (235, 235, 245, 255), (170, 170, 190, 255)),
    )
    write_beep(ROOT / "assets" / "audio" / "beep.wav")


if __name__ == "__main__":
    main()
