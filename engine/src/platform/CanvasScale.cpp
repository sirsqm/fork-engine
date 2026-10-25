// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/CanvasScale.h"

namespace fe {

CanvasViewport CanvasViewport::from_css_size(f32 css_width, f32 css_height, f32 device_pixel_ratio) noexcept {
    CanvasViewport viewport;
    if (css_width <= 0.0f || css_height <= 0.0f) {
        return viewport;
    }

    // A browser reports a ratio of 1 or more. Clamp rather than trust: a ratio of
    // zero or a negative would produce a zero-sized buffer, and the viewport
    // would then be zero, which every renderer treats as a hidden window.
    const f32 ratio = device_pixel_ratio > 1.0f ? device_pixel_ratio : 1.0f;

    const f32 buffer_width = css_width * ratio;
    const f32 buffer_height = css_height * ratio;
    viewport.buffer_width = static_cast<u32>(buffer_width);
    viewport.buffer_height = static_cast<u32>(buffer_height);

    // Derive the scale from the *rounded* buffer, not from the raw ratio, so a
    // point converted through it lands inside the buffer that was actually set.
    // On a 1.5 ratio the rounding differs between the axes; using the ratio here
    // would put a touch a fraction of a pixel outside the buffer's edge.
    if (viewport.buffer_width > 0 && viewport.buffer_height > 0) {
        viewport.css_to_buffer_scale = Vec2{static_cast<f32>(viewport.buffer_width) / css_width,
                                            static_cast<f32>(viewport.buffer_height) / css_height};
    }
    return viewport;
}

} // namespace fe
