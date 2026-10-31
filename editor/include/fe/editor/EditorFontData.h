// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

// The editor's typefaces, embedded in the binary.
//
// The editor ships a real typeface (Inter, with JetBrains Mono for the monospaced
// spots) instead of Dear ImGui's built-in bitmap font, which is what makes the
// chrome read as a tool rather than a debug overlay. The bytes live in the
// executable so the same code finds the font in a build tree, an install tree,
// and the AppImage: there is no data directory to locate relative to the
// executable, and a user cannot break the editor by moving a file.
//
// The arrays are defined in a generated translation unit (see
// cmake/EmbedFonts.cmake) and this header is the committed contract it satisfies.
// The fonts themselves are committed under editor/assets/fonts/ with their OFL
// licences beside them.

namespace fe::editor::fonts {

// One embedded font: the bytes and their length. Not null-terminated.
struct EmbeddedFont {
    const unsigned char* data;
    unsigned long size;
};

// In the order EmbedFonts.cmake emits them: Inter Regular, Inter SemiBold,
// JetBrains Mono Regular.
extern const EmbeddedFont kEmbeddedFonts[];
extern const unsigned long kEmbeddedFontCount;

} // namespace fe::editor::fonts
