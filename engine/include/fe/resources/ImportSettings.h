// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/rhi/Types.h"

#include <string_view>

namespace fe {

// Per-asset import settings: how a source file is turned into a runtime
// resource.
//
// An asset is imported once, and the choices made then (how a texture is
// sampled, later how a mesh is scaled or a sprite sheet is sliced) are
// properties of the asset, not of every component that references it. They are
// stored in the asset's sidecar next to its GUID, so they travel with the file
// and survive a re-import. See
// docs/adr/0027-asset-import-settings.md.
//
// The settings are a single tagged value type rather than a per-scheme class
// hierarchy: the sidecar is JSON, the editor draws them by kind, and the number
// of kinds is small. A new asset type adds a kind and the fields it needs
// without changing how any other type is stored.
//
// This type is a value: it knows how to serialize itself and how to describe
// itself, but it never touches the filesystem or a graphics device. Applying a
// setting is the loader's job, because only the loader knows what the setting
// means for its resource.
struct ImportSettings {
    // The scheme these settings belong to ("tex", "obj", "audio", ""). It is
    // the top-level selector the sidecar already stores; the settings are the
    // payload for that scheme.
    enum class Kind : u8 {
        Generic, // No type-specific settings yet.
        Texture, // A GPU texture (scheme "tex").
    };

    static constexpr i32 kFormatVersion = 1;

    Kind kind = Kind::Generic;

    // --- Texture settings (read when kind == Texture) ----------------------
    // Sampling filter for both minification and magnification.
    rhi::TextureFilter filter = rhi::TextureFilter::Linear;
    // Addressing mode for both U and V.
    rhi::TextureWrap wrap = rhi::TextureWrap::ClampToEdge;
    // Generate a mip chain on upload.
    bool mipmaps = false;

    // The defaults the texture importer produces, i.e. the behaviour before
    // import settings existed. Used to reject a document that sets nothing.
    [[nodiscard]] static ImportSettings texture_defaults() noexcept {
        ImportSettings settings;
        settings.kind = Kind::Texture;
        return settings;
    }

    // True when the settings are all defaults for their kind, so a sidecar can
    // omit them entirely and keep its file identical to one written before
    // import settings existed.
    [[nodiscard]] bool is_default() const noexcept;
};

// A stable name for a settings kind, used in the sidecar JSON and by the editor.
[[nodiscard]] const char* to_string(ImportSettings::Kind kind) noexcept;

// Parses a settings kind from its name. Unknown names are an error, so a
// sidecar written by a newer engine fails clearly instead of importing an asset
// with settings silently dropped.
[[nodiscard]] Result<ImportSettings::Kind> import_kind_from_string(std::string_view name);

// Serializes settings to a JSON object and reads them back. Both are pure: they
// take and return values and never touch the filesystem. `read` fails on a
// malformed document or an unknown kind; a missing optional field takes its
// default, so a document written by an older engine still loads.
[[nodiscard]] std::string import_settings_to_json(const ImportSettings& settings);
[[nodiscard]] Result<ImportSettings> import_settings_from_json(std::string_view json_text);

} // namespace fe
