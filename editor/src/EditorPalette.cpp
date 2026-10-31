// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/EditorPalette.h"

#include <algorithm>
#include <cmath>

namespace fe::editor {
namespace {

// The sRGB transfer function, undone: a channel's stored value back to linear
// light. The piecewise form is the standard one (IEC 61966-2-1); a single 2.2
// power would be close but wrong near black, which is exactly where a dark
// theme's text sits.
[[nodiscard]] f32 linearize(f32 channel) noexcept {
    if (channel <= 0.04045f) {
        return channel / 12.92f;
    }
    return std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

} // namespace

f32 relative_luminance(const Rgba& colour) noexcept {
    return 0.2126f * linearize(colour.r) + 0.7152f * linearize(colour.g) + 0.0722f * linearize(colour.b);
}

f32 contrast_ratio(const Rgba& a, const Rgba& b) noexcept {
    const f32 la = relative_luminance(a);
    const f32 lb = relative_luminance(b);
    const f32 lighter = std::max(la, lb);
    const f32 darker = std::min(la, lb);
    return (lighter + 0.05f) / (darker + 0.05f);
}

} // namespace fe::editor
