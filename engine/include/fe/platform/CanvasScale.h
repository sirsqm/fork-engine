// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

namespace fe {

// The mapping between a web canvas's CSS box and its drawing buffer.
//
// A browser gives a `<canvas>` two independent sizes, and a game is wrong unless
// it knows about both:
//
//   * the *CSS box* is the element's laid-out size in CSS pixels. The page owns
//     it — the export shell makes the canvas fluid, so it changes when the window
//     or a phone is resized or rotated — and a DOM event reports coordinates
//     relative to it;
//   * the *drawing buffer* is the canvas's `width`/`height` attributes in device
//     pixels. The engine owns it, and it is what the viewport must match.
//
// They differ for two reasons: the page can resize the box without the engine
// knowing, and a HiDPI display reports a `devicePixelRatio` above 1, so a CSS
// pixel is several device pixels. Comparing an event's CSS coordinate against a
// viewport measured in device pixels therefore misplaces every touch the moment
// either is true — which is exactly the case on the phones this target exists
// for.
//
// This struct is the one place that arithmetic lives. It is pure — no DOM, no
// Emscripten — so the web backend's resize and input paths share one tested
// definition rather than each doing its own conversion. See
// docs/adr/0077-web-canvas-resize-and-dpr.md.
struct CanvasViewport {
    // The drawing buffer the canvas should have, in device pixels.
    u32 buffer_width = 0;
    u32 buffer_height = 0;

    // Device pixels per CSS pixel, per axis. One scalar would be wrong when the
    // rounding of the two axes differs, so both are kept.
    Vec2 css_to_buffer_scale{1.0f, 1.0f};

    // The mapping for a CSS box of `css_width` x `css_height` on a display whose
    // device pixel ratio is `device_pixel_ratio`.
    //
    // The ratio is clamped to at least 1: a browser reports 1 or more, and a 0 or
    // negative (a page that set it, a headless quirk) would collapse the buffer
    // to nothing and leave the viewport at zero. A CSS dimension that is not
    // positive yields an empty viewport, which the caller reads as "not laid out
    // yet" rather than as a size.
    [[nodiscard]] static CanvasViewport from_css_size(f32 css_width, f32 css_height, f32 device_pixel_ratio) noexcept;

    // True when the mapping describes a usable surface. A canvas the page has not
    // laid out yet has no CSS size, so its mapping is empty and must not be
    // applied over a good one.
    [[nodiscard]] bool valid() const noexcept { return buffer_width > 0 && buffer_height > 0; }

    // Converts a DOM event coordinate — CSS pixels relative to the canvas, which
    // is what the browser reports — into drawing-buffer pixels, the space the
    // viewport and the action layer's touch regions are in.
    [[nodiscard]] Vec2 css_to_buffer_point(f32 css_x, f32 css_y) const noexcept {
        return Vec2{css_x * css_to_buffer_scale.x, css_y * css_to_buffer_scale.y};
    }
};

} // namespace fe
