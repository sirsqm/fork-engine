// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/Window.h"

#include <memory>

namespace fe {

// The desktop window backend: a real OS window with an OpenGL context, via GLFW.
//
// GLFW is the implementation detail this class exists to contain. It never
// appears in a public header, and it appears in exactly one translation unit
// (GlfwWindow.cpp), so a platform where GLFW cannot be compiled — the web
// target — simply does not compile this file. See
// docs/adr/0041-platform-contract-and-null-backend.md.
//
// Internal to the platform module: a caller selects it with the default
// `WindowDesc::backend` and receives a `Window`.
class GlfwWindow final : public Window {
public:
    // Creates the GLFW library (once per process), the OS window, and its
    // OpenGL context, and installs the input callbacks. Returns an Error if GLFW
    // has no display or the context could not be created.
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(const WindowDesc& desc);

    ~GlfwWindow() override;

    GlfwWindow(const GlfwWindow&) = delete;
    GlfwWindow& operator=(const GlfwWindow&) = delete;

    void poll_events() override;
    void swap_buffers() override;

    [[nodiscard]] bool should_close() const override;
    void request_close() override;

    [[nodiscard]] u32 width() const noexcept override;
    [[nodiscard]] u32 height() const noexcept override;
    [[nodiscard]] u32 framebuffer_width() const noexcept override;
    [[nodiscard]] u32 framebuffer_height() const noexcept override;

    void set_title(const std::string& title) override;
    void set_vsync(bool enabled) override;

    // Resolves an OpenGL entry point through GLFW's loader, which asks the
    // current context's driver. This is what the graphics backend loads its
    // function pointers with.
    [[nodiscard]] void* get_proc_address(const char* name) const override;

private:
    GlfwWindow();
};

} // namespace fe
