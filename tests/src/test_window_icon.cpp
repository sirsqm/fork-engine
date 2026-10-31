// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the engine's window icon.
//
// The icon is generated from the project's logo (tools/gen_engine_icon.py), so
// what is worth pinning is not the exact pixels - those are regenerated from the
// SVG - but the properties that make the buffer usable by a window backend: it
// is square, it is the size the generator promises, it has transparent and
// opaque pixels (so it is a mark on a transparent field, not a filled square or
// an empty one), and the ink and the default are what the API says.

#include <doctest/doctest.h>

#include "fe/platform/Window.h"
#include "fe/platform/WindowIcon.h"

using namespace fe;

namespace {

[[nodiscard]] bool covered(const WindowIcon& icon, u32 x, u32 y) {
    return icon.pixels()[(static_cast<usize>(y) * icon.width() + x) * 4 + 3] != 0;
}

} // namespace

TEST_CASE("the default icon is empty") {
    const WindowIcon icon;
    CHECK(icon.empty());
    CHECK(icon.pixels().empty());
    CHECK(icon.width() == 0);
    CHECK(icon.height() == 0);
}

TEST_CASE("the mark is a square RGBA buffer of the promised size") {
    const WindowIcon icon = WindowIcon::white();

    CHECK_FALSE(icon.empty());
    CHECK(icon.width() == WindowIcon::kEdge);
    CHECK(icon.height() == WindowIcon::kEdge);
    CHECK(icon.pixels().size() == static_cast<usize>(WindowIcon::kEdge) * WindowIcon::kEdge * 4);
}

TEST_CASE("the mark covers some pixels and leaves the rest transparent") {
    // A filled square or an empty buffer would both satisfy a size check, so
    // assert the shape is actually a mark: some covered pixels and some clear
    // ones. The emblem does not fill its box, so both must be present.
    const WindowIcon icon = WindowIcon::white();

    usize opaque = 0;
    usize clear = 0;
    for (usize i = 0; i < icon.pixels().size(); i += 4) {
        if (icon.pixels()[i + 3] == 0) {
            ++clear;
        } else {
            ++opaque;
        }
    }

    CHECK(opaque > 0);
    CHECK(clear > 0);
    CHECK(opaque + clear == static_cast<usize>(WindowIcon::kEdge) * WindowIcon::kEdge);
}

TEST_CASE("the mark is the ring and fork, not a filled or transposed square") {
    // The emblem is a ring (a hollow circle) with a fork inside it. Assert
    // landmarks that a mis-generated mask would break: the corners are clear,
    // the ring's band is covered at mid-height on both sides, and the fork's
    // stem is covered below the middle. The very centre is deliberately not
    // checked: it falls in the gap between the fork's tines.
    const WindowIcon icon = WindowIcon::white();
    const u32 last = WindowIcon::kEdge - 1;
    const u32 mid = WindowIcon::kEdge / 2;

    CHECK_FALSE(covered(icon, 0, 0));
    CHECK_FALSE(covered(icon, last, 0));
    CHECK_FALSE(covered(icon, 0, last));
    CHECK_FALSE(covered(icon, last, last));

    // The ring's left and right band, at mid-height.
    CHECK(covered(icon, 1, mid));
    CHECK(covered(icon, last - 1, mid));
    // The fork's stem, below the ring's centre.
    CHECK(covered(icon, mid, last - 9));
}

TEST_CASE("the white and black marks share geometry and differ only in ink") {
    const WindowIcon white = WindowIcon::white();
    const WindowIcon black = WindowIcon::black();

    REQUIRE(white.pixels().size() == black.pixels().size());
    for (usize i = 0; i < white.pixels().size(); i += 4) {
        // Same coverage at every pixel...
        CHECK(white.pixels()[i + 3] == black.pixels()[i + 3]);
        if (white.pixels()[i + 3] != 0) {
            // ...and the covered pixels are white in one and black in the other.
            CHECK(white.pixels()[i] == 255);
            CHECK(black.pixels()[i] == 0);
        }
    }
}

TEST_CASE("mark tints the covered pixels with the requested ink") {
    const WindowIcon icon = WindowIcon::mark(10, 20, 30);
    for (usize i = 0; i < icon.pixels().size(); i += 4) {
        if (icon.pixels()[i + 3] != 0) {
            CHECK(icon.pixels()[i] == 10);
            CHECK(icon.pixels()[i + 1] == 20);
            CHECK(icon.pixels()[i + 2] == 30);
        }
    }
}

TEST_CASE("a window descriptor defaults to the engine mark") {
    const WindowDesc desc;
    CHECK_FALSE(desc.icon.empty());
    CHECK(desc.icon.width() == WindowIcon::kEdge);
}

TEST_CASE("the null window accepts an icon without a surface") {
    // The base-class default is a no-op, which is what a backend with no OS
    // needs; the call must not crash or require a display.
    WindowDesc desc;
    desc.backend = WindowBackend::Null;
    desc.visible = false;
    auto window = Window::create(desc);
    REQUIRE(window.ok());
    window.value()->set_icon(WindowIcon::black());
    CHECK(window.value()->width() == desc.width);
}
