// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/render/RenderExtract.h"

#include "fe/core/FrameArena.h"
#include "fe/jobs/JobSystem.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Font.h"
#include "fe/resources/Texture2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene/TextLabel.h"
#include "fe/scene/Tilemap.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <algorithm>
#include <array>

namespace fe::render {
namespace {

// The world-space AABB of a local AABB under `model`. Transforming the eight
// corners and taking the min/max is exact for an AABB: the extremes of a linear
// map over a box occur at vertices. This is the same computation the 3D
// renderer used inline before the seam; it lives here now so extraction can
// bake the world AABB into each instance once, and culling becomes a pure
// function of the array.
void expand_to_world_aabb(const Vec3& local_min, const Vec3& local_max, const Mat4& model, Vec3& out_min,
                          Vec3& out_max) noexcept {
    const std::array<Vec3, 8> corners = {
        Vec3{local_min.x, local_min.y, local_min.z}, Vec3{local_max.x, local_min.y, local_min.z},
        Vec3{local_min.x, local_max.y, local_min.z}, Vec3{local_max.x, local_max.y, local_min.z},
        Vec3{local_min.x, local_min.y, local_max.z}, Vec3{local_max.x, local_min.y, local_max.z},
        Vec3{local_min.x, local_max.y, local_max.z}, Vec3{local_max.x, local_max.y, local_max.z},
    };
    out_min = transform_point(model, corners[0]);
    out_max = out_min;
    for (usize i = 1; i < corners.size(); ++i) {
        const Vec3 p = transform_point(model, corners[i]);
        out_min.x = std::min(out_min.x, p.x);
        out_min.y = std::min(out_min.y, p.y);
        out_min.z = std::min(out_min.z, p.z);
        out_max.x = std::max(out_max.x, p.x);
        out_max.y = std::max(out_max.y, p.y);
        out_max.z = std::max(out_max.z, p.z);
    }
}

// One sprite's resolved instance, or a reason to skip it. Both the serial and
// the parallel walk use this, so the two cannot diverge on which nodes count or
// on what a node resolves to.
[[nodiscard]] bool resolve_sprite(const Node& node, SpriteInstance& out) {
    const auto* sprite = node.get_component<Sprite2D>();
    if (sprite == nullptr || !sprite->has_texture()) {
        // An untextured sprite is an authoring state, not content; skip it,
        // matching the pre-seam renderer.
        return false;
    }
    const auto* texture = dynamic_cast<const Texture2D*>(sprite->texture.get());
    if (texture == nullptr) {
        return false;
    }

    // Resolve the draw size: explicit size, else the texture size.
    Vec2 size = sprite->size;
    if (size.x == 0.0f || size.y == 0.0f) {
        size = Vec2{static_cast<f32>(texture->width()), static_cast<f32>(texture->height())};
    }

    out = SpriteInstance{};
    out.texture = &texture->texture();
    out.sprite = sprite;
    out.world = node.world_matrix();
    out.size = size;
    out.uv = sprite->uv;
    out.color = sprite->color;
    out.layer = sprite->layer;
    return true;
}

// Flattens the scene into node pointers in pre-order. The parallel walks index
// this list by chunk; the order is the same depth-first order for_each_node
// visits, which is what makes a chunked walk reproduce the serial one.
void flatten_nodes(const Scene& scene, std::vector<const Node*>& out) {
    out.clear();
    scene.for_each_node([&](Node& node) { out.push_back(&node); });
}

// Resolves the instance for cell (x, y) of `map` into `out`, or returns false
// when the cell is empty. Shared by the serial and parallel tile walks, and by
// the vector and arena destinations, so none of them can disagree on a cell's
// world matrix or UV.
[[nodiscard]] bool resolve_tile(const Tilemap& map, const Mat4& node_world, const Texture2D* texture, u32 x, u32 y,
                                SpriteInstance& out) {
    const i32 tile = map.at(x, y);
    if (tile == Tilemap::kEmpty) {
        return false;
    }

    // Cell (0, 0) is the map's top-left, +X right and +Y down, so the map spans
    // x in [0, width * tile.x] and y in [-height * tile.y, 0] in the node's local
    // space. Centring each tile within its cell puts the cell's centre at
    // (x + 0.5) tiles across.
    const f32 local_x = (static_cast<f32>(x) + 0.5f) * map.tile_size.x;
    const f32 local_y = -(static_cast<f32>(y) + 0.5f) * map.tile_size.y;

    out = SpriteInstance{};
    out.texture = &texture->texture();
    out.tilemap = &map;
    out.world = node_world * Mat4::translation(Vec3{local_x, local_y, 0.0f});
    out.size = map.tile_size;
    out.uv = map.tile_uv(tile);
    out.color = map.color;
    out.layer = map.layer;
    return true;
}

// The vector-destination wrapper around resolve_tile.
void append_tile(const Tilemap& map, const Mat4& node_world, const Texture2D* texture, u32 x, u32 y,
                 std::vector<SpriteInstance>& out) {
    SpriteInstance instance;
    if (resolve_tile(map, node_world, texture, x, y, instance)) {
        out.push_back(instance);
    }
}

// One mesh instance's resolved state, or a reason to skip it. Shared by the
// serial and parallel walks.
[[nodiscard]] bool resolve_mesh(const Node& node, MeshInstance& out) {
    const auto* renderer = node.get_component<MeshRenderer3D>();
    if (renderer == nullptr || !renderer->has_mesh()) {
        return false;
    }
    const auto* mesh = dynamic_cast<const Mesh*>(renderer->mesh.get());
    if (mesh == nullptr || mesh->index_count() == 0) {
        return false;
    }

    const Mat4 model = node.world_matrix();
    out = MeshInstance{};
    out.renderer = renderer;
    out.mesh = mesh;
    out.model = model;
    expand_to_world_aabb(mesh->bounds_min(), mesh->bounds_max(), model, out.world_min, out.world_max);
    return true;
}

// The pen's starting x, in the node's local space: where alignment places the
// line's left edge relative to the node origin. Measuring first is what lets a
// score be pinned to a corner without the author knowing its width. Shared by
// the vector and arena walks.
[[nodiscard]] f32 pen_origin(const TextLabel& label, const Font& font) noexcept {
    if (label.align == TextLabel::Align::Left) {
        return 0.0f;
    }
    const f32 width = font.measure(label.text) * label.size;
    return label.align == TextLabel::Align::Center ? -width * 0.5f : -width;
}

// One glyph quad's resolved instance, or a reason to skip it. `pen` is the
// label's pen origin in the node's local space, already advanced past the glyphs
// before this one. Shared by the vector and arena text walks so the two cannot
// disagree on a glyph's placement or cell.
[[nodiscard]] bool resolve_glyph(const TextLabel& label, const Font& font, const Mat4& node_world, u8 code,
                                 const Vec2& pen, const Texture2D* texture, SpriteInstance& out) {
    const Font::Glyph* glyph = font.glyph(code);
    // A byte the atlas does not hold draws nothing. It is not an error: an
    // unsupported character in a string should be a gap, and the caller's
    // advance() already skipped its width.
    if (glyph == nullptr || glyph->size.x <= 0.0f || glyph->size.y <= 0.0f) {
        return false;
    }
    // A zero-area cell is the atlas's way of saying "no ink" -- a space, or a
    // glyph the generator deliberately left blank. It advances but draws
    // nothing, so a blank never costs a transparent quad.
    if (glyph->uv.z <= glyph->uv.x || glyph->uv.w <= glyph->uv.y) {
        return false;
    }

    // The metrics are em units, so `size` is the single scale factor. The glyph
    // quad's centre is the pen plus the bearing plus half the glyph, and the
    // bearing's +Y is up while the world's +Y is up too, so no flip is needed
    // here: the atlas's V axis is handled by the generator writing v0 top-down.
    const f32 scale = label.size;
    const Vec2 half{glyph->size.x * 0.5f * scale, glyph->size.y * 0.5f * scale};
    const f32 local_x = pen.x + glyph->bearing.x * scale + half.x;
    const f32 local_y = pen.y + glyph->bearing.y * scale + half.y;

    out = SpriteInstance{};
    out.texture = &texture->texture();
    out.label = &label;
    out.world = node_world * Mat4::translation(Vec3{local_x, local_y, 0.0f});
    out.size = Vec2{glyph->size.x * scale, glyph->size.y * scale};
    out.uv = glyph->uv;
    out.color = label.color;
    out.layer = label.layer;
    return true;
}

// Lays out one label's text and appends a quad per drawable glyph to `out`.
// Returns how many were appended. Shared by every text walk.
usize append_label(const TextLabel& label, const Mat4& node_world, const Font& font, const Texture2D* texture,
                   std::vector<SpriteInstance>& out) {
    f32 pen_x = pen_origin(label, font);

    const usize before = out.size();
    for (const char raw : label.text) {
        const u8 code = static_cast<u8>(raw);
        SpriteInstance instance;
        if (resolve_glyph(label, font, node_world, code, Vec2{pen_x, 0.0f}, texture, instance)) {
            out.push_back(instance);
        }
        // The pen advances whether or not the glyph drew, so a missing glyph is
        // a gap in a line of the right length rather than a shifted one.
        pen_x += font.advance(code) * label.size;
    }
    return out.size() - before;
}

// Appends the resolved instances of `chunks` chunk-local lists to `out`, in
// chunk order. Every parallel walk ends this way, and it is the whole reason the
// parallel result equals the serial one: the fan-out order is unspecified, but
// the concatenation order is fixed, and no two chunks write the same slot.
template <typename Instance>
void concatenate(std::vector<Instance>& out, const std::vector<std::vector<Instance>>& parts, u32 chunks) {
    usize total = 0;
    for (u32 c = 0; c < chunks; ++c) {
        total += parts[c].size();
    }
    out.reserve(out.size() + total);
    for (u32 c = 0; c < chunks; ++c) {
        out.insert(out.end(), parts[c].begin(), parts[c].end());
    }
}

// The visibility predicate shared by the vector and arena culls, so the two
// cannot disagree on what is on screen. A quad-sized AABB centred on the
// sprite's world position; deriving it from the resolved size means a sprite
// larger than the view is kept.
[[nodiscard]] bool sprite_visible(const SpriteInstance& instance, const Frustum& frustum) noexcept {
    const Vec3 centre = transform_point(instance.world, Vec3{});
    const Vec3 half{instance.size.x * 0.5f, instance.size.y * 0.5f, 0.0f};
    return frustum.intersects_aabb(centre - half, centre + half);
}

} // namespace

void extract_sprites(const Scene& scene, std::vector<SpriteInstance>& out, JobSystem* jobs) {
    out.clear();

    if (jobs == nullptr || !jobs->is_threaded() || scene.node_count() < kParallelExtractThreshold) {
        scene.for_each_node([&](Node& node) {
            SpriteInstance instance;
            if (resolve_sprite(node, instance)) {
                out.push_back(instance);
            }
        });
        return;
    }

    // Parallel walk. The scene is flattened first because the hierarchy is a
    // pointer tree, which a chunk index cannot address directly; the flat list
    // is read-only during the fan-out, so every worker reads it safely. Each
    // chunk appends to its own list, and the lists are joined in node order.
    std::vector<const Node*> flat;
    flatten_nodes(scene, flat);

    const usize n = flat.size();
    const u32 chunks = static_cast<u32>(std::min<usize>(n, static_cast<usize>(jobs->worker_count()) * 4));
    std::vector<std::vector<SpriteInstance>> parts(chunks);
    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = n * c / chunks;
        const usize hi = n * (c + 1) / chunks;
        auto& local = parts[c];
        for (usize i = lo; i < hi; ++i) {
            SpriteInstance instance;
            if (resolve_sprite(*flat[i], instance)) {
                local.push_back(instance);
            }
        }
    });
    concatenate(out, parts, chunks);
}

