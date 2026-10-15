// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Frustum.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <vector>

namespace fe {

class Mesh;
class MeshRenderer3D;
class Scene;
class Sprite2D;

namespace rhi {
class Texture;
} // namespace rhi

namespace render {

// The extraction seam (ADR-0017, ADR-0052).
//
// A renderer used to walk the scene graph, decide what was visible, sort it, and
// issue draws in one pass. The seam splits that into two steps that meet at a
// flat array of instances:
//
//   1. Extraction - one walk of the hierarchy that reads each node's components
//      and emits a plain value per drawable (what to draw, its world matrix, its
//      material handle). It does no GPU work.
//   2. Culling - a separate pass over that array that drops instances whose world
//      bounds lie outside the view frustum.
//
// Draw submission then iterates the surviving array. The array is the unit that
// sorting, batching, instancing, and parallel culling operate on; it is ADR-0002
// applied to the hot path: the authoring model (scene graph) and the processing
// model (flat array) are deliberately different, and the conversion happens in
// one place.
//
// The types are shared by both renderers so a test can hold an extracted array
// and assert on it with no GPU, and so the follow-on parallel-culling step has
// one shape to chunk regardless of which renderer produced it.
//
// A `MeshInstance` stores the mesh's world-space AABB rather than its local
// bounds. Computing it once at extraction is what makes the culling pass a pure
// function of the array: it needs no `Mesh` and no `Scene`.

// One sprite's resolved draw state. The renderer does not hold the `Sprite2D`
// past extraction for anything but the fields the draw needs; every value a draw
// reads is either copied here or reachable from `sprite` (which the scene owns
// and which outlives the frame).
struct SpriteInstance {
    // The GPU texture to bind. Never null in an extracted array: a sprite with
    // no texture, or a texture that is not a `Texture2D`, is skipped.
    rhi::Texture* texture = nullptr;
    // The sprite component, kept for `layer` (the primary sort key) and `color`.
    const Sprite2D* sprite = nullptr;
    // The owning node's world matrix.
    Mat4 world{};
    // The quad size in world units, resolved from the sprite's explicit size or,
    // when the sprite asked for the texture size, from the texture.
    Vec2 size{};
};

// One mesh instance's resolved draw state.
struct MeshInstance {
    // The renderer component, kept for `base_color`, `specular`, and its texture.
    const MeshRenderer3D* renderer = nullptr;
    // The mesh to draw. Its GPU buffers are acquired at draw time; the instance
    // only records which mesh it is.
    const Mesh* mesh = nullptr;
    // The owning node's world matrix.
    Mat4 model{};
    // The mesh's local AABB expanded to world space under `model`, computed once
    // so the culling pass needs neither the `Mesh` nor the `Scene`.
    Vec3 world_min{};
    Vec3 world_max{};
};

// Collects every drawable sprite in `scene` into `out` (cleared first). A sprite
// is skipped when it has no texture or its texture is not a `Texture2D`, which
// matches the pre-seam behaviour: an untextured sprite is an authoring state,
// not content.
void extract_sprites(const Scene& scene, std::vector<SpriteInstance>& out);

// Collects every drawable mesh instance in `scene` into `out` (cleared first).
// Each referenced mesh is also appended to `meshes_seen` (never cleared here),
// which the renderer's buffer sweep uses to decide liveness: a mesh that is
// merely off screen must keep its GPU buffers. Pass nullptr when the caller does
// not need the liveness list.
void extract_meshes(const Scene& scene, std::vector<MeshInstance>& out, std::vector<const Mesh*>* meshes_seen);

// --- Culling ---------------------------------------------------------------
//
// Conservative: an instance is dropped only when its world AABB lies wholly
// outside the frustum, so culling can only ever save a draw, never hide a
// visible instance. Returns the number dropped. A null `frustum` culls nothing,
// which is the explicit "draw everything" case.
//
// These are pure functions of the array, which is what lets them be tested with
// no scene and, later, run in parallel over chunks of the array.

// Culls sprites using a quad-sized AABB around each sprite's world position.
// The AABB is derived from the sprite's resolved size, so a sprite larger than
// the frustum is kept, as it must be.
usize cull_sprites(std::vector<SpriteInstance>& instances, const Frustum* frustum);

// Culls meshes using each instance's precomputed world AABB.
usize cull_meshes(std::vector<MeshInstance>& instances, const Frustum* frustum);

} // namespace render
} // namespace fe
