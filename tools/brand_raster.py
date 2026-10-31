#!/usr/bin/env python3
"""Rasterises the Fork Engine mark from its SVG, with no image library.

The branding SVGs (`branding/forkengine-*.svg`) draw the mark as filled
polygons: the only path commands are absolute move, line and close. A scanline
fill with the nonzero winding rule therefore reproduces them exactly - no curve
flattening, no anti-aliasing heuristic, and nothing beyond the standard library.
That matters because the project generates its binary assets rather than
checking in hand-drawn blobs (see `tools/gen_breakout_assets.py`), so the SVG is
the one source of truth and every binary form is a render of it.

Both generated assets come from here: the AppImage icon (a 256x256 PNG) and the
engine's window-icon header (a 64x64 coverage mask). Keeping the rasteriser in
one module means the two cannot drift into different interpretations of the same
mark.

This is a helper for `tools/gen_appimage_icon.py` and `tools/gen_engine_icon.py`,
not a standalone tool.
"""
import re
import zlib
import struct


class SvgError(ValueError):
    """The SVG uses a feature this rasteriser does not implement."""


def parse_mark(svg_text):
    """Parses the filled paths of an SVG into (view_width, view_height, paths).

    Each path is `(points, transform)`: `points` is a list of (x, y) in the
    path's own coordinates, and `transform` is (translate_x, translate_y, scale)
    from the enclosing group. Only the transform forms the branding files use are
    accepted; anything else raises, so a future SVG cannot be silently mis-placed.
    """
    view = re.search(r'viewBox="([^"]+)"', svg_text)
    if view is None:
        raise SvgError("the SVG has no viewBox")
    numbers = [float(v) for v in view.group(1).replace(",", " ").split()]
    if len(numbers) != 4:
        raise SvgError("the viewBox is not 'min-x min-y width height'")
    view_w, view_h = numbers[2], numbers[3]

    paths = []
    for group in re.finditer(r"<g\b([^>]*)>(.*?)</g>", svg_text, re.S):
        transform = _parse_transform(group.group(1))
        for path in re.finditer(r'<path\b[^>]*\bd="([^"]+)"', group.group(2)):
            paths.append((_parse_path(path.group(1)), transform))
    if not paths:
        raise SvgError("the SVG has no filled paths")
    return view_w, view_h, paths


def _parse_transform(attributes):
    """Returns (translate_x, translate_y, scale) from a group's transform."""
    tx = ty = 0.0
    scale = 1.0
    match = re.search(r'transform="([^"]+)"', attributes)
    if match is None:
        return tx, ty, scale
    body = match.group(1)
    for name, args in re.findall(r"(\w+)\(([^)]*)\)", body):
        values = [float(v) for v in args.replace(",", " ").split()]
        if name == "translate":
            tx, ty = values[0], values[1] if len(values) > 1 else 0.0
        elif name == "scale":
            scale = values[0]
        else:
            raise SvgError(f"unsupported transform '{name}'")
    return tx, ty, scale


def _parse_path(d):
    """Parses an absolute M/L/Z path into a list of (x, y) vertices.

    The branding files use only these commands. Any other (a curve, a relative
    move, an arc) raises rather than being approximated: a mark rendered from a
    guessed geometry would be wrong in a way no test would catch.
    """
    tokens = re.findall(r"[A-Za-z]|-?\d*\.?\d+(?:[eE][-+]?\d+)?", d)
    points = []
    command = None
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token.isalpha():
            command = token
            i += 1
            if command in ("Z", "z"):
                continue
            if command not in ("M", "L"):
                raise SvgError(f"unsupported path command '{command}'")
            continue
        if command is None:
            raise SvgError("a path begins with coordinates and no command")
        points.append((float(tokens[i]), float(tokens[i + 1])))
        i += 2
    return points


def rasterize(svg_text, width, height, margin=0.0):
    """Renders the mark to a `width`x`height` coverage mask (one byte per pixel).

    The mark is fitted into the target box preserving its aspect ratio and
    centred, which is what an app icon wants. `margin` is the fraction of the
    box left empty on each side, so an icon does not run the mark to the very
    edge. Coverage is 0 or 255: the source is polygons, so a pixel is inside or
    outside, and the shapes are large enough at the sizes generated here that the
    hard edge reads cleanly. A pixel is inside when the nonzero winding number of
    the paths is not zero.
    """
    view_w, view_h, paths = parse_mark(svg_text)
    inner_w = width * (1.0 - 2.0 * margin)
    inner_h = height * (1.0 - 2.0 * margin)
    scale = min(inner_w / view_w, inner_h / view_h)
    offset_x = (width - view_w * scale) / 2.0
    offset_y = (height - view_h * scale) / 2.0

    polygons = []
    for points, (tx, ty, group_scale) in paths:
        polygons.append(
            [
                (
                    (x * group_scale + tx) * scale + offset_x,
                    (y * group_scale + ty) * scale + offset_y,
                )
                for (x, y) in points
            ]
        )

    mask = bytearray(width * height)
    for polygon in polygons:
        if len(polygon) < 3:
            continue
        ys = [p[1] for p in polygon]
        first_row = max(0, int(min(ys)))
        last_row = min(height, int(max(ys)) + 2)
        for row in range(first_row, last_row):
            y = row + 0.5
            crossings = []
            count = len(polygon)
            for k in range(count):
                x1, y1 = polygon[k]
                x2, y2 = polygon[(k + 1) % count]
                if (y1 <= y) != (y2 <= y):
                    t = (y - y1) / (y2 - y1)
                    crossings.append((x1 + t * (x2 - x1), 1 if y2 > y1 else -1))
            crossings.sort()
            winding = 0
            span_start = 0.0
            for x, direction in crossings:
                was_inside = winding != 0
                winding += direction
                inside = winding != 0
                if not was_inside and inside:
                    span_start = x
                elif was_inside and not inside:
                    _fill_span(mask, width, row, span_start, x)
    return mask


def _fill_span(mask, width, row, x0, x1):
    """Sets the pixels whose centres lie in [x0, x1) on `row`."""
    start = max(0, int(x0 + 0.5))
    end = min(width, int(x1 + 0.5) + 1)
    base = row * width
    for x in range(start, end):
        if x0 <= x + 0.5 < x1:
            mask[base + x] = 255


def mask_to_rgba(mask, width, height, colour):
    """Expands a coverage mask to RGBA, tinting the covered pixels `colour`."""
    r, g, b = colour
    pixels = bytearray(width * height * 4)
    for i, coverage in enumerate(mask):
        if coverage:
            o = i * 4
            pixels[o] = r
            pixels[o + 1] = g
            pixels[o + 2] = b
            pixels[o + 3] = coverage
    return pixels


def write_png(path, width, height, pixels):
    """Writes an 8-bit RGBA PNG. `pixels` is width*height*4 bytes, row-major."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type 0 (None) for each scanline
        raw.extend(pixels[y * stride : (y + 1) * stride])
    png = b"\x89PNG\r\n\x1a\n"
    png += _png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += _png_chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += _png_chunk(b"IEND", b"")
    path.write_bytes(png)
    return len(png)


def _png_chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )
