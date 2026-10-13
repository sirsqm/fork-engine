// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/Component.h"
#include "fe/scene/Transform.h"

namespace fe {

// Out-of-line definitions for the scene value types. Transform and Component
// are header-only by design; this translation unit exists so the engine library
// has a stable place for any future out-of-line members and so the module's
// compilation unit is not empty.

Component::~Component() = default;

} // namespace fe
