// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/Window.h"

#include <string>

namespace fe {

// The null (headless) window backend: a Window with no OS surface and no
// graphics context.
//
// It exists for CI, dedicated servers, and deterministic tests that run with no
// display. It satisfies the same Window interface as the GLFW backend, so the
// engine's frame loop drives it identically; the only difference is that it
// reports `has_context() == false`, which is how the runtime knows to skip the
// graphics stack rather than pretend to render.
//
// Internal to the platform module: a caller selects it with
// `WindowDesc::backend = WindowBackend::Null` and receives a `Window`.
class NullWindow final : public Window {
public:
    explicit NullWindow(const WindowDesc& desc);
    ~NullWindow() override;

    NullWindow(const NullWindow&) = delete;
    NullWindow& operator=(const NullWindow&) = delete;

    // No OS to poll.
    void poll_events() override;
    // No back buffer to present.
    void swap_buffers() override;

    [[nodiscard]] bool should_close() const override;
    void request_close() override;

    [[nodiscard]] u32 width() const noexcept override;
    [[nodiscard]] u32 height() const noexcept override;
    [[nodiscard]] u32 framebuffer_width() const noexcept override;
    [[nodiscard]] u32 framebuffer_height() const noexcept override;

    void set_title(const std::string& title) override;
    void set_vsync(bool enabled) override;

    // No context, so there is no entry point to resolve.
    [[nodiscard]] void* get_proc_address(const char* name) const override;

    // The distinguishing fact of this backend.
    [[nodiscard]] bool has_context() const noexcept override { return false; }

private:
    // Reported by width()/framebuffer_width(). Kept so the runtime's viewport
    // maths has a stable, caller-chosen size even with no surface.
    u32 width_ = 0;
    u32 height_ = 0;
    bool close_requested_ = false;
};

} // namespace fe
