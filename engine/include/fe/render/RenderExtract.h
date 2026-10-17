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
class Tilemap;

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
// reads is copied here.
//
// A tile from a `Tilemap` produces one of these too: a tile *is* a textured
// quad, so the map reuses the sprite draw path rather than adding a second one.
// The copied fields are what make that possible -- the draw step never has to
// ask which component produced an instance.
struct SpriteInstance {
    // The GPU texture to bind. Never null in an extracted array: a sprite with
    // no texture, or a texture that is not a `Texture2D`, is skipped.
    rhi::Texture* texture = nullptr;
    // The sprite component, kept for provenance (tests and debugging). The draw
    // reads the resolved fields below, not this.
    const Sprite2D* sprite = nullptr;
    // The tilemap component, when this instance is a tile rather than a sprite.
    // Null for a sprite; `sprite` is null for a tile.
    const Tilemap* tilemap = nullptr;
    // The owning node's world matrix, already composed with the tile's own cell
    // offset for a tilemap instance.
    Mat4 world{};
    // The quad size in world units, resolved from the sprite's explicit size or,
    // when the sprite asked for the texture size, from the texture. For a tile
    // this is the tileset's tile size.
    Vec2 size{};
    // The UV sub-rectangle to sample. Resolved at extraction so a tile's cell
    // and a sprite's atlas cell reach the draw step the same way.
    Vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
    // The tint multiplied with the sampled texel.
    Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    // Render order within the 2D pass. Higher values draw on top.
    i32 layer = 0;
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

// A key that identifies a mesh instance's static geometry across frames. A BVH
// over static geometry is only worth rebuilding when the geometry actually
// moved, so the renderer compares one of these per frame and rebuilds only on a
// change. It is a value type on purpose: no scene, no node, no pointers into
// engine state, so a test can build the key directly and assert on the rebuild
// decision with no scene at all.
//
// Equality is exact, not approximate. A rebuild is cheap relative to a wrong
// cull, so a float that changed by one ulp must count as a change.
struct MeshInstanceKey {
    const Mesh* mesh = nullptr;
    Vec3 world_min{};
    Vec3 world_max{};

    [[nodiscard]] constexpr bool operator==(const MeshInstanceKey&) const noexcept = default;
};

// Builds the key for `instance`.
[[nodiscard]] inline MeshInstanceKey instance_key(const MeshInstance& instance) noexcept {
    return MeshInstanceKey{instance.mesh, instance.world_min, instance.world_max};
}

// Collects every drawable sprite in `scene` into `out` (cleared first). A sprite
// is skipped when it has no texture or its texture is not a `Texture2D`, which
// matches the pre-seam behaviour: an untextured sprite is an authoring state,
// not content.
void extract_sprites(const Scene& scene, std::vector<SpriteInstance>& out);

// Appends every non-empty tile of every `Tilemap` in `scene` to `out`. Unlike
// extract_sprites this does *not* clear: a tilemap and the sprites above it
// belong in one array so the layer sort orders them together. Pass an array
// already filled by extract_sprites, or an empty one for tiles alone.
//
// Each tile becomes one `SpriteInstance` whose `world` is the map node's world
// matrix composed with the cell's local offset, and whose `uv` is the tile's
// cell in the tileset. A map with no texture, an empty grid, or a zero tile size
// contributes nothing.
void extract_tiles(const Scene& scene, std::vector<SpriteInstance>& out);

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
