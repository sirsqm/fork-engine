// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/Viewport.h"

#include "fe/resources/Texture2D.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"

#include <algorithm>
#include <cmath>

namespace fe::editor {
namespace {

// Zoom is multiplicative per wheel notch; the factor is applied as
// `factor ^ scroll` so a fast scroll zooms proportionally rather than linearly.
constexpr f32 kZoomPerNotch = 1.15f;
constexpr f32 kMinZoom = 0.05f;
constexpr f32 kMaxZoom = 64.0f;

// The visible world half-extents for a camera in a viewport of the given size.
// Mirrors Camera2D::view_projection so picking and rendering agree: the visible
// box spans viewport_size / zoom world units, centred on the camera node.
[[nodiscard]] Vec2 half_extent_for(const Camera2D& camera, u32 width, u32 height) noexcept {
    const f32 zoom = camera.zoom > 0.0f ? camera.zoom : 1.0f;
    return Vec2{static_cast<f32>(width) * 0.5f / zoom, static_cast<f32>(height) * 0.5f / zoom};
}

// The camera centre in world space: the camera node's world translation, or the
// origin for a detached camera (matching Camera2D::view_projection).
[[nodiscard]] Vec2 camera_centre(const Camera2D& camera) noexcept {
    if (camera.node() == nullptr) {
        return Vec2{0.0f, 0.0f};
    }
    const Vec3 centre = transform_point(camera.node()->world_matrix(), Vec3{});
    return Vec2{centre.x, centre.y};
}

} // namespace

void Viewport::set_size(u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
}

void Viewport::world_view(const Camera2D& camera, Vec2& out_centre, Vec2& out_half_extent) const noexcept {
    out_centre = camera_centre(camera);
    out_half_extent = half_extent_for(camera, width_, height_);
}

Vec2 Viewport::screen_to_world(const Camera2D& camera, const Vec2& screen_point) const noexcept {
    if (width_ == 0 || height_ == 0) {
        return Vec2{0.0f, 0.0f};
    }
    Vec2 centre;
    Vec2 half;
    world_view(camera, centre, half);
    const f32 zoom = camera.zoom > 0.0f ? camera.zoom : 1.0f;

    // Screen X grows right and world X grows right; screen Y grows down and
    // world Y grows up, so the vertical offset flips sign. This is the inverse
    // of Camera2D's orthographic mapping.
    const f32 world_x = centre.x + (screen_point.x - static_cast<f32>(width_) * 0.5f) / zoom;
    const f32 world_y = centre.y - (screen_point.y - static_cast<f32>(height_) * 0.5f) / zoom;
    return Vec2{world_x, world_y};
}

void Viewport::pan_by_pixels(Camera2D& camera, const Vec2& mouse_delta) noexcept {
    if (camera.node() == nullptr) {
        return;
    }
    const f32 zoom = camera.zoom > 0.0f ? camera.zoom : 1.0f;
    Transform& transform = camera.node()->transform();
    // Move the camera opposite to the drag so the world tracks the cursor.
    transform.position.x -= mouse_delta.x / zoom;
    transform.position.y += mouse_delta.y / zoom;
}

void Viewport::zoom_at_cursor(Camera2D& camera, const Vec2& cursor, f32 scroll) noexcept {
    if (camera.node() == nullptr || scroll == 0.0f || width_ == 0 || height_ == 0) {
        return;
    }

    // The world point under the cursor before the zoom, from the *old* zoom.
    const Vec2 anchor_world = screen_to_world(camera, cursor);

    const f32 new_zoom = std::clamp(camera.zoom * std::pow(kZoomPerNotch, scroll), kMinZoom, kMaxZoom);

    // Solve for the camera centre that puts `anchor_world` back under the
    // cursor at the new zoom, so the view does not jump.
    Transform& transform = camera.node()->transform();
    transform.position.x = anchor_world.x - (cursor.x - static_cast<f32>(width_) * 0.5f) / new_zoom;
    transform.position.y = anchor_world.y + (cursor.y - static_cast<f32>(height_) * 0.5f) / new_zoom;
    camera.zoom = new_zoom;
}

bool Viewport::node_world_bounds(const Node& node, Vec2& out_min, Vec2& out_max) noexcept {
    const auto* sprite = node.get_component<Sprite2D>();
    if (sprite == nullptr) {
        return false;
    }

    // Resolve the quad size the same way the renderer does: explicit size wins,
    // else the texture's pixel size. A sprite with neither has no extent to pick.
    Vec2 size = sprite->size;
    if (size.x == 0.0f || size.y == 0.0f) {
        const auto* texture = dynamic_cast<const Texture2D*>(sprite->texture.get());
        if (texture == nullptr) {
            return false;
        }
        size = Vec2{static_cast<f32>(texture->width()), static_cast<f32>(texture->height())};
    }

    const Vec2 half = size * 0.5f;
    const Mat4 world = node.world_matrix();
    // The sprite is drawn as an axis-aligned quad in local space but a node may
    // be rotated/scaled, so transform all four corners and take their bounds.
    const Vec2 corners[4] = {
        Vec2{-half.x, -half.y},
        Vec2{half.x, -half.y},
        Vec2{half.x, half.y},
        Vec2{-half.x, half.y},
    };

    out_min = Vec2{0.0f, 0.0f};
    out_max = Vec2{0.0f, 0.0f};
    for (usize i = 0; i < 4; ++i) {
        const Vec3 world_corner = transform_point(world, Vec3{corners[i].x, corners[i].y, 0.0f});
        const Vec2 point{world_corner.x, world_corner.y};
        if (i == 0) {
            out_min = point;
            out_max = point;
            continue;
        }
        out_min.x = std::min(out_min.x, point.x);
        out_min.y = std::min(out_min.y, point.y);
        out_max.x = std::max(out_max.x, point.x);
        out_max.y = std::max(out_max.y, point.y);
    }
    return true;
}

bool Viewport::node_contains_world_point(const Node& node, const Vec2& world_point) noexcept {
    Vec2 min;
    Vec2 max;
    if (!node_world_bounds(node, min, max)) {
        return false;
    }
    return world_point.x >= min.x && world_point.x <= max.x && world_point.y >= min.y && world_point.y <= max.y;
}

Node* Viewport::pick(const Scene& scene, const Camera2D& camera, const Vec2& screen_point) const noexcept {
    if (width_ == 0 || height_ == 0) {
        return nullptr;
    }
    const Vec2 world = screen_to_world(camera, screen_point);

    Node* best = nullptr;
    f32 best_area = 0.0f;
    scene.for_each_node([&](Node& node) {
        Vec2 min;
        Vec2 max;
        if (!node_world_bounds(node, min, max)) {
            return;
        }
        if (world.x < min.x || world.x > max.x || world.y < min.y || world.y > max.y) {
            return;
        }
        const f32 area = (max.x - min.x) * (max.y - min.y);
        // Smallest containing quad wins, so a small sprite stacked over a large
        // backdrop is selectable rather than shadowed by it.
        if (best == nullptr || area < best_area) {
            best = &node;
            best_area = area;
        }
    });
    return best;
}

bool Viewport::pick_bounds(const Scene& scene, const Camera2D& camera, const Vec2& screen_point, Vec2& out_min,
                           Vec2& out_max) const noexcept {
    Node* node = pick(scene, camera, screen_point);
    if (node == nullptr) {
        return false;
    }
    return node_world_bounds(*node, out_min, out_max);
}

void Viewport::world_bounds_to_screen(const Camera2D& camera, const Vec2& min, const Vec2& max,
                                      std::span<Vec2> out_quad) const noexcept {
    if (out_quad.size() < 4) {
        return;
    }
    Vec2 centre;
    Vec2 half;
    world_view(camera, centre, half);
    const f32 zoom = camera.zoom > 0.0f ? camera.zoom : 1.0f;

    const auto to_screen = [&](const Vec2& world) {
        const f32 sx = (world.x - centre.x) * zoom + static_cast<f32>(width_) * 0.5f;
        const f32 sy = static_cast<f32>(height_) * 0.5f - (world.y - centre.y) * zoom;
        return Vec2{sx, sy};
    };

    out_quad[0] = to_screen(Vec2{min.x, min.y});
    out_quad[1] = to_screen(Vec2{max.x, min.y});
    out_quad[2] = to_screen(Vec2{max.x, max.y});
    out_quad[3] = to_screen(Vec2{min.x, max.y});
}

} // namespace fe::editor
