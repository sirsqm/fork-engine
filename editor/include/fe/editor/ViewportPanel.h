// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/editor/Panel.h"
#include "fe/editor/Viewport.h"
#include "fe/editor/Viewport3D.h"
#include "fe/rhi/Types.h"

namespace fe {

namespace rhi {
class Device;
}

namespace renderer2d {
class Renderer2D;
}

namespace renderer3d {
class Renderer3D;
}

namespace editor {

// The viewport panel: the scene rendered into an off-screen target and shown as
// an image inside an ImGui window.
//
// This is where the RHI's render-target seam pays off (docs/ARCHITECTURE.md
// section 8): the panel binds a `RenderTarget` sized to its content region,
// renders the scene with the shared renderer, unbinds (which restores the window
// viewport), and hands the target's colour texture to `ImGui::Image`. The
// runtime's renderer is used unchanged, so the viewport shows exactly what the
// game would draw, and no second render path exists.
//
// It serves both renderers. A scene may hold 2D content, 3D content, or both, so
// the panel renders the mode the context asks for (`ViewportMode`), and the two
// modes share the target, the image, and the pick/drag gesture. The 3D mode
// needs a depth buffer, so it uses a second target with `depth = true`; the 2D
// mode keeps the colour-only one, because a 2D scene has no depth to test.
//
// All interaction logic - camera pan/zoom and picking for 2D, orbit/pan/dolly
// and ray picking for 3D - lives in `Viewport` and `Viewport3D`, which are
// headless and unit-tested. This class is the thin toolkit projection of them,
// which is the same split the hierarchy and inspector use.
//
// Resource ownership: the targets are created lazily against the embedded
// device, recreated when the panel's size changes, and released in shutdown()
// before the device goes away. The panel never outlives the device.
class ViewportPanel final : public Panel {
public:
    ViewportPanel() = default;
    ~ViewportPanel() override;

    ViewportPanel(const ViewportPanel&) = delete;
    ViewportPanel& operator=(const ViewportPanel&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return "Viewport"; }

    // Binds the panel to the engine's device and renderers. All three must
    // outlive the panel. Safe to call once after engine start-up. The 3D
    // renderer may be null in a host that has none, in which case the 3D mode
    // reports it is unavailable rather than crashing.
    void initialize(rhi::Device& device, renderer2d::Renderer2D& renderer,
                    renderer3d::Renderer3D* renderer_3d = nullptr);

    // Releases the off-screen targets. Must be called before the device shuts
    // down, because a target's destructor frees GPU objects.
    void shutdown();

    void draw(EditorContext& context) override;

    // The pixel size of the last frame's render target (0 before the first
    // draw). Exposed for tests, which assert the target tracks the panel.
    [[nodiscard]] u32 target_width() const noexcept { return target_width_; }
    [[nodiscard]] u32 target_height() const noexcept { return target_height_; }

    // The off-screen target after the first draw, or null before it. Borrowed;
    // the panel owns it. `target()` is the colour target the last frame drew
    // into, whichever mode it was.
    [[nodiscard]] rhi::RenderTarget* target() const noexcept { return target_.get(); }

    // True when the last frame drew in 3D. Exposed for tests, which assert the
    // mode selection without a screenshot.
    [[nodiscard]] bool is_3d() const noexcept { return drew_3d_; }

private:
    // Ensures the target matches the requested size and depth, recreating it on
    // a change. Returns false when there is no device or the size is degenerate.
    // The depth flag is part of the identity, so switching modes re-creates the
    // target once and then reuses it.
    [[nodiscard]] bool ensure_target(u32 width, u32 height, bool depth);

    // Draws the 3D path into the target and handles its interaction. `avail` is
    // the image size and `image_origin` its top-left screen position, both as
    // plain float pairs so this header does not depend on the UI toolkit.
    void draw_3d(EditorContext& context, const Scene& display_scene, bool playing, const Vec2& avail,
                 const Vec2& image_origin);

    // Draws the 2D path into the target and handles its interaction.
    void draw_2d(EditorContext& context, const Scene& display_scene, bool playing, const Vec2& avail,
                 const Vec2& image_origin);

    rhi::Device* device_ = nullptr;
    renderer2d::Renderer2D* renderer_ = nullptr;
    renderer3d::Renderer3D* renderer_3d_ = nullptr;
    rhi::RenderTargetRef target_;
    u32 target_width_ = 0;
    u32 target_height_ = 0;
    bool target_has_depth_ = false;
    bool drew_3d_ = false;
    Viewport viewport_;
    Viewport3D viewport_3d_;

    // The in-progress left-button gesture in the viewport. A press arms it; once
    // the cursor travels past a small threshold it becomes a move (`moved_`),
    // and a release without one is a pick. `drag_origin_` is the selected node's
    // world position when the move began and `drag_start_world_` is the world
    // point under the cursor at that moment; the node is placed at
    // `drag_origin_ + (cursor_world - drag_start_world_)` each frame, so it keeps
    // the offset it was grabbed by and the gesture accumulates no rounding.
    bool armed_ = false;
    bool moved_ = false;
    Vec2 press_pos_{};
    Vec2 drag_origin_{};
    Vec2 drag_start_world_{};

    // The 3D drag's starting state: the selected node's world position and the
    // world point under the cursor when the move began, so a 3D drag also keeps
    // the grab offset. The drag moves the node in the camera's facing plane.
    Vec3 drag_origin_3d_{};
    Vec3 drag_start_world_3d_{};
};

} // namespace editor
} // namespace fe