void extract_text(const Scene& scene, std::vector<SpriteInstance>& out, JobSystem* jobs) {
    // `jobs` is accepted for symmetry with the other walks and so a caller can
    // pass its pool through without branching. A line is at most a few dozen
    // quads, so the fan-out would cost more than it saves; the layout is always
    // serial, which is also what keeps a line from splitting across chunks.
    (void)jobs;
    scene.for_each_node([&](Node& node) {
        const auto* label = node.get_component<TextLabel>();
        if (label == nullptr || label->empty()) {
            return;
        }
        const auto* font = dynamic_cast<const Font*>(label->font.get());
        if (font == nullptr || !font->has_atlas()) {
            return;
        }
        // A glyph is drawn from the same texture type a sprite is, so an
        // unrecognised atlas is skipped for the same reason.
        const auto* texture = dynamic_cast<const Texture2D*>(font->atlas().get());
        if (texture == nullptr) {
            return;
        }
        append_label(*label, node.world_matrix(), *font, texture, out);
    });
}

void extract_tiles(const Scene& scene, std::vector<SpriteInstance>& out, JobSystem* jobs) {
    scene.for_each_node([&](Node& node) {
        const auto* map = node.get_component<Tilemap>();
        if (map == nullptr || map->empty()) {
            return;
        }
        // A tile is drawn from the same texture type a sprite is, so an
        // unrecognised texture is skipped for the same reason.
        const auto* texture = dynamic_cast<const Texture2D*>(map->texture.get());
        if (texture == nullptr) {
            return;
        }
        // A zero-size tile would produce a degenerate quad; an authoring mistake
        // should draw nothing rather than a line of zero-area triangles.
        if (map->tile_size.x == 0.0f || map->tile_size.y == 0.0f) {
            return;
        }

        const Mat4 node_world = node.world_matrix();
        const usize tile_count = static_cast<usize>(map->width) * map->height;

        // The tiles of one map are a flat grid, so they can be split by cell
        // rather than by node. Below the threshold the fan-out is not worth it;
        // a map with a handful of tiles stays serial even with a pool.
        if (jobs == nullptr || !jobs->is_threaded() || tile_count < kParallelTileThreshold) {
            for (u32 y = 0; y < map->height; ++y) {
                for (u32 x = 0; x < map->width; ++x) {
                    append_tile(*map, node_world, texture, x, y, out);
                }
            }
            return;
        }

        const u32 chunks = static_cast<u32>(std::min<usize>(tile_count, static_cast<usize>(jobs->worker_count()) * 4));
        std::vector<std::vector<SpriteInstance>> parts(chunks);
        jobs->parallel_for(0, chunks, [&](usize c) {
            const usize lo = tile_count * c / chunks;
            const usize hi = tile_count * (c + 1) / chunks;
            auto& local = parts[c];
            for (usize t = lo; t < hi; ++t) {
                const u32 x = static_cast<u32>(t % map->width);
                const u32 y = static_cast<u32>(t / map->width);
                append_tile(*map, node_world, texture, x, y, local);
            }
        });
        concatenate(out, parts, chunks);
    });
}

