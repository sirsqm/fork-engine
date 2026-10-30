// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Frustum.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"

#include <span>
#include <vector>

namespace fe {

class FrameArena;
class JobSystem;
class Mesh;
class MeshRenderer3D;
class Scene;
class Sprite2D;
class TextLabel;
class Tilemap;

namespace anim {
class Animator;
class SkinnedMesh;
class SkinnedMeshRenderer3D;
} // namespace anim

namespace rhi {
class Texture;
} // namespace rhi

namespace ui {
class UiElement;
} // namespace ui

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
    // The text label, when this instance is a glyph rather than a sprite or a
    // tile. At most one of `sprite`, `tilemap`, and `label` is non-null; like
    // `tilemap` it is provenance the draw step does not read.
    const TextLabel* label = nullptr;
    // The UI element, when this instance is a screen-space UI quad (a fill, a UI
    // texture, or a UI glyph) rather than world content. Null for world content.
    // Provenance for tests and debugging, like the fields above; it is also how a
    // test tells a UI glyph from a world glyph when both are in one array.
    const ui::UiElement* ui = nullptr;
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

    // Exact field equality. Extraction is a pure function of the scene, so a
    // parallel extraction must reproduce a serial one bit for bit; a test
    // compares two arrays with this to prove it, which an approximate compare
    // could not (it would hide a lost or reordered instance).
    [[nodiscard]] constexpr bool operator==(const SpriteInstance&) const noexcept = default;
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

    // Exact field equality, for the same reason `SpriteInstance` has it: a
    // parallel extraction must reproduce the serial array exactly.
    [[nodiscard]] constexpr bool operator==(const MeshInstance&) const noexcept = default;
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

// One skinned mesh's resolved draw state.
//
// A separate struct from `MeshInstance` rather than extra fields on it. A skinned
// instance carries a palette and a different vertex stream, and it cannot be
// batched with a static instance of the same mesh (the two need different shaders
// and different vertex layouts), so a union of the two would be a struct whose
// fields are only meaningful for one of its two states. The two arrays stay
// separate and the renderer runs two passes over them.
//
// The palette is a borrowed pointer, not a copy. It is owned by the `Animator`
// the instance came from and is valid for the duration of the frame, which is
// exactly the window the draw runs in. Copying it into the instance would be four
// matrices per joint per character per frame for no benefit.
struct SkinnedInstance {
    // The skinned renderer component, for its resolved material. Kept for the
    // same reason `MeshInstance::renderer` is.
    const anim::SkinnedMeshRenderer3D* renderer = nullptr;
    // The mesh to draw. Its GPU buffers are acquired at draw time.
    const anim::SkinnedMesh* mesh = nullptr;
    // The animator that produced the palette, or null when the mesh has none.
    // Kept for provenance and so a test can tell "drawn in bind pose" from
    // "drawn posed".
    const anim::Animator* animator = nullptr;
    // The skinning palette to upload, one matrix per joint. Null or empty means
    // the bind pose, which is what an unanimated character draws with.
    const std::vector<Mat4>* palette = nullptr;
    // The owning node's world matrix.
    Mat4 model{};
    // The mesh's world-space AABB. When the palette is valid this is the *posed*
    // bounds, expanded by the skinning: a raised arm reaches outside the bind-pose
    // box, so a cull that used the bind box would drop a character mid-swing.
    Vec3 world_min{};
    Vec3 world_max{};

