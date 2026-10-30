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
struct SkinnedVertex3D {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    // The four joints influencing this vertex, as indices into the skeleton.
    std::array<u16, 4> joints{0, 0, 0, 0};
    // The four weights, one per entry of `joints`.
    std::array<f32, 4> weights{0.0f, 0.0f, 0.0f, 0.0f};
};

// The same vertex with the skinning data dropped, for a caller that wants to
// compare a skinned mesh against its static form (a test proving a bind-pose
// skinned mesh is the static mesh).
[[nodiscard]] inline Vertex3D to_static_vertex(const SkinnedVertex3D& vertex) noexcept {
    return Vertex3D{vertex.position, vertex.normal, vertex.uv};
}

} // namespace fe