void extract_meshes(const Scene& scene, std::vector<MeshInstance>& out, std::vector<const Mesh*>* meshes_seen,
                    JobSystem* jobs) {
    out.clear();

    if (jobs == nullptr || !jobs->is_threaded() || scene.node_count() < kParallelExtractThreshold) {
        scene.for_each_node([&](Node& node) {
            MeshInstance instance;
            if (resolve_mesh(node, instance)) {
                out.push_back(instance);
                if (meshes_seen != nullptr) {
                    meshes_seen->push_back(instance.mesh);
                }
            }
        });
        return;
    }

    std::vector<const Node*> flat;
    flatten_nodes(scene, flat);

    const usize n = flat.size();
    const u32 chunks = static_cast<u32>(std::min<usize>(n, static_cast<usize>(jobs->worker_count()) * 4));
    std::vector<std::vector<MeshInstance>> parts(chunks);
    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = n * c / chunks;
        const usize hi = n * (c + 1) / chunks;
        auto& local = parts[c];
        for (usize i = lo; i < hi; ++i) {
            MeshInstance instance;
            if (resolve_mesh(*flat[i], instance)) {
                local.push_back(instance);
            }
        }
    });
    concatenate(out, parts, chunks);

    // `meshes_seen` is filled after the fan-out, from the joined array, so it is
    // in node order regardless of how the chunks ran.
    if (meshes_seen != nullptr) {
        meshes_seen->reserve(meshes_seen->size() + out.size());
        for (const MeshInstance& instance : out) {
            meshes_seen->push_back(instance.mesh);
        }
    }
}

