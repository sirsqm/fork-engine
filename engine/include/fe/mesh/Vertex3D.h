// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"

namespace fe {

// A single mesh vertex: position, normal, and texture coordinate.
//
// This is the interleaved layout the 3D renderer uploads. It is deliberately
// small and fixed: the first cut renders lit, textured surfaces only. Adding
// tangents, colour, or bone weights later means a second layout and a shader
// that consumes it, not a redesign of this one.
struct Vertex3D {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

} // namespace fe
