// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/serialization/Json.h"

namespace fe {

// All JSON conversions for the engine's value types live in Json.h as inline
// functions. This translation unit exists so that (a) the JSON dependency is
// anchored to one place in the build and (b) format changes have a stable file
// to reference. It is intentionally almost empty.

} // namespace fe