// --- Arena-backed extraction (ADR-0054 follow-on) --------------------------

usize sprite_instance_bound(const Scene& scene) {
    // One sprite per node is the worst case for the sprite walk (a node with no
    // sprite contributes none), plus every cell of every tilemap. A tilemap node
    // may also carry a sprite, so the two add rather than max. Counting the whole
    // grid over-counts the empty cells, which is the point of a bound: it must
    // never be short, and the caller sizes the arena from it.
    usize bound = scene.node_count();
    scene.for_each_node([&](Node& node) {
        const auto* map = node.get_component<Tilemap>();
        if (map != nullptr) {
            bound += static_cast<usize>(map->width) * map->height;
        }
        // A label can draw at most one quad per byte of its text (an unlisted
        // byte draws none), so counting the bytes can never be short. Counting
        // glyphs would under-count a string whose font is missing, which is
        // exactly the case the arena path must not overflow on.
        const auto* label = node.get_component<TextLabel>();
        if (label != nullptr) {
            bound += label->text.size();
        }
    });
    return bound;
}

usize extract_sprites_into(FrameArena& arena, const Scene& scene, std::span<SpriteInstance> out, JobSystem* jobs) {
    if (out.empty()) {
        return 0;
    }

    if (jobs == nullptr || !jobs->is_threaded() || scene.node_count() < kParallelExtractThreshold) {
        usize written = 0;
        scene.for_each_node([&](Node& node) {
            if (written == out.size()) {
                return;
            }
            if (resolve_sprite(node, out[written])) {
                ++written;
            }
        });
        return written;
    }

    // Parallel: count each chunk, turn the counts into offsets, then let each
    // chunk write straight into its own slice. The two passes read the same
    // read-only scene and use the same predicate (`resolve_sprite`), so the write
    // lands exactly where the count said and the result matches the serial array.
    //
    // The flat node list and the per-chunk counts come from the arena, so this
    // path performs no heap allocation either -- which is the whole reason it
    // exists rather than reusing the vector path's chunk lists.
    const usize capacity = scene.node_count();
    auto* flat = static_cast<const Node**>(arena.allocate(sizeof(const Node*) * capacity, alignof(const Node*)));
    const u32 chunk_capacity =
        static_cast<u32>(std::min<usize>(capacity, static_cast<usize>(jobs->worker_count()) * 4));
    auto* counts = static_cast<usize*>(arena.allocate(sizeof(usize) * chunk_capacity, alignof(usize)));
    auto* offsets = static_cast<usize*>(arena.allocate(sizeof(usize) * chunk_capacity, alignof(usize)));
    if (flat == nullptr || counts == nullptr || offsets == nullptr) {
        // Arena exhausted before the fan-out. Fall back to the serial walk, which
        // needs no scratch and still respects the output bound.
        usize written = 0;
        scene.for_each_node([&](Node& node) {
            if (written < out.size() && resolve_sprite(node, out[written])) {
                ++written;
            }
        });
        return written;
    }

    // Fill the flat list, but never past the capacity: the walk is the source of
    // truth for how many nodes there are, and the chunking uses that count.
    usize n = 0;
    scene.for_each_node([&](Node& node) {
        if (n < capacity) {
            flat[n++] = &node;
        }
    });
    const u32 chunks = static_cast<u32>(std::min<usize>(n, chunk_capacity));

    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = n * c / chunks;
        const usize hi = n * (c + 1) / chunks;
        usize count = 0;
        SpriteInstance scratch;
        for (usize i = lo; i < hi; ++i) {
            if (resolve_sprite(*flat[i], scratch)) {
                ++count;
            }
        }
        counts[c] = count;
    });

    // Clamp to the output bound and take the running offsets serially, so the
    // destination is never overrun even if the caller sized it too small.
    usize total = 0;
    for (u32 c = 0; c < chunks; ++c) {
        offsets[c] = total;
        total += std::min(counts[c], out.size() - total);
    }

    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = n * c / chunks;
        const usize hi = n * (c + 1) / chunks;
        usize write = offsets[c];
        for (usize i = lo; i < hi; ++i) {
            if (write == out.size()) {
                break;
            }
            if (resolve_sprite(*flat[i], out[write])) {
                ++write;
            }
        }
    });

    return total;
}

