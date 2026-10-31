// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ViewportPanel.h"

#include "fe/core/Log.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/OperationCommand.h"
#include "fe/editor/PlaySession.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Transform.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/serialization/Operation.h"

#include "imgui.h"

#include <array>
#include <cmath>

namespace fe::editor {
namespace {

// The window background the viewport is composited over. A near-black so the
// scene reads against it; the scene's own camera clear is used inside the
// target.
constexpr rhi::ClearColor kWindowClear{0.06f, 0.06f, 0.07f, 1.0f};

// The selection outline colour (amber), deliberately distinct from any panel
// chrome so it is unambiguous which node is selected.
constexpr ImU32 kSelectionColor = IM_COL32(255, 176, 32, 255);

// How far the cursor must travel before a left press becomes a move rather than
// a click. A few pixels, so a pick with a slightly unsteady hand still picks.
constexpr f32 kDragThresholdPixels = 3.0f;

// The clear colour the scene's active 2D camera asks for, or the window clear
// when the scene has no 2D camera. Shared with the runtime's own policy.
[[nodiscard]] rhi::ClearColor scene_clear_2d(const Scene& scene) noexcept {
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(scene); camera != nullptr) {
        return rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }
    return kWindowClear;
}

// The clear colour the scene's active 3D camera asks for, or the window clear.
[[nodiscard]] rhi::ClearColor scene_clear_3d(const Scene& scene) noexcept {
    if (const Camera3D* camera = renderer3d::Renderer3D::find_active_camera(scene); camera != nullptr) {
        return rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }
    return kWindowClear;
}

// The scene's active 3D camera as a mutable pointer, so the viewport rig can
// write its transform. `Renderer3D::find_active_camera` returns a const pointer
// (the renderer only reads it); the editor owns the scene and needs to move the
// camera, so it resolves the same camera by the same rule (first primary, else
// first found) but through the mutable `Node::get_component`.
[[nodiscard]] Camera3D* find_editable_camera(Scene& scene) noexcept {
    Camera3D* first = nullptr;
    Camera3D* first_primary = nullptr;
    scene.for_each_node([&](Node& node) {
        Camera3D* camera = node.get_component<Camera3D>();
        if (camera == nullptr) {
            return;
        }
        if (first == nullptr) {
            first = camera;
        }
        if (first_primary == nullptr && camera->primary) {
            first_primary = camera;
        }
    });
    return first_primary != nullptr ? first_primary : first;
}

// Resolves the context's mode against the scene: `Auto` is 3D when the scene has
// a 3D camera, otherwise 2D.
[[nodiscard]] bool wants_3d(ViewportMode mode, const Scene& scene) noexcept {
    switch (mode) {
    case ViewportMode::ThreeD:
        return true;
    case ViewportMode::TwoD:
        return false;
    case ViewportMode::Auto:
        break;
    }
    return renderer3d::Renderer3D::find_active_camera(scene) != nullptr;
}

// The twelve edges of an AABB as pairs of corner indices, for drawing a
// world-space selection box as projected line segments.
constexpr int kBoxEdges[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, // bottom face
    {4, 5}, {5, 6}, {6, 7}, {7, 4}, // top face
    {0, 4}, {1, 5}, {2, 6}, {3, 7}, // verticals
};

} // namespace

ViewportPanel::~ViewportPanel() {
    // Release the target here as well as in shutdown(), because a panel can be
    // destroyed without an explicit shutdown when the UI tears down. The target
    // only exists when a device was bound, which implies shutdown() semantics.
    target_.reset();
}

void ViewportPanel::initialize(rhi::Device& device, renderer2d::Renderer2D& renderer,
                               renderer3d::Renderer3D* renderer_3d) {
    device_ = &device;
    renderer_ = &renderer;
    renderer_3d_ = renderer_3d;
}

void ViewportPanel::shutdown() {
    target_.reset();
    target_width_ = 0;
    target_height_ = 0;
    target_has_depth_ = false;
}

