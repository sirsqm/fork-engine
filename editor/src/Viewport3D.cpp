// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/Viewport3D.h"

#include "fe/anim/SkinnedMesh.h"
#include "fe/anim/SkinnedMeshRenderer3D.h"
#include "fe/mesh/Mesh.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fe::editor {
namespace {

// Yaw/pitch are per pixel of drag; pitch is clamped just short of the poles so
// the camera's up vector never becomes parallel to its forward (which would make
// the view matrix degenerate).
constexpr f32 kOrbitRadiansPerPixel = 0.01f;
constexpr f32 kPitchLimit = 1.55334f; // ~89 degrees, in radians.

// Dolly is multiplicative per wheel notch, like the 2D viewport's zoom, so a
// fast scroll changes the distance proportionally rather than linearly.
constexpr f32 kDollyPerNotch = 1.15f;
constexpr f32 kMinDistance = 0.05f;
constexpr f32 kMaxDistance = 1.0e5f;

// The margin left around a framed box, so "focus" puts the object in view with a
// little breathing room rather than touching the edges of the frame.
constexpr f32 kFrameMargin = 1.25f;

// Transforms a homogeneous point by a 4x4 matrix, keeping the w component. The
// projection is the one place w is not 1 after a transform, so the perspective
// divide needs it; `transform_point` drops it.
Vec4 transform_vec4(const Mat4& mat, const Vec4& v) noexcept {
    return Vec4{
        mat.at(0, 0) * v.x + mat.at(0, 1) * v.y + mat.at(0, 2) * v.z + mat.at(0, 3) * v.w,
        mat.at(1, 0) * v.x + mat.at(1, 1) * v.y + mat.at(1, 2) * v.z + mat.at(1, 3) * v.w,
        mat.at(2, 0) * v.x + mat.at(2, 1) * v.y + mat.at(2, 2) * v.z + mat.at(2, 3) * v.w,
        mat.at(3, 0) * v.x + mat.at(3, 1) * v.y + mat.at(3, 2) * v.z + mat.at(3, 3) * v.w,
    };
}

// Expands an AABB to include a point.
void include_point(Vec3& box_min, Vec3& box_max, const Vec3& point) noexcept {
    box_min.x = std::min(box_min.x, point.x);
    box_min.y = std::min(box_min.y, point.y);
    box_min.z = std::min(box_min.z, point.z);
    box_max.x = std::max(box_max.x, point.x);
    box_max.y = std::max(box_max.y, point.y);
    box_max.z = std::max(box_max.z, point.z);
}

// The world-space AABB of a local box under a node's world matrix, by
// transforming all eight corners. A rotated node's world AABB is larger than its
// local box, which is exactly why the corners are transformed rather than the
// box's min/max.
void transform_bounds(const Mat4& world, const Vec3& local_min, const Vec3& local_max, Vec3& out_min,
                      Vec3& out_max) noexcept {
    const Vec3 corners[8] = {
        Vec3{local_min.x, local_min.y, local_min.z}, Vec3{local_max.x, local_min.y, local_min.z},
        Vec3{local_max.x, local_max.y, local_min.z}, Vec3{local_min.x, local_max.y, local_min.z},
        Vec3{local_min.x, local_min.y, local_max.z}, Vec3{local_max.x, local_min.y, local_max.z},
        Vec3{local_max.x, local_max.y, local_max.z}, Vec3{local_min.x, local_max.y, local_max.z},
    };
    out_min = transform_point(world, corners[0]);
    out_max = out_min;
    for (usize i = 1; i < 8; ++i) {
        include_point(out_min, out_max, transform_point(world, corners[i]));
    }
}

} // namespace

void Viewport3D::set_size(u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
}

Vec3 Viewport3D::eye() const noexcept {
    // The rig's eye sits `distance` from the pivot, opposite the direction the
    // camera looks, so applying the rig and reading `Camera3D::eye` agree.
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    basis(forward, right, up);
    return orbit_.pivot - forward * orbit_.distance;
}

