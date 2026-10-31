// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

// The island graph lives in the shared `physics` module so the 2D and 3D
// systems use one implementation (ADR-0111). This header keeps the
// `fe::physics2d` names the 2D system and its tests already use.
#include "fe/physics/IslandSolver.h"

namespace fe::physics2d {

using physics::IslandBuilder;
using physics::IslandLink;

} // namespace fe::physics2d