bool ViewportPanel::ensure_target(u32 width, u32 height, bool depth) {
    if (device_ == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (target_ != nullptr && target_width_ == width && target_height_ == height && target_has_depth_ == depth) {
        return true;
    }

    rhi::RenderTargetDesc desc;
    desc.name = "editor.viewport";
    desc.width = width;
    desc.height = height;
    // The 3D pass depth-tests, so it needs a depth buffer; the 2D pass has no
    // depth to test and skips the allocation. The depth buffer is a plain
    // renderbuffer (not sampleable): the panel only presents the colour texture,
    // so nothing reads the depth back.
    desc.depth = depth;
    auto created = device_->create_render_target(desc);
    if (!created) {
        FE_LOG_ERROR("editor: viewport target creation failed: %s", created.error().message().c_str());
        target_.reset();
        target_width_ = 0;
        target_height_ = 0;
        target_has_depth_ = false;
        return false;
    }
    target_ = created.value();
    target_width_ = width;
    target_height_ = height;
    target_has_depth_ = depth;
    return true;
}

void ViewportPanel::draw_2d(EditorContext& context, const Scene& display_scene, bool playing, const Vec2& avail,
                            const Vec2& image_origin) {
    viewport_.set_size(target_width_, target_height_);

    // Render the scene into the off-screen target, then return to the window.
    // Binding the target sets the viewport to its size; binding nullptr restores
    // the window's, so the ImGui draw below is not offset.
    device_->bind_render_target(target_.get());
    target_->clear(scene_clear_2d(display_scene));
    auto rendered = renderer_->render(*device_, display_scene, target_width_, target_height_);
    device_->bind_render_target(nullptr);
    if (!rendered) {
        FE_LOG_ERROR("editor: viewport render failed: %s", rendered.error().message().c_str());
    }

    // --- Present the target as an ImGui image --------------------------
    const auto texture_id = static_cast<ImTextureID>(target_->color_texture()->native_handle());
    // GL textures have their origin at the bottom row, ImGui's at the top, so
    // the V axis is flipped to right the image.
    ImGui::Image(ImTextureRef{texture_id}, ImVec2(avail.x, avail.y), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

    const bool image_hovered = ImGui::IsItemHovered();

    // --- Camera pan and zoom (authored scene only) ---------------------
    if (!playing && image_hovered) {
        const ImGuiIO& io = ImGui::GetIO();
        Camera2D* camera = renderer2d::Renderer2D::find_active_camera(context.scene());
        if (camera != nullptr) {
            // Middle-drag pans; the world follows the cursor.
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                const ImVec2 delta = io.MouseDelta;
                viewport_.pan_by_pixels(*camera, Vec2{delta.x, delta.y});
            }
            if (io.MouseWheel != 0.0f) {
                const Vec2 cursor{io.MousePos.x - image_origin.x, io.MousePos.y - image_origin.y};
                viewport_.zoom_at_cursor(*camera, cursor, io.MouseWheel);
            }
        }
    }

    // --- Selection and drag to move -------------------------------------
    // A left press arms a gesture. It becomes a move once the cursor travels
    // past a small threshold, so a plain click still picks a node and a click
    // on empty space clears the selection - and a click never writes a
    // no-op position edit into the undo history.
    Camera2D* active_camera = renderer2d::Renderer2D::find_active_camera(context.scene());
    if (!playing && active_camera != nullptr) {
        const ImGuiIO& io = ImGui::GetIO();

        if (image_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            armed_ = true;
            moved_ = false;
            press_pos_ = Vec2{io.MousePos.x, io.MousePos.y};
        }

        if (armed_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // The button came up: a press that never moved is a pick.
                if (!moved_ && image_hovered) {
                    const Vec2 cursor{io.MousePos.x - image_origin.x, io.MousePos.y - image_origin.y};
                    context.hierarchy().select(viewport_.pick(context.scene(), *active_camera, cursor));
                }
                armed_ = false;
                // The gesture ended, so the next edit is a new undo step.
                context.break_undo_coalescing();
            } else {
                const Vec2 now{io.MousePos.x, io.MousePos.y};
                const Vec2 travel{now.x - press_pos_.x, now.y - press_pos_.y};
                if (!moved_ && std::sqrt(travel.x * travel.x + travel.y * travel.y) > kDragThresholdPixels) {
                    // The gesture is now a move. Capture where it started, so
                    // the node keeps the offset it was grabbed by and the
                    // gesture accumulates no rounding.
                    moved_ = true;
                    if (Node* selected = context.hierarchy().selected_node(); selected != nullptr) {
                        const Vec3 world = transform_point(selected->world_matrix(), Vec3{});
                        drag_origin_ = Vec2{world.x, world.y};
                        const Vec2 cursor{press_pos_.x - image_origin.x, press_pos_.y - image_origin.y};
                        drag_start_world_ = viewport_.screen_to_world(*active_camera, cursor);
                    }
                }

                if (moved_ && context.is_editable()) {
                    if (Node* selected = context.hierarchy().selected_node(); selected != nullptr) {
                        const Vec2 cursor{now.x - image_origin.x, now.y - image_origin.y};
                        const Vec2 target = viewport_.screen_to_world(*active_camera, cursor);
                        const Vec2 moved_pos = Vec2{drag_origin_.x + (target.x - drag_start_world_.x),
                                                    drag_origin_.y + (target.y - drag_start_world_.y)};

                        const Transform& transform = selected->transform();
                        serialization::Operation op;
                        op.kind = serialization::OperationKind::SetTransform;
                        op.node_id = selected->id();
                        op.path = {"position"};
                        op.transform_value = Vec3{moved_pos.x, moved_pos.y, transform.position.z};
                        context.execute(std::make_unique<OperationCommand>(std::move(op)));
                    }
                }
            }
        }
    }

    // --- Selection outline --------------------------------------------
    if (!playing && context.hierarchy().has_selection()) {
        const Node* selected = context.hierarchy().selected_node();
        Camera2D* camera = renderer2d::Renderer2D::find_active_camera(context.scene());
        if (selected != nullptr && camera != nullptr) {
            Vec2 min;
            Vec2 max;
            if (Viewport::node_world_bounds(*selected, min, max)) {
                std::array<Vec2, 4> quad{};
                viewport_.world_bounds_to_screen(*camera, min, max, quad);
                ImVec2 points[4];
                for (usize i = 0; i < 4; ++i) {
                    points[i] = ImVec2(image_origin.x + quad[i].x, image_origin.y + quad[i].y);
                }
                ImGui::GetWindowDrawList()->AddPolyline(points, 4, kSelectionColor, ImDrawFlags_Closed, 2.0f);
            }
        }
    }
}

