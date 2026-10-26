// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/TextLabel.h"

#include "fe/resources/Font.h"

namespace fe {

f32 TextLabel::measured_width() const noexcept {
    const auto* as_font = dynamic_cast<const Font*>(font.get());
    if (as_font == nullptr) {
        return 0.0f;
    }
    // The atlas is laid out at a nominal pixel size and `size` scales it, so the
    // measured advance is scaled the same way the quads are. Without a nominal
    // size in the metrics the scale is the identity, which is what a font
    // generated at one pixel size wants.
    return as_font->measure(text) * size;
}

} // namespace fe
