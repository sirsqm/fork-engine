// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/editor/Panel.h"
#include "fe/editor/Viewport.h"
#include "fe/rhi/Types.h"

namespace fe {

namespace rhi {
class Device;
}

namespace renderer2d {
class Renderer2D;
}

namespace editor {

// The 2D viewport panel: the scene rendered into an off-screen target and shown
// as an image inside an ImGui window.
//
// This is where the RHI's render-target seam pays off (docs/ARCHITECTURE.md
// section 8): the panel binds a `RenderTarget` sized to its content region,
// renders the scene with the shared `Renderer2D`, unbinds (which restores the
// window viewport), and hands the target's colour texture to `ImGui::Image`.
// The runtime's renderer is used unchanged, so the viewport shows exactly what
// the game would draw, and no second render path exists.
//
// All interaction logic - camera pan/zoom and picking - lives in `Viewport`,
// which is headless and unit-tested. This class is the thin toolkit projection
// of it, which is the same split the hierarchy and inspector use.
//
// Resource ownership: the target is created lazily against the embedded
// device, recreated when the panel's size changes, and released in shutdown()
// before the device goes away. The panel never outlives the device.
class ViewportPanel final : public Panel {
public:
    ViewportPanel() = default;
    ~ViewportPanel() override;

    ViewportPanel(const ViewportPanel&) = delete;
    ViewportPanel& operator=(const ViewportPanel&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return "Viewport"; }

    // Binds the panel to the engine's device and 2D renderer. Both must outlive
    // the panel. Safe to call once after engine start-up.
    void initialize(rhi::Device& device, renderer2d::Renderer2D& renderer);

    // Releases the off-screen target. Must be called before the device shuts
    // down, because the target's destructor frees GPU objects.
    void shutdown();

    void draw(EditorContext& context) override;

    // The pixel size of the last frame's render target (0 before the first
    // draw). Exposed for tests, which assert the target tracks the panel.
    [[nodiscard]] u32 target_width() const noexcept { return target_width_; }
    [[nodiscard]] u32 target_height() const noexcept { return target_height_; }

    // The off-screen target after the first draw, or null before it. Borrowed;
    // the panel owns it.
    [[nodiscard]] rhi::RenderTarget* target() const noexcept { return target_.get(); }

private:
    // Ensures the target matches the requested size, recreating it on a change.
    // Returns false when there is no device or the size is degenerate.
    [[nodiscard]] bool ensure_target(u32 width, u32 height);

    rhi::Device* device_ = nullptr;
    renderer2d::Renderer2D* renderer_ = nullptr;
    rhi::RenderTargetRef target_;
    u32 target_width_ = 0;
    u32 target_height_ = 0;
    Viewport viewport_;
};

} // namespace editor
} // namespace fe