void Viewport3D::basis(Vec3& out_forward, Vec3& out_right, Vec3& out_up) const noexcept {
    // Yaw about world +Y, then pitch about the resulting right axis. The camera
    // looks down its local -Z, so the forward direction the rig faces is the
    // negative of the basis' Z column; building it from yaw/pitch directly keeps
    // the three axes consistent with what `apply_to` writes into the transform.
    const f32 cy = std::cos(orbit_.yaw);
    const f32 sy = std::sin(orbit_.yaw);
    const f32 cp = std::cos(orbit_.pitch);
    const f32 sp = std::sin(orbit_.pitch);

    // Forward: -Z rotated by pitch (about X) then yaw (about Y).
    out_forward = Vec3{-sy * cp, sp, -cy * cp};
    // Right is world +X rotated by yaw.
    out_right = Vec3{cy, 0.0f, -sy};
    // Up completes the right-handed frame.
    out_up = out_right.cross(out_forward).normalized();
}

void Viewport3D::sync_from_camera(const Camera3D& camera) noexcept {
    const Node* owner = camera.node();
    if (owner == nullptr) {
        return;
    }
    // Recover yaw and pitch from the camera's forward direction, matching the
    // forward the basis builds, so applying the rig back reproduces the camera's
    // orientation. The distance is preserved (a camera has no notion of one), so
    // the pivot lands that far in front of the eye.
    const Vec3 forward = camera.forward();
    orbit_.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    orbit_.yaw = std::atan2(-forward.x, -forward.z);
    const Vec3 eye_position = camera.eye();
    orbit_.pivot = eye_position + forward * orbit_.distance;
}

void Viewport3D::apply_to(Camera3D& camera) const noexcept {
    Node* owner = camera.node();
    if (owner == nullptr) {
        return;
    }
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    basis(forward, right, up);

    // The camera looks down its local -Z, so the node's rotation is the basis
    // with Z = -forward. Building the rotation from a matrix and decomposing it
    // keeps the roll at zero (up is derived from the basis), which is what an
    // orbit camera wants.
    Mat4 rotation = Mat4::identity();
    rotation.at(0, 0) = right.x;
    rotation.at(1, 0) = right.y;
    rotation.at(2, 0) = right.z;
    rotation.at(0, 1) = up.x;
    rotation.at(1, 1) = up.y;
    rotation.at(2, 1) = up.z;
    rotation.at(0, 2) = -forward.x;
    rotation.at(1, 2) = -forward.y;
    rotation.at(2, 2) = -forward.z;

    Transform& transform = owner->transform();
    transform.position = eye();
    transform.rotation = Transform::from_matrix(rotation).rotation;
}

void Viewport3D::orbit_by_pixels(const Vec3& mouse_delta) noexcept {
    orbit_.yaw -= mouse_delta.x * kOrbitRadiansPerPixel;
    orbit_.pitch += mouse_delta.y * kOrbitRadiansPerPixel;
    orbit_.pitch = std::clamp(orbit_.pitch, -kPitchLimit, kPitchLimit);
}

void Viewport3D::pan_by_pixels(const Camera3D& camera, const Vec2& mouse_delta) noexcept {
    (void)camera;
    if (height_ == 0) {
        return;
    }
    // Scale the pixel drag by the world size of a pixel at the pivot's depth, so
    // the world tracks the cursor regardless of distance or field of view. The
    // pixel-to-world factor comes from the vertical FOV and the view height.
    Vec3 forward;
    Vec3 right;
    Vec3 up;
    basis(forward, right, up);

    const f32 fov = radians(camera.fov_degrees);
    const f32 world_per_pixel = 2.0f * orbit_.distance * std::tan(fov * 0.5f) / static_cast<f32>(height_);
    const Vec3 offset = right * (-mouse_delta.x * world_per_pixel) + up * (mouse_delta.y * world_per_pixel);
    orbit_.pivot += offset;
}

void Viewport3D::dolly_by_scroll(f32 scroll) noexcept {
    if (scroll == 0.0f) {
        return;
    }
    orbit_.distance = std::clamp(orbit_.distance / std::pow(kDollyPerNotch, scroll), kMinDistance, kMaxDistance);
}