    [[nodiscard]] constexpr bool operator==(const SkinnedInstance&) const noexcept = default;
};

// Builds the key for `instance`.
[[nodiscard]] inline MeshInstanceKey instance_key(const MeshInstance& instance) noexcept {
    return MeshInstanceKey{instance.mesh, instance.world_min, instance.world_max};
}

// Collects every drawable sprite in `scene` into `out` (cleared first). A sprite
// is skipped when it has no texture or its texture is not a `Texture2D`, which
// matches the pre-seam behaviour: an untextured sprite is an authoring state,
// not content.
//
// `jobs` is optional. With a null pointer (the default) the walk is serial, which
// is the whole behaviour when no pool exists. With a real `JobSystem` and a scene
// large enough to pay for the fan-out (see `kParallelExtractThreshold`) the walk
// is chunked and run in parallel; the result is byte-for-byte identical to the
// serial one, because each chunk appends to its own list and the lists are
// concatenated in node order. A scene below the threshold runs serially even
// when a pool is supplied, because fanning out a handful of nodes costs more than
// it saves (ADR-0017: a change that cannot show a win does not land).
void extract_sprites(const Scene& scene, std::vector<SpriteInstance>& out, JobSystem* jobs = nullptr);

// Appends every non-empty tile of every `Tilemap` in `scene` to `out`. Unlike
// extract_sprites this does *not* clear: a tilemap and the sprites above it
// belong in one array so the layer sort orders them together. Pass an array
// already filled by extract_sprites, or an empty one for tiles alone.
//
// Each tile becomes one `SpriteInstance` whose `world` is the map node's world
// matrix composed with the cell's local offset, and whose `uv` is the tile's
// cell in the tileset. A map with no texture, an empty grid, or a zero tile size
// contributes nothing.
//
// `jobs` behaves as it does for extract_sprites, with one difference: the tile
// walk is parallelised only when the map has more tiles than the threshold
// (`kParallelTileThreshold`), and a single map is split across chunks by cell so
// its tiles are still appended in row-major order. Chunk-local lists keep the
// result identical to serial.
void extract_tiles(const Scene& scene, std::vector<SpriteInstance>& out, JobSystem* jobs = nullptr);

// Appends the glyph quads of every `TextLabel` in `scene` to `out`. Like
// extract_tiles this does *not* clear, so a HUD's text and the sprites under it
// land in one array and the layer sort orders them together.
//
// One glyph becomes one `SpriteInstance` whose `world` is the label node's world
// matrix composed with the glyph's local offset and whose `uv` is the glyph's
// atlas cell. The metrics are in em units, so the label's `size` is the single
// scale factor. A label with no font, no text, or a font whose atlas is missing
// contributes nothing; a byte with no glyph contributes nothing but still
// advances by zero, so an unsupported character is a gap and not a broken line.
//
// `jobs` behaves as it does for extract_sprites. A single label is laid out
// serially -- a line is at most a few dozen quads and the fan-out would cost more
// than it saves -- so text never splits mid-line.
void extract_text(const Scene& scene, std::vector<SpriteInstance>& out, JobSystem* jobs = nullptr);

// --- Arena-backed extraction (ADR-0054 follow-on) --------------------------
//
// The `std::vector` overloads above are correct and remain the default. These
// write the same instances into a caller-supplied span backed by the frame
// arena, so a frame can build its extraction output with no heap traffic at all.
//
// The trade is explicit: the caller must size the destination, because the arena
// does not grow. `out.size()` bounds how many are written, and the return value is
// how many were (which may be fewer than the scene holds if the bound was too
// small). `out` is overwritten from the front; any elements past the returned
// count are untouched.
//
// The parallel path writes the same values in the same order as the serial one,
// and neither allocates from the heap: the per-chunk lists the vector path uses
// would each be a heap allocation, so these paths count the instances first and
// then write each chunk straight into its final slice of `out`. Any scratch the
// parallel path needs (the flattened node list, the per-chunk counts) comes from
// `arena` too, so the whole step stays off the heap.
//
// `jobs` behaves as it does above, including the thresholds. A null or
// non-threaded pool always takes the serial path.
usize extract_sprites_into(FrameArena& arena, const Scene& scene, std::span<SpriteInstance> out,
                           JobSystem* jobs = nullptr);

// Appends tiles into `out` starting at `offset`, returning the new end index. It
// appends rather than overwrites for the same reason the vector overload does:
// tiles and sprites share one array so the layer sort orders them together. Pass
// the count `extract_sprites_into` returned as `offset`.
usize extract_tiles_into(FrameArena& arena, const Scene& scene, std::span<SpriteInstance> out, usize offset,
                         JobSystem* jobs = nullptr);

// Appends text glyphs into `out` starting at `offset`, returning the new end
// index. It appends for the same reason the other walks do: text shares one
// array with the sprites and tiles so the layer sort orders them together. Pass
// the count the previous walk returned as `offset`.
usize extract_text_into(FrameArena& arena, const Scene& scene, std::span<SpriteInstance> out, usize offset,
                        JobSystem* jobs = nullptr);

// An upper bound on the number of sprite instances `scene` can produce: one
// sprite per node, plus one per non-empty cell of every tilemap, plus one per
// byte of every label's text. A caller sizing an arena destination uses this so
// the extraction can never overflow the span. It walks the scene once and
// allocates nothing.
//
// The text term counts bytes rather than glyphs because the bound must never be
// short: an unlisted byte draws nothing, so counting it is safe, and the count is
// what keeps the arena from being sized for a short string and then handed a long
// one.
[[nodiscard]] usize sprite_instance_bound(const Scene& scene);

// --- UI (ADR-0084) ---------------------------------------------------------
//
// UI is screen-space, so it cannot share the world pass's projection: a HUD must
// not move when the camera pans. It is therefore extracted into the same
// `SpriteInstance` shape -- a UI fill, a UI texture, and a glyph are all textured
// quads -- but positioned in *viewport pixels* (origin top-left, +Y down) and
// drawn by a second pass with a pixel-space projection. Sharing the instance
// shape is what makes a UI quad cost no new shader, no new pipeline state, and no
// new batching path; the two passes differ only in the matrix they draw with.
//
// These do *not* clear `out` and append, so a caller that wants UI over the world
// draws the world array first and this second. The instances are written in draw
// order already (a parent before its children, siblings in tree order), so a
// caller must not re-sort them across the pass boundary.
//
// Layout is delegated to `ui::layout`, the same function the input system calls
// to hit test. One layout implementation, called from two places, is what
// guarantees a button draws where it responds.
//
// `jobs` is accepted for symmetry with the other walks. Layout is a scene walk
// and a UI is a few dozen elements, so the fan-out would cost more than it saves;
// the walk is serial.
void extract_ui(const Scene& scene, u32 viewport_width, u32 viewport_height, std::vector<SpriteInstance>& out,
                JobSystem* jobs = nullptr);

// The arena-destination UI walk, appending from `offset` and returning the new
// end index, for the same reason the other arena walks append.
usize extract_ui_into(FrameArena& arena, const Scene& scene, u32 viewport_width, u32 viewport_height,
                      std::span<SpriteInstance> out, usize offset, JobSystem* jobs = nullptr);

// An upper bound on the number of UI instances `scene` can produce for a
// viewport: a fill, a texture, and one glyph per byte of text, per visible
// element. Like `sprite_instance_bound` it counts bytes rather than glyphs so the
// bound can never be short.
[[nodiscard]] usize ui_instance_bound(const Scene& scene);

// Collects every drawable mesh instance in `scene` into `out` (cleared first).
// Each referenced mesh is also appended to `meshes_seen` (never cleared here),
// which the renderer's buffer sweep uses to decide liveness: a mesh that is
// merely off screen must keep its GPU buffers. Pass nullptr when the caller does
// not need the liveness list.
//
// `jobs` behaves as it does for extract_sprites. `meshes_seen` is filled in node
// order regardless of the mode, so the buffer sweep sees the same list either
// way.
void extract_meshes(const Scene& scene, std::vector<MeshInstance>& out, std::vector<const Mesh*>* meshes_seen,
                    JobSystem* jobs = nullptr);

// --- Skinned extraction ----------------------------------------------------
//
// Skinned meshes are extracted separately from static ones, for the reason
// `SkinnedInstance` documents: they need a different shader, a different vertex
// layout, and a per-instance palette, so they cannot share a draw or a batch key
// with a static mesh.
//
// The extraction is a pure read of the scene: it does not sample a clip or
// compute a palette. It takes the palette the `Animator` already published, so
// the ordering rule is explicit and testable -- `SkeletonAnimationSystem` must
// have run first. An instance whose animator has no palette yet extracts with a
// null palette and draws in its bind pose, which is the correct behaviour for a
// character whose first animation step has not happened.
//
// The world AABB is the *posed* box when a palette is present, because a skinned
// character reaches outside its bind-pose bounds as it moves. Computing it here
// costs one pass over the mesh's vertices per instance per frame; a caller that
// would rather not pay it (a scene of many distant characters) is the reason the
// bind-pose box is used when there is no palette, and the reason this is a
// separate function a caller can choose not to call.
void extract_skinned_meshes(const Scene& scene, std::vector<SkinnedInstance>& out, JobSystem* jobs = nullptr);

// Culls skinned instances by their precomputed world AABB, exactly as
// `cull_meshes` does for static ones. A null `frustum` culls nothing.
[[nodiscard]] usize cull_skinned_meshes(std::vector<SkinnedInstance>& instances, const Frustum* frustum,
                                        JobSystem* jobs = nullptr);

// --- Culling ---------------------------------------------------------------
//
// Conservative: an instance is dropped only when its world AABB lies wholly
// outside the frustum, so culling can only ever save a draw, never hide a
// visible instance. Returns the number dropped. A null `frustum` culls nothing,
// which is the explicit "draw everything" case.
//
// These are pure functions of the array, which is what lets them be tested with
// no scene and run in parallel over chunks of the array.
//
// `jobs` is optional and defaults to null (serial). Above
// `kParallelCullThreshold` instances, a real `JobSystem` tests visibility in
// parallel into a byte mask and then compacts serially; that is the shape the
// measurement supports. Culling is cheap per instance, so on the common
// (smaller) scene the fan-out costs more than the test it saves and the serial
// path is kept -- which is why the threshold is high. The survivors are in the
// same order either way: the mask is indexed by position, so compaction moves
// instance i to its original relative place.

// Culls sprites using a quad-sized AABB around each sprite's world position.
// The AABB is derived from the sprite's resolved size, so a sprite larger than
// the frustum is kept, as it must be.
usize cull_sprites(std::vector<SpriteInstance>& instances, const Frustum* frustum, JobSystem* jobs = nullptr);

// The arena-destination cull. `instances` is a fixed span (the extraction wrote
// `used` of it); survivors are compacted to the front and the count kept is
// returned. Nothing is resized and nothing is allocated on the heap: the parallel
// visibility mask comes from `arena`, so a frame whose extraction came from the
// arena stays off the heap end to end. A null `frustum` keeps everything and
// returns `used`.
//
// The result is the same survivors in the same order as the vector overload:
// compaction preserves relative order in both.
usize cull_sprites_in_place(FrameArena& arena, std::span<SpriteInstance> instances, usize used, const Frustum* frustum,
                            JobSystem* jobs = nullptr);

// Culls meshes using each instance's precomputed world AABB.
usize cull_meshes(std::vector<MeshInstance>& instances, const Frustum* frustum, JobSystem* jobs = nullptr);

// --- Parallel thresholds ---------------------------------------------------
//
// The instance counts at or above which a supplied `JobSystem` is actually used.
// Below them the serial path runs even when a pool is available, because the
// fan-out (queueing chunks, waking workers, joining) is a fixed cost that a small
// scene does not repay.
//
// The values come from a sweep on the reference machine (4 workers, the null
// device), timing only the step, with the input copy excluded:
//
//   extract: ~1200 nodes is break-even (1.0x); 2000 is 1.25x, 8000 is ~1.9x.
//   cull:    below ~16000 instances the parallel path is *slower* (a loss at
//            2000: 0.33x); it only wins from about 16000 (1.04x) upward.
//
// So extraction crosses over early and culling late, which is why the two
// thresholds differ by an order of magnitude. They are deliberately conservative:
// the cost of parallelising too eagerly is a slower frame, and these numbers are
// a starting point to be re-measured, not a law.
inline constexpr usize kParallelExtractThreshold = 1024;
inline constexpr usize kParallelTileThreshold = 4096;
inline constexpr usize kParallelCullThreshold = 16384;

} // namespace render
} // namespace fe
