// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the canvas-to-drawing-buffer mapping (docs/PLAN.md,
// `web-resize-and-dpr`).
//
// The mapping is a pure function of a CSS box and a device pixel ratio, so it
// needs no browser. What it pins is the arithmetic the web backend relies on: a
// canvas whose CSS box differs from its drawing buffer, and a HiDPI display,
// must still put a touch where it looks. See
// docs/adr/0077-web-canvas-resize-and-dpr.md.

#include <doctest/doctest.h>

#include "fe/platform/CanvasScale.h"

using namespace fe;

TEST_CASE("a ratio of one maps CSS pixels straight to device pixels") {
    const CanvasViewport viewport = CanvasViewport::from_css_size(800.0f, 450.0f, 1.0f);

    CHECK(viewport.valid());
    CHECK(viewport.buffer_width == 800);
    CHECK(viewport.buffer_height == 450);
    CHECK(viewport.css_to_buffer_scale.x == doctest::Approx(1.0f));
    CHECK(viewport.css_to_buffer_scale.y == doctest::Approx(1.0f));

    const Vec2 point = viewport.css_to_buffer_point(100.0f, 50.0f);
    CHECK(point.x == doctest::Approx(100.0f));
    CHECK(point.y == doctest::Approx(50.0f));
}

TEST_CASE("a HiDPI ratio grows the drawing buffer and scales event coordinates") {
    // A 400x300 CSS box on a 2x display: the buffer is 800x600 device pixels, and
    // a click at the CSS midpoint lands at the buffer midpoint rather than at the
    // CSS coordinate. Without the scale a touch on a phone would be off by half
    // the canvas, which is the bug this item exists to fix.
    const CanvasViewport viewport = CanvasViewport::from_css_size(400.0f, 300.0f, 2.0f);

    CHECK(viewport.valid());
    CHECK(viewport.buffer_width == 800);
    CHECK(viewport.buffer_height == 600);

    const Vec2 point = viewport.css_to_buffer_point(200.0f, 150.0f);
    CHECK(point.x == doctest::Approx(400.0f));
    CHECK(point.y == doctest::Approx(300.0f));
}

TEST_CASE("a fractional ratio derives the scale from the rounded buffer") {
    // 1.5 rounds the two axes independently, so the scale must be derived from
    // the buffer that was actually set, not from the raw ratio: a point converted
    // through the raw ratio can land a fraction outside the buffer's edge.
    const CanvasViewport viewport = CanvasViewport::from_css_size(333.0f, 111.0f, 1.5f);

    CHECK(viewport.valid());
    CHECK(viewport.buffer_width == 499);
    CHECK(viewport.buffer_height == 166);

    // The bottom-right CSS corner must map to the buffer's last pixel, not beyond.
    const Vec2 corner = viewport.css_to_buffer_point(333.0f, 111.0f);
    CHECK(corner.x == doctest::Approx(499.0f));
    CHECK(corner.y == doctest::Approx(166.0f));
}

TEST_CASE("a ratio below one is clamped so the buffer never collapses") {
    // A browser reports 1 or more. A zero or negative (a headless quirk, a page
    // that set it) must not produce a zero-sized buffer, which every renderer
    // reads as a hidden window.
    const CanvasViewport viewport = CanvasViewport::from_css_size(640.0f, 360.0f, 0.0f);

    CHECK(viewport.valid());
    CHECK(viewport.buffer_width == 640);
    CHECK(viewport.buffer_height == 360);
}

TEST_CASE("an unlaid-out canvas yields an empty mapping") {
    // A hidden tab or a display:none element has no CSS size. The caller must be
    // able to tell that apart from a real size, so it keeps the last good mapping
    // instead of blanking the viewport.
    CHECK_FALSE(CanvasViewport::from_css_size(0.0f, 0.0f, 2.0f).valid());
    CHECK_FALSE(CanvasViewport::from_css_size(-4.0f, 100.0f, 2.0f).valid());
    CHECK_FALSE(CanvasViewport::from_css_size(100.0f, 0.0f, 2.0f).valid());
}
