// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "NullWindow.h"

namespace fe {

NullWindow::NullWindow(const WindowDesc& desc) : Window("null"), width_(desc.width), height_(desc.height) {}

NullWindow::~NullWindow() = default;

void NullWindow::poll_events() {
    // No OS events exist. The InputState stays empty by construction.
}

void NullWindow::swap_buffers() {
    // No back buffer to present.
}

bool NullWindow::should_close() const {
    return close_requested_;
}

void NullWindow::request_close() {
    close_requested_ = true;
}

u32 NullWindow::width() const noexcept {
    return width_;
}

u32 NullWindow::height() const noexcept {
    return height_;
}

u32 NullWindow::framebuffer_width() const noexcept {
    // No scale factor and no surface: the logical size is the framebuffer size.
    return width_;
}

u32 NullWindow::framebuffer_height() const noexcept {
    return height_;
}

void NullWindow::set_title(const std::string& title) {
    // Nothing to retitle; accept and ignore so callers do not branch.
    (void)title;
}

void NullWindow::set_vsync(bool enabled) {
    (void)enabled;
}

void* NullWindow::get_proc_address(const char* name) const {
    (void)name;
    return nullptr;
}

} // namespace fe