usize extract_tiles_into(FrameArena& arena, const Scene& scene, std::span<SpriteInstance> out, usize offset,
                         JobSystem* jobs) {
    usize written = offset;
    scene.for_each_node([&](Node& node) {
        const auto* map = node.get_component<Tilemap>();
        if (map == nullptr || map->empty()) {
            return;
        }
        const auto* texture = dynamic_cast<const Texture2D*>(map->texture.get());
        if (texture == nullptr || map->tile_size.x == 0.0f || map->tile_size.y == 0.0f) {
            return;
        }

        const Mat4 node_world = node.world_matrix();
        const usize tile_count = static_cast<usize>(map->width) * map->height;

        if (jobs == nullptr || !jobs->is_threaded() || tile_count < kParallelTileThreshold) {
            for (u32 y = 0; y < map->height && written < out.size(); ++y) {
                for (u32 x = 0; x < map->width && written < out.size(); ++x) {
                    if (resolve_tile(*map, node_world, texture, x, y, out[written])) {
                        ++written;
                    }
                }
            }
            return;
        }

        // Parallel: the same count-then-write shape as the sprite walk, over the
        // map's cells. Offsets are global (they start at `written`), so the tiles
        // land in row-major order after whatever the caller already wrote.
        const u32 chunks = static_cast<u32>(std::min<usize>(tile_count, static_cast<usize>(jobs->worker_count()) * 4));
        auto* counts = static_cast<usize*>(arena.allocate(sizeof(usize) * chunks, alignof(usize)));
        auto* offsets = static_cast<usize*>(arena.allocate(sizeof(usize) * chunks, alignof(usize)));
        if (counts == nullptr || offsets == nullptr) {
            for (usize t = 0; t < tile_count && written < out.size(); ++t) {
                const u32 x = static_cast<u32>(t % map->width);
                const u32 y = static_cast<u32>(t / map->width);
                if (resolve_tile(*map, node_world, texture, x, y, out[written])) {
                    ++written;
                }
            }
            return;
        }

        jobs->parallel_for(0, chunks, [&](usize c) {
            const usize lo = tile_count * c / chunks;
            const usize hi = tile_count * (c + 1) / chunks;
            usize count = 0;
            SpriteInstance scratch;
            for (usize t = lo; t < hi; ++t) {
                const u32 x = static_cast<u32>(t % map->width);
                const u32 y = static_cast<u32>(t / map->width);
                if (resolve_tile(*map, node_world, texture, x, y, scratch)) {
                    ++count;
                }
            }
            counts[c] = count;
        });

        usize total = written;
        for (u32 c = 0; c < chunks; ++c) {
            offsets[c] = total;
            total += std::min(counts[c], out.size() - total);
        }

        jobs->parallel_for(0, chunks, [&](usize c) {
            const usize lo = tile_count * c / chunks;
            const usize hi = tile_count * (c + 1) / chunks;
            usize write = offsets[c];
            for (usize t = lo; t < hi; ++t) {
                if (write == out.size()) {
                    break;
                }
                const u32 x = static_cast<u32>(t % map->width);
                const u32 y = static_cast<u32>(t / map->width);
                if (resolve_tile(*map, node_world, texture, x, y, out[write])) {
                    ++write;
                }
            }
        });

        written = total;
    });
    return written;
}

