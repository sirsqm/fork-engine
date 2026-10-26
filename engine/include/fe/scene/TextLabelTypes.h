// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the text component with the reflection registry, so the scene
// serializer persists it and the inspector can edit it, exactly as
// register_tilemap_types does for a tilemap.
void register_text_types();

} // namespace fe::reflect
