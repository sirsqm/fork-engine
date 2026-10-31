// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/WindowIcon.h"

#include "EngineIconMask.h"

namespace fe {

WindowIcon::WindowIcon(u8 r, u8 g, u8 b) {
    width_ = kEdge;
    height_ = kEdge;
    pixels_.assign(static_cast<usize>(kEdge) * kEdge * 4, 0);
    for (u32 i = 0; i < kEdge * kEdge; ++i) {
        // Row-major, most-significant bit first, matching the generator.
        const u8 packed = platform::icon::kMask[i / 8];
        const bool covered = (packed & (0x80u >> (i % 8))) != 0;
        if (covered) {
            const usize o = static_cast<usize>(i) * 4;
            pixels_[o] = r;
            pixels_[o + 1] = g;
            pixels_[o + 2] = b;
            pixels_[o + 3] = 255;
        }
    }
}

WindowIcon WindowIcon::white() {
    return WindowIcon(255, 255, 255);
}

WindowIcon WindowIcon::black() {
    return WindowIcon(0, 0, 0);
}

WindowIcon WindowIcon::mark(u8 r, u8 g, u8 b) {
    return WindowIcon(r, g, b);
}

} // namespace fe