usize extract_text_into(FrameArena& arena, const Scene& scene, std::span<SpriteInstance> out, usize offset,
                        JobSystem* jobs) {
    // The arena variant needs no scratch of its own: the layout is serial and
    // each glyph is written straight into its final slot, so this walk allocates
    // nothing from the heap and asks the arena for nothing. `arena` and `jobs`
    // are accepted for signature parity with the other walks.
    (void)arena;
    (void)jobs;
    usize written = offset;
    scene.for_each_node([&](Node& node) {
        if (written == out.size()) {
            return;
        }
        const auto* label = node.get_component<TextLabel>();
        if (label == nullptr || label->empty()) {
            return;
        }
        const auto* font = dynamic_cast<const Font*>(label->font.get());
        if (font == nullptr || !font->has_atlas()) {
            return;
        }
        const auto* texture = dynamic_cast<const Texture2D*>(font->atlas().get());
        if (texture == nullptr) {
            return;
        }

        const Mat4 node_world = node.world_matrix();
        f32 pen_x = pen_origin(*label, *font);
        for (const char raw : label->text) {
            if (written == out.size()) {
                return;
            }
            const u8 code = static_cast<u8>(raw);
            if (resolve_glyph(*label, *font, node_world, code, Vec2{pen_x, 0.0f}, texture, out[written])) {
                ++written;
            }
            pen_x += font->advance(code) * label->size;
        }
    });
    return written;
}

