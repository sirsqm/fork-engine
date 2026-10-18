// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/render/RenderExtract.h"

#include "fe/jobs/JobSystem.h"
#include "fe/mesh/Mesh.h"
#include "fe/resources/Texture2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
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

// Appends the instance for cell (x, y) of `map` to `out`, or nothing when the
// cell is empty. Shared by the serial and parallel tile walks so the two cannot
// disagree on the cell's world matrix or UV.
void append_tile(const Tilemap& map, const Mat4& node_world, const Texture2D* texture, u32 x, u32 y,
                 std::vector<SpriteInstance>& out) {
    const i32 tile = map.at(x, y);
    if (tile == Tilemap::kEmpty) {
        return;
    }

    // Cell (0, 0) is the map's top-left, +X right and +Y down, so the map spans
    // x in [0, width * tile.x] and y in [-height * tile.y, 0] in the node's local
    // space. Centring each tile within its cell puts the cell's centre at
    // (x + 0.5) tiles across.
    const f32 local_x = (static_cast<f32>(x) + 0.5f) * map.tile_size.x;
    const f32 local_y = -(static_cast<f32>(y) + 0.5f) * map.tile_size.y;

    SpriteInstance instance;
    instance.texture = &texture->texture();
    instance.tilemap = &map;
    instance.world = node_world * Mat4::translation(Vec3{local_x, local_y, 0.0f});
    instance.size = map.tile_size;
    instance.uv = map.tile_uv(tile);
    instance.color = map.color;
    instance.layer = map.layer;
    out.push_back(instance);
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

usize cull_sprites(std::vector<SpriteInstance>& instances, const Frustum* frustum, JobSystem* jobs) {
    if (frustum == nullptr) {
        return 0;
    }
    const usize before = instances.size();
    if (jobs == nullptr || !jobs->is_threaded() || before < kParallelCullThreshold) {
        std::erase_if(instances, [&](const SpriteInstance& instance) {
            // A quad-sized AABB centred on the sprite's world position. Deriving
            // it from the resolved size means a sprite larger than the view is
            // kept.
            const Vec3 centre = transform_point(instance.world, Vec3{});
            const Vec3 half{instance.size.x * 0.5f, instance.size.y * 0.5f, 0.0f};
            return !frustum->intersects_aabb(centre - half, centre + half);
        });
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
            const SpriteInstance& instance = instances[i];
            const Vec3 centre = transform_point(instance.world, Vec3{});
            const Vec3 half{instance.size.x * 0.5f, instance.size.y * 0.5f, 0.0f};
            keep[i] = frustum->intersects_aabb(centre - half, centre + half) ? 1 : 0;
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
