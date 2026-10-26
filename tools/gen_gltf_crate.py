#!/usr/bin/env python3
"""Generates the 3D game's imported crate: a glTF, its buffer, and its texture.

Kept in the repository so the binary assets can be regenerated deterministically
instead of being opaque blobs, and so the shipping path can be exercised with no
model exporter and no image library: the `.gltf` is JSON, the `.bin` is
`struct`-packed, and the PNG is written with `struct` + `zlib` (as
`make_checker_asset.py` does).

Outputs (relative to the repository root):

    examples/game3d_project/assets/meshes/crate.gltf
    examples/game3d_project/assets/meshes/crate.bin
    examples/game3d_project/assets/meshes/crate_albedo.png

The crate is a *multi-file* glTF on purpose: its vertex data lives in the
external `.bin` and its base-colour map in an external image, both named *inside*
the glTF rather than by any scene. That is the shape a pruned pack used to drop,
and it is why this asset exists in the example the web acceptance test exports
(tests/web/run_web_game.sh, ADR-0088).

Run:  python3 tools/gen_gltf_crate.py
"""
import json
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "examples" / "game3d_project" / "assets" / "meshes"

# A unit cube, one metre across, centred on the origin, matching cube.obj. Six
# faces, four unique vertices each, so every face carries one flat normal and a
# full 0..1 UV rectangle.
FACES = [
    ((0.0, 0.0, 1.0), [(0.5, -0.5, 0.5), (0.5, -0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5)]),
    ((0.0, 0.0, 1.0), [(0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (-0.5, 0.5, -0.5), (-0.5, 0.5, 0.5)]),
    ((1.0, 0.0, 0.0), [(0.5, 0.5, 0.5), (0.5, 0.5, -0.5), (0.5, -0.5, -0.5), (0.5, -0.5, 0.5)]),
    ((-1.0, 0.0, 0.0), [(-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5), (-0.5, -0.5, -0.5), (-0.5, -0.5, 0.5)]),
    ((0.0, 1.0, 0.0), [(0.5, 0.5, 0.5), (-0.5, 0.5, 0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5)]),
    ((0.0, -1.0, 0.0), [(0.5, -0.5, 0.5), (-0.5, -0.5, 0.5), (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5)]),
]
UVS = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]


def geometry():
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in FACES:
        base = len(positions)
        for corner, uv in zip(corners, UVS):
            positions.append(corner)
            normals.append(normal)
            uvs.append(uv)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def pad4(blob: bytes) -> bytes:
    return blob + b"\x00" * ((4 - len(blob) % 4) % 4)


def chunk(tag: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def crate_png(size: int = 64) -> bytes:
    """A two-tone crate face: a border and a diagonal, so the map is not flat."""
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        for x in range(size):
            border = x < 6 or y < 6 or x >= size - 6 or y >= size - 6
            stripe = abs((x + y) - (size - 1)) < 4
            if border:
                colour = (60, 44, 30, 255)
            elif stripe:
                colour = (150, 110, 70, 255)
            else:
                colour = (198, 152, 96, 255)
            raw.extend(colour)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    return png


def main() -> None:
    positions, normals, uvs, indices = geometry()
    pos = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm = b"".join(struct.pack("<3f", *n) for n in normals)
    uv = b"".join(struct.pack("<2f", *t) for t in uvs)
    idx = b"".join(struct.pack("<H", i) for i in indices)

    pos_off = 0
    nrm_off = len(pos)
    uv_off = nrm_off + len(nrm)
    idx_off = uv_off + len(uv)
    buffer = pad4(pos) + pad4(nrm) + pad4(uv) + pad4(idx)
    # Re-derive the offsets after padding, since padding may have shifted them.
    nrm_off = len(pad4(pos))
    uv_off = nrm_off + len(pad4(nrm))
    idx_off = uv_off + len(pad4(uv))

    doc = {
        "asset": {"version": "2.0", "generator": "tools/gen_gltf_crate.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "Crate"}],
        "buffers": [{"uri": "crate.bin", "byteLength": len(buffer)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_off, "byteLength": len(pos), "target": 34962},
            {"buffer": 0, "byteOffset": nrm_off, "byteLength": len(nrm), "target": 34962},
            {"buffer": 0, "byteOffset": uv_off, "byteLength": len(uv), "target": 34962},
            {"buffer": 0, "byteOffset": idx_off, "byteLength": len(idx), "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
             "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "images": [{"uri": "crate_albedo.png"}],
        "textures": [{"source": 0}],
        "materials": [{
            "name": "Crate",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0,
                "roughnessFactor": 0.85,
            },
            "doubleSided": False,
        }],
        "meshes": [{"name": "Crate", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3,
            "material": 0,
        }]}],
    }

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "crate.gltf").write_text(json.dumps(doc, indent=2) + "\n")
    (OUT_DIR / "crate.bin").write_bytes(buffer)
    (OUT_DIR / "crate_albedo.png").write_bytes(crate_png())
    print(f"wrote {OUT_DIR}/crate.gltf, crate.bin ({len(buffer)} bytes), crate_albedo.png")


if __name__ == "__main__":
    main()