usize cull_sprites(std::vector<SpriteInstance>& instances, const Frustum* frustum, JobSystem* jobs) {
    if (frustum == nullptr) {
        return 0;
    }
    const usize before = instances.size();
    if (jobs == nullptr || !jobs->is_threaded() || before < kParallelCullThreshold) {
        std::erase_if(instances, [&](const SpriteInstance& instance) { return !sprite_visible(instance, *frustum); });
        return before - instances.size();
    }

    // Parallel: test visibility into a byte mask, then compact once, serially.
    // The mask is indexed by position, so the survivors keep their original
    // relative order; the compaction is a single sequential pass over the array,
    // which is cheaper than having each worker build and re-join its own list.
    std::vector<u8> keep(before, 0);
    const u32 chunks = static_cast<u32>(std::min<usize>(before, static_cast<usize>(jobs->worker_count()) * 4));
    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = before * c / chunks;
        const usize hi = before * (c + 1) / chunks;
        for (usize i = lo; i < hi; ++i) {
            keep[i] = sprite_visible(instances[i], *frustum) ? 1 : 0;
        }
    });

    usize write = 0;
    for (usize read = 0; read < before; ++read) {
        if (keep[read] != 0) {
            instances[write++] = instances[read];
        }
    }
    instances.resize(write);
    return before - write;
}

usize cull_sprites_in_place(FrameArena& arena, std::span<SpriteInstance> instances, usize used, const Frustum* frustum,
                            JobSystem* jobs) {
    used = std::min(used, instances.size());
    if (frustum == nullptr) {
        return used;
    }

    if (jobs == nullptr || !jobs->is_threaded() || used < kParallelCullThreshold) {
        usize write = 0;
        for (usize read = 0; read < used; ++read) {
            if (sprite_visible(instances[read], *frustum)) {
                if (write != read) {
                    instances[write] = instances[read];
                }
                ++write;
            }
        }
        return write;
    }

    // Parallel: same shape as the vector path, but the mask comes from the arena
    // so the step allocates nothing on the heap.
    auto* keep = static_cast<u8*>(arena.allocate(sizeof(u8) * used, alignof(u8)));
    if (keep == nullptr) {
        // No scratch: fall back to the serial compaction, which needs none.
        usize write = 0;
        for (usize read = 0; read < used; ++read) {
            if (sprite_visible(instances[read], *frustum)) {
                if (write != read) {
                    instances[write] = instances[read];
                }
                ++write;
            }
        }
        return write;
    }

    const u32 chunks = static_cast<u32>(std::min<usize>(used, static_cast<usize>(jobs->worker_count()) * 4));
    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = used * c / chunks;
        const usize hi = used * (c + 1) / chunks;
        for (usize i = lo; i < hi; ++i) {
            keep[i] = sprite_visible(instances[i], *frustum) ? 1 : 0;
        }
    });

    usize write = 0;
    for (usize read = 0; read < used; ++read) {
        if (keep[read] != 0) {
            if (write != read) {
                instances[write] = instances[read];
            }
            ++write;
        }
    }
    return write;
}

usize cull_meshes(std::vector<MeshInstance>& instances, const Frustum* frustum, JobSystem* jobs) {
    if (frustum == nullptr) {
        return 0;
    }
    const usize before = instances.size();
    if (jobs == nullptr || !jobs->is_threaded() || before < kParallelCullThreshold) {
        std::erase_if(instances, [&](const MeshInstance& instance) {
            return !frustum->intersects_aabb(instance.world_min, instance.world_max);
        });
        return before - instances.size();
    }

    std::vector<u8> keep(before, 0);
    const u32 chunks = static_cast<u32>(std::min<usize>(before, static_cast<usize>(jobs->worker_count()) * 4));
    jobs->parallel_for(0, chunks, [&](usize c) {
        const usize lo = before * c / chunks;
        const usize hi = before * (c + 1) / chunks;
        for (usize i = lo; i < hi; ++i) {
            keep[i] = frustum->intersects_aabb(instances[i].world_min, instances[i].world_max) ? 1 : 0;
        }
    });

    usize write = 0;
    for (usize read = 0; read < before; ++read) {
        if (keep[read] != 0) {
            instances[write++] = instances[read];
        }
    }
    instances.resize(write);
    return before - write;
}

} // namespace fe::render