void ViewportPanel::draw_3d(EditorContext& context, const Scene& display_scene, bool playing, const Vec2& avail,
                            const Vec2& image_origin) {
    viewport_3d_.set_size(target_width_, target_height_);

    // While editing, the rig moves the camera, so it needs the mutable one; while
    // playing, the camera is the scene's own and is only read.
    Camera3D* editable_camera = playing ? nullptr : find_editable_camera(context.scene());
    const Camera3D* camera = playing ? renderer3d::Renderer3D::find_active_camera(display_scene) : editable_camera;
    if (camera == nullptr) {
        // No 3D camera: clear and say so rather than render a black frame with no
        // explanation.
        device_->bind_render_target(target_.get());
        target_->clear(kWindowClear);
        device_->bind_render_target(nullptr);
        const auto texture_id = static_cast<ImTextureID>(target_->color_texture()->native_handle());
        ImGui::Image(ImTextureRef{texture_id}, ImVec2(avail.x, avail.y), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        ImGui::TextDisabled("No 3D camera in the scene");
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();

    // The image is drawn first, so the hover test below reflects the region the
    // cursor is over. The texture shown is the previous frame's; the frame this
    // gesture produces is rendered immediately after, so the lag is one frame.
    const auto texture_id = static_cast<ImTextureID>(target_->color_texture()->native_handle());
    ImGui::Image(ImTextureRef{texture_id}, ImVec2(avail.x, avail.y), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    const bool image_hovered = ImGui::IsItemHovered();

    if (!playing) {
        // First frame on this camera: seed the rig from it, so an authored camera
        // is the starting view rather than being overwritten by the default rig.
        if (!viewport_3d_.is_seeded()) {
            viewport_3d_.sync_from_camera(*camera);
            viewport_3d_.mark_seeded();
        }

        if (image_hovered) {
            // Right-drag orbits, middle-drag pans, wheel dollies. This is the
            // near-universal 3D-editor convention, and it leaves the left button
            // free for select and move.
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                const ImVec2 delta = io.MouseDelta;
                viewport_3d_.orbit_by_pixels(Vec3{delta.x, delta.y, 0.0f});
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                const ImVec2 delta = io.MouseDelta;
                viewport_3d_.pan_by_pixels(*camera, Vec2{delta.x, delta.y});
            }
            if (io.MouseWheel != 0.0f) {
                viewport_3d_.dolly_by_scroll(io.MouseWheel);
            }
        }

        // "F" frames the selection, "A" frames everything - the shortcuts every
        // 3D tool uses.
        if (image_hovered && !io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                if (Node* selected = context.hierarchy().selected_node(); selected != nullptr) {
                    Vec3 box_min;
                    Vec3 box_max;
                    if (Viewport3D::node_world_bounds(*selected, box_min, box_max)) {
                        viewport_3d_.frame_bounds(box_min, box_max, camera->fov_degrees);
                    }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_A)) {
                Vec3 box_min;
                Vec3 box_max;
                if (Viewport3D::scene_world_bounds(context.scene(), box_min, box_max)) {
                    viewport_3d_.frame_bounds(box_min, box_max, camera->fov_degrees);
                }
            }
        }

        // --- Selection and drag to move (left button) ---------------------
        if (image_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            armed_ = true;
            moved_ = false;
            press_pos_ = Vec2{io.MousePos.x, io.MousePos.y};
        }

        if (armed_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!moved_ && image_hovered) {
                    const Vec2 cursor{io.MousePos.x - image_origin.x, io.MousePos.y - image_origin.y};
                    context.hierarchy().select(viewport_3d_.pick(context.scene(), *camera, cursor));
                }
                armed_ = false;
                context.break_undo_coalescing();
            } else {
                const Vec2 now{io.MousePos.x, io.MousePos.y};
                const Vec2 travel{now.x - press_pos_.x, now.y - press_pos_.y};
                if (!moved_ && std::sqrt(travel.x * travel.x + travel.y * travel.y) > kDragThresholdPixels) {
                    moved_ = true;
                    if (Node* selected = context.hierarchy().selected_node(); selected != nullptr) {
                        drag_origin_3d_ = transform_point(selected->world_matrix(), Vec3{});
                        const Vec2 cursor{press_pos_.x - image_origin.x, press_pos_.y - image_origin.y};
                        // The drag plane faces the camera and passes through the
                        // grabbed point, so the node moves in the screen plane.
                        const Vec3 plane_normal = camera->forward();
                        if (!viewport_3d_.screen_to_plane(*camera, cursor, drag_origin_3d_, plane_normal,
                                                          drag_start_world_3d_)) {
                            // The plane is edge-on to the ray, so the grab point
                            // is undefined; abort the move rather than jump.
                            moved_ = false;
                        }
                    }
                }

                if (moved_ && context.is_editable()) {
                    if (Node* selected = context.hierarchy().selected_node(); selected != nullptr) {
                        const Vec2 cursor{now.x - image_origin.x, now.y - image_origin.y};
                        Vec3 target;
                        if (viewport_3d_.screen_to_plane(*camera, cursor, drag_origin_3d_, camera->forward(), target)) {
                            const Vec3 moved_pos = drag_origin_3d_ + (target - drag_start_world_3d_);
                            serialization::Operation op;
                            op.kind = serialization::OperationKind::SetTransform;
                            op.node_id = selected->id();
                            op.path = {"position"};
                            op.transform_value = moved_pos;
                            context.execute(std::make_unique<OperationCommand>(std::move(op)));
                        }
                    }
                }
            }
        }
    }

    // The rig writes the camera's transform; this must happen before the render
    // so the frame shows the view the gesture just produced.
    if (!playing && editable_camera != nullptr) {
        viewport_3d_.apply_to(*editable_camera);
    }

    // --- Render into the target ---------------------------------------
    device_->bind_render_target(target_.get());
    target_->clear(scene_clear_3d(display_scene));
    auto rendered = renderer_3d_->render(*device_, display_scene, target_width_, target_height_);
    device_->bind_render_target(nullptr);
    if (!rendered) {
        FE_LOG_ERROR("editor: 3D viewport render failed: %s", rendered.error().message().c_str());
    }

    // --- Selection box -------------------------------------------------
    if (!playing && context.hierarchy().has_selection()) {
        const Node* selected = context.hierarchy().selected_node();
        if (selected != nullptr) {
            Vec3 box_min;
            Vec3 box_max;
            if (Viewport3D::node_world_bounds(*selected, box_min, box_max)) {
                std::array<Vec3, 8> corners{};
                Viewport3D::aabb_corners(box_min, box_max, corners);
                std::array<Vec2, 8> projected{};
                bool all_visible = true;
                for (usize i = 0; i < 8; ++i) {
                    if (!viewport_3d_.world_to_screen(*camera, corners[i], projected[i])) {
                        all_visible = false;
                        break;
                    }
                }
                if (all_visible) {
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    for (const auto& edge : kBoxEdges) {
                        const ImVec2 a{image_origin.x + projected[edge[0]].x, image_origin.y + projected[edge[0]].y};
                        const ImVec2 b{image_origin.x + projected[edge[1]].x, image_origin.y + projected[edge[1]].y};
                        draw_list->AddLine(a, b, kSelectionColor, 2.0f);
                    }
                }
            }
        }
    }
}

