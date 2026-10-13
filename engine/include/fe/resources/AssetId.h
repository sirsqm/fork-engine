// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/core/Uuid.h"

#include <string>
#include <string_view>

namespace fe {

// The stable identity of an asset.
//
// An AssetId is a fe::Uuid, not a second id type: node identity and resource
// identity are the same kind of thing, and reusing the type keeps one
// implementation of parsing, formatting, and hashing. See
// docs/adr/0020-resource-identity-guid-sidecar.md.
//
// The identity is persisted in the asset's sidecar metadata file and is not
// derived from the asset's path, which is what lets a reference survive a file
// move or rename.
using AssetId = Uuid;

// The prefix that marks a resource reference as GUID-based rather than
// path-based. A reference string is either "guid:<uuid>" (resolved through the
// asset database) or the legacy "<scheme>:<path>" form (resolved directly).
inline constexpr std::string_view kGuidScheme = "guid";

// The suffix of an asset's metadata sidecar: "hero.png" -> "hero.png.meta".
inline constexpr std::string_view kMetaSuffix = ".meta";

// Formats a GUID reference for storage: "guid:3f2504e0-....".
[[nodiscard]] inline std::string to_reference(const AssetId& id) {
    return std::string(kGuidScheme) + ":" + id.to_string();
}

// Returns true when `reference` is a GUID reference ("guid:..."), and parses the
// id into `out_id` when it is well formed. A malformed "guid:" reference returns
// false, so the caller can report it rather than treat it as a path.
[[nodiscard]] inline bool parse_reference(std::string_view reference, AssetId& out_id) noexcept {
    if (reference.size() <= kGuidScheme.size() + 1 || reference.compare(0, kGuidScheme.size(), kGuidScheme) != 0 ||
        reference[kGuidScheme.size()] != ':') {
        return false;
    }
    return AssetId::try_parse(reference.substr(kGuidScheme.size() + 1), out_id);
}

} // namespace fe
