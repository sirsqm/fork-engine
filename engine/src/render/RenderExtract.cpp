// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/render/RenderExtract.h"

#include "fe/mesh/Mesh.h"
#include "fe/resources/Texture2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
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

} // namespace

void extract_sprites(const Scene& scene, std::vector<SpriteInstance>& out) {
    out.clear();
    scene.for_each_node([&](Node& node) {
        const auto* sprite = node.get_component<Sprite2D>();
        if (sprite == nullptr || !sprite->has_texture()) {
            // An untextured sprite is an authoring state, not content; skip it,
            // matching the pre-seam renderer.
            return;
        }
        const auto* texture = dynamic_cast<const Texture2D*>(sprite->texture.get());
        if (texture == nullptr) {
            return;
        }

        // Resolve the draw size: explicit size, else the texture size.
        Vec2 size = sprite->size;
        if (size.x == 0.0f || size.y == 0.0f) {
            size = Vec2{static_cast<f32>(texture->width()), static_cast<f32>(texture->height())};
        }

        out.push_back(SpriteInstance{&texture->texture(), sprite, node.world_matrix(), size});
    });
}

void extract_meshes(const Scene& scene, std::vector<MeshInstance>& out, std::vector<const Mesh*>* meshes_seen) {
    out.clear();
    scene.for_each_node([&](Node& node) {
        const auto* renderer = node.get_component<MeshRenderer3D>();
        if (renderer == nullptr || !renderer->has_mesh()) {
            return;
        }
        const auto* mesh = dynamic_cast<const Mesh*>(renderer->mesh.get());
        if (mesh == nullptr || mesh->index_count() == 0) {
            return;
        }
        if (meshes_seen != nullptr) {
            meshes_seen->push_back(mesh);
        }

        const Mat4 model = node.world_matrix();
        MeshInstance instance;
        instance.renderer = renderer;
        instance.mesh = mesh;
        instance.model = model;
        expand_to_world_aabb(mesh->bounds_min(), mesh->bounds_max(), model, instance.world_min, instance.world_max);
        out.push_back(instance);
    });
}

usize cull_sprites(std::vector<SpriteInstance>& instances, const Frustum* frustum) {
    if (frustum == nullptr) {
        return 0;
    }
    const usize before = instances.size();
    std::erase_if(instances, [&](const SpriteInstance& instance) {
        // A quad-sized AABB centred on the sprite's world position. Deriving it
        // from the resolved size means a sprite larger than the view is kept.
        const Vec3 centre = transform_point(instance.world, Vec3{});
        const Vec3 half{instance.size.x * 0.5f, instance.size.y * 0.5f, 0.0f};
        return !frustum->intersects_aabb(centre - half, centre + half);
    });
    return before - instances.size();
}

usize cull_meshes(std::vector<MeshInstance>& instances, const Frustum* frustum) {
    if (frustum == nullptr) {
        return 0;
    }
    const usize before = instances.size();
    std::erase_if(instances, [&](const MeshInstance& instance) {
        return !frustum->intersects_aabb(instance.world_min, instance.world_max);
    });
    return before - instances.size();
}

} // namespace fe::render
