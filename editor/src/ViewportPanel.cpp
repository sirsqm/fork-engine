// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ViewportPanel.h"

#include "fe/core/Log.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/OperationCommand.h"
#include "fe/editor/PlaySession.h"
#include "fe/renderer2d/Renderer2D.h"
#include "fe/rhi/Device.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Transform.h"
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

// The clear colour the scene's active camera asks for, or the window clear when
// the scene has no camera. Shared with the runtime's own policy.
[[nodiscard]] rhi::ClearColor scene_clear(const Scene& scene) noexcept {
    if (const Camera2D* camera = renderer2d::Renderer2D::find_active_camera(scene); camera != nullptr) {
        return rhi::ClearColor{camera->clear_r, camera->clear_g, camera->clear_b, camera->clear_a};
    }
    return kWindowClear;
}

} // namespace

ViewportPanel::~ViewportPanel() {
    // Release the target here as well as in shutdown(), because a panel can be
    // destroyed without an explicit shutdown when the UI tears down. The target
    // only exists when a device was bound, which implies shutdown() semantics.
    target_.reset();
}

void ViewportPanel::initialize(rhi::Device& device, renderer2d::Renderer2D& renderer) {
    device_ = &device;
    renderer_ = &renderer;
}

void ViewportPanel::shutdown() {
    target_.reset();
    target_width_ = 0;
    target_height_ = 0;
}

bool ViewportPanel::ensure_target(u32 width, u32 height) {
    if (device_ == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (target_ != nullptr && target_width_ == width && target_height_ == height) {
        return true;
    }

    rhi::RenderTargetDesc desc;
    desc.name = "editor.viewport";
    desc.width = width;
    desc.height = height;
    auto created = device_->create_render_target(desc);
    if (!created) {
        FE_LOG_ERROR("editor: viewport target creation failed: %s", created.error().message().c_str());
        target_.reset();
        target_width_ = 0;
        target_height_ = 0;
        return false;
    }
    target_ = created.value();
    target_width_ = width;
    target_height_ = height;
    return true;
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

        if (!ensure_target(width, height)) {
            ImGui::TextDisabled("Viewport target unavailable.");
            ImGui::End();
            return;
        }

        viewport_.set_size(width, height);

        const Scene& display_scene = playing ? play.play_scene() : context.scene();

        // Render the scene into the off-screen target, then return to the
        // window. Binding the target sets the viewport to its size; binding
        // nullptr restores the window's, so the ImGui draw below is not offset.
        device_->bind_render_target(target_.get());
        target_->clear(scene_clear(display_scene));
        auto rendered = renderer_->render(*device_, display_scene, width, height);
        device_->bind_render_target(nullptr);
        if (!rendered) {
            FE_LOG_ERROR("editor: viewport render failed: %s", rendered.error().message().c_str());
        }

        // --- Present the target as an ImGui image --------------------------
        const ImVec2 image_origin = ImGui::GetCursorScreenPos();
        const auto texture_id = static_cast<ImTextureID>(target_->color_texture()->native_handle());
        // GL textures have their origin at the bottom row, ImGui's at the top,
        // so the V axis is flipped to right the image.
        ImGui::Image(ImTextureRef{texture_id}, avail, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

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

        // A compact status line: the viewport's own size, the current zoom, and
        // whether the view is live or being edited.
        const Camera2D* display_camera = renderer2d::Renderer2D::find_active_camera(display_scene);
        ImGui::TextDisabled("%ux%u  zoom %.2f  %s", width, height,
                            static_cast<double>(display_camera != nullptr ? display_camera->zoom : 1.0f),
                            playing ? "playing" : "editing");
    }
    ImGui::End();
}

} // namespace fe::editor