void ViewportPanel::draw(EditorContext& context) {
    if (ImGui::Begin(name())) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const u32 width = avail.x >= 1.0f ? static_cast<u32>(avail.x) : 1U;
        const u32 height = avail.y >= 1.0f ? static_cast<u32>(avail.y) : 1U;

        if (device_ == nullptr || renderer_ == nullptr) {
            ImGui::TextDisabled("Viewport is not bound to a device.");
            ImGui::End();
            return;
        }

        // While playing, the viewport shows the play copy (read-only); otherwise
        // it shows the authored scene, which the user is editing.
        const PlaySession& play = context.play();
        const bool playing = play.is_playing();
        const Scene& display_scene = playing ? play.play_scene() : context.scene();

        // Resolve the mode. Auto follows the scene; an explicit 3D mode still
        // needs the 3D renderer to exist, so a host without one falls back to 2D
        // rather than drawing nothing.
        bool use_3d = wants_3d(context.viewport_mode(), display_scene);
        if (use_3d && renderer_3d_ == nullptr) {
            use_3d = false;
        }
        if (use_3d != drew_3d_) {
            // Entering a mode re-seeds the 3D rig from the scene's camera, so the
            // authored camera is the starting view rather than the last orbit.
            viewport_3d_.clear_seeded();
        }
        drew_3d_ = use_3d;

        if (!ensure_target(width, height, use_3d)) {
            ImGui::TextDisabled("Viewport target unavailable.");
            ImGui::End();
            return;
        }

        const ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
        const Vec2 image_origin{cursor_pos.x, cursor_pos.y};
        const Vec2 image_size{avail.x, avail.y};

        if (use_3d) {
            draw_3d(context, display_scene, playing, image_size, image_origin);
        } else {
            draw_2d(context, display_scene, playing, image_size, image_origin);
        }

        // A compact status line: the viewport's own size, the mode, and whether
        // the view is live or being edited.
        const char* mode_label = use_3d ? "3D" : "2D";
        if (use_3d) {
            const Camera3D* display_camera = renderer3d::Renderer3D::find_active_camera(display_scene);
            ImGui::TextDisabled("%ux%u  %s  fov %.0f  %s", width, height, mode_label,
                                static_cast<double>(display_camera != nullptr ? display_camera->fov_degrees : 60.0f),
                                playing ? "playing" : "editing");
        } else {
            const Camera2D* display_camera = renderer2d::Renderer2D::find_active_camera(display_scene);
            ImGui::TextDisabled("%ux%u  %s  zoom %.2f  %s", width, height, mode_label,
                                static_cast<double>(display_camera != nullptr ? display_camera->zoom : 1.0f),
                                playing ? "playing" : "editing");
        }
    }
    ImGui::End();
}

} // namespace fe::editor