void Viewport3D::frame_bounds(const Vec3& box_min, const Vec3& box_max, f32 fov_degrees) noexcept {
    const Vec3 centre = (box_min + box_max) * 0.5f;
    const Vec3 half = (box_max - box_min) * 0.5f;
    const f32 radius = half.length();
    const f32 fov = radians(fov_degrees > 1.0f ? fov_degrees : 60.0f);
    // Distance so a sphere of `radius` fits the vertical field of view, with the
    // margin, and never below the minimum (a degenerate box has radius 0).
    const f32 needed = (radius > 0.0f ? radius : 1.0f) * kFrameMargin / std::tan(fov * 0.5f);
    orbit_.pivot = centre;
    orbit_.distance = std::clamp(needed, kMinDistance, kMaxDistance);
}

bool Viewport3D::node_world_bounds(const Node& node, Vec3& out_min, Vec3& out_max) noexcept {
    const Mat4 world = node.world_matrix();

    // A skinned renderer's mesh is the character's content; a static renderer's
    // is its geometry. A node with neither has no extent, so it is neither
    // framable nor pickable - which is what keeps the active camera, a bare
    // transform, and a light out of the way of a pick.
    const auto* skinned = node.get_component<anim::SkinnedMeshRenderer3D>();
    if (skinned != nullptr) {
        if (const anim::SkinnedMesh* mesh = skinned->skinned_mesh(); mesh != nullptr) {
            transform_bounds(world, mesh->bounds_min(), mesh->bounds_max(), out_min, out_max);
            return true;
        }
    }

    const auto* mesh_renderer = node.get_component<MeshRenderer3D>();
    if (mesh_renderer != nullptr) {
        if (const auto* mesh = dynamic_cast<const Mesh*>(mesh_renderer->mesh.get()); mesh != nullptr) {
            transform_bounds(world, mesh->bounds_min(), mesh->bounds_max(), out_min, out_max);
            return true;
        }
    }

    return false;
}

bool Viewport3D::scene_world_bounds(const Scene& scene, Vec3& out_min, Vec3& out_max) noexcept {
    bool any = false;
    scene.for_each_node([&](Node& node) {
        Vec3 node_min;
        Vec3 node_max;
        if (!node_world_bounds(node, node_min, node_max)) {
            return;
        }
        if (!any) {
            out_min = node_min;
            out_max = node_max;
            any = true;
            return;
        }
        include_point(out_min, out_max, node_min);
        include_point(out_min, out_max, node_max);
    });
    return any;
}

