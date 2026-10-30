// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/mesh/Vertex3D.h"

#include <array>

namespace fe {

// A mesh vertex that carries skinning data as well as a position, normal, and
// uv.
//
// Why a second struct and a second layout rather than adding fields to
// `Vertex3D`. Every existing mesh — every primitive generator, every OBJ, every
// unskinned glTF — is a `Vertex3D`, and the static 3D path's vertex shader
// declares exactly three attributes. Widening `Vertex3D` would make every static
// vertex pay 16 bytes for joint data it does not have, and would force the
// static shader to declare attributes it ignores. `Vertex3D`'s own comment
// prescribes this split ("adding tangents, colour, or bone weights later means a
// second layout and a shader that consumes it, not a redesign of this one"), and
// the renderer already keys its GPU buffers per mesh, so two layouts cost nothing
// but a second upload path.
//
// The four influences. Linear blend skinning weights a vertex by up to four
// joints, which is what a DCC exporter writes by default and what a joint can
// actually influence in practice; a vertex near a shoulder blends between the
// arm and the chest. glTF allows four per set and multiple sets; this reads the
// first set, which is what essentially every exporter emits, and a vertex with
// fewer influences simply leaves the remaining weights at zero.
//
// `joints` indexes the skeleton's joint array. The weights need not sum to one:
// the skinner normalises them, because a hand-authored or hand-edited asset can
// carry weights that do not, and a vertex whose weights sum to 0.5 would
// otherwise shrink towards the origin.
//
// The joint indices are `f32`, not `u16`, and that is forced by the GPU side:
// `VertexLayout` has no integer kind, so the attribute the shader reads is a
// float and the shader converts it (`ivec4(a_joints + 0.5)`). Storing `u16` here
// would make this struct 56 bytes while the layout's stride is 64, and the
// renderer sizes the vertex buffer from `sizeof(SkinnedVertex3D)` -- so every
// mesh would be uploaded 8 bytes per vertex past a buffer sized for fewer
// vertices, and the draw would read out of bounds. The two must agree, which the
// static_assert below pins.
struct SkinnedVertex3D {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    // The four joints influencing this vertex, as indices into the skeleton.
    // Exact integers, held as floats to match the vertex layout.
    std::array<f32, 4> joints{0.0f, 0.0f, 0.0f, 0.0f};
    // The four weights, one per entry of `joints`.
    std::array<f32, 4> weights{0.0f, 0.0f, 0.0f, 0.0f};
};

// The interleaved layout's stride, restated so a change to either the struct or
// the layout fails to compile here rather than at draw time. The matching
// `VertexLayout::skinned_position_normal_uv()` declares attributes at offsets
// 0, 12, 24, 32, and 48, which only line up when this is 64.
static_assert(sizeof(SkinnedVertex3D) == 64, "SkinnedVertex3D must match VertexLayout::skinned_position_normal_uv()");

// A joint index as an array subscript. The field is a float so the vertex layout
// can describe it (see above); the value is an exact integer, and this is the one
// place the conversion happens on the CPU side, so a negative or absurd value
// reads as an index no palette has rather than as a wild subscript.
[[nodiscard]] inline usize joint_index(f32 joint) noexcept {
    return joint < 0.0f ? static_cast<usize>(-1) : static_cast<usize>(joint);
}

// The same vertex with the skinning data dropped, for a caller that wants to
// compare a skinned mesh against its static form (a test proving a bind-pose
// skinned mesh is the static mesh).
[[nodiscard]] inline Vertex3D to_static_vertex(const SkinnedVertex3D& vertex) noexcept {
    return Vertex3D{vertex.position, vertex.normal, vertex.uv};
}

} // namespace fe