bool Viewport3D::ray_intersects_aabb(const Vec3& origin, const Vec3& direction, const Vec3& box_min,
                                     const Vec3& box_max, f32& out_t) noexcept {
    f32 t_near = -std::numeric_limits<f32>::infinity();
    f32 t_far = std::numeric_limits<f32>::infinity();
    const f32 origin_axis[3] = {origin.x, origin.y, origin.z};
    const f32 dir_axis[3] = {direction.x, direction.y, direction.z};
    const f32 min_axis[3] = {box_min.x, box_min.y, box_min.z};
    const f32 max_axis[3] = {box_max.x, box_max.y, box_max.z};

    for (usize i = 0; i < 3; ++i) {
        if (std::abs(dir_axis[i]) < 1e-8f) {
            // The ray is parallel to this slab: a miss unless the origin is
            // already between the two planes.
            if (origin_axis[i] < min_axis[i] || origin_axis[i] > max_axis[i]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.0f / dir_axis[i];
        f32 t1 = (min_axis[i] - origin_axis[i]) * inv;
        f32 t2 = (max_axis[i] - origin_axis[i]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_near = std::max(t_near, t1);
        t_far = std::min(t_far, t2);
        if (t_near > t_far) {
            return false;
        }
    }

    // The box is behind the ray when the exit is negative; the ray starts inside
    // when the entry is negative but the exit is not, in which case the hit is at
    // the origin (t = 0).
    if (t_far < 0.0f) {
        return false;
    }
    out_t = t_near > 0.0f ? t_near : 0.0f;
    return true;
}

bool Viewport3D::screen_ray(const Camera3D& camera, const Vec2& screen_point, Vec3& out_origin,
                            Vec3& out_direction) const noexcept {
    if (width_ == 0 || height_ == 0) {
        return false;
    }
    // Unproject the near and far points through the inverse view-projection and
    // take the ray between them. This is the exact inverse of what the renderer
    // does, so a pick lands where the geometry is drawn.
    const Mat4 view_projection = camera.view_projection(width_, height_);
    const Mat4 inverse_vp = inverse(view_projection);

    // Screen to normalised device coordinates. Screen Y grows down and NDC Y
    // grows up, so the vertical term flips.
    const f32 ndc_x = 2.0f * screen_point.x / static_cast<f32>(width_) - 1.0f;
    const f32 ndc_y = 1.0f - 2.0f * screen_point.y / static_cast<f32>(height_);

    const auto unproject = [&](f32 z) {
        const Vec4 world = transform_vec4(inverse_vp, Vec4{ndc_x, ndc_y, z, 1.0f});
        if (std::abs(world.w) < 1e-8f) {
            return Vec3{};
        }
        return Vec3{world.x / world.w, world.y / world.w, world.z / world.w};
    };

    const Vec3 near_point = unproject(-1.0f);
    const Vec3 far_point = unproject(1.0f);
    const Vec3 direction = (far_point - near_point).normalized();
    if (direction.length_squared() <= 0.0f) {
        return false;
    }
    out_origin = near_point;
    out_direction = direction;
    return true;
}

Node* Viewport3D::pick(const Scene& scene, const Camera3D& camera, const Vec2& screen_point) const noexcept {
    Vec3 origin;
    Vec3 direction;
    if (!screen_ray(camera, screen_point, origin, direction)) {
        return nullptr;
    }

    Node* best = nullptr;
    f32 best_t = std::numeric_limits<f32>::infinity();
    scene.for_each_node([&](Node& node) {
        Vec3 box_min;
        Vec3 box_max;
        if (!node_world_bounds(node, box_min, box_max)) {
            return;
        }
        f32 t = 0.0f;
        if (!ray_intersects_aabb(origin, direction, box_min, box_max, t)) {
            return;
        }
        // Nearest hit wins, so a small object in front of a large one is picked.
        if (best == nullptr || t < best_t) {
            best = &node;
            best_t = t;
        }
    });
    return best;
}

bool Viewport3D::world_to_screen(const Camera3D& camera, const Vec3& world, Vec2& out_screen) const noexcept {
    if (width_ == 0 || height_ == 0) {
        return false;
    }
    const Mat4 view_projection = camera.view_projection(width_, height_);
    const Vec4 clip = transform_vec4(view_projection, Vec4{world.x, world.y, world.z, 1.0f});
    if (clip.w <= 1e-6f) {
        // Behind the camera: the projection is mirrored through the origin, so
        // drawing it would put the point in the wrong place. The caller skips it.
        return false;
    }
    const f32 ndc_x = clip.x / clip.w;
    const f32 ndc_y = clip.y / clip.w;
    out_screen.x = (ndc_x + 1.0f) * 0.5f * static_cast<f32>(width_);
    out_screen.y = (1.0f - ndc_y) * 0.5f * static_cast<f32>(height_);
    return true;
}

bool Viewport3D::screen_to_plane(const Camera3D& camera, const Vec2& screen_point, const Vec3& plane_point,
                                 const Vec3& plane_normal, Vec3& out_world) const noexcept {
    Vec3 origin;
    Vec3 direction;
    if (!screen_ray(camera, screen_point, origin, direction)) {
        return false;
    }
    const Vec3 normal = plane_normal.normalized();
    const f32 denominator = normal.dot(direction);
    if (std::abs(denominator) < 1e-6f) {
        return false; // The ray runs along the plane; there is no single hit.
    }
    const f32 t = normal.dot(plane_point - origin) / denominator;
    if (t < 0.0f) {
        return false; // The plane is behind the camera.
    }
    out_world = origin + direction * t;
    return true;
}

void Viewport3D::aabb_corners(const Vec3& box_min, const Vec3& box_max, std::span<Vec3> out) noexcept {
    if (out.size() < 8) {
        return;
    }
    out[0] = Vec3{box_min.x, box_min.y, box_min.z};
    out[1] = Vec3{box_max.x, box_min.y, box_min.z};
    out[2] = Vec3{box_max.x, box_max.y, box_min.z};
    out[3] = Vec3{box_min.x, box_max.y, box_min.z};
    out[4] = Vec3{box_min.x, box_min.y, box_max.z};
    out[5] = Vec3{box_max.x, box_min.y, box_max.z};
    out[6] = Vec3{box_max.x, box_max.y, box_max.z};
    out[7] = Vec3{box_min.x, box_max.y, box_max.z};
}

} // namespace fe::editor
