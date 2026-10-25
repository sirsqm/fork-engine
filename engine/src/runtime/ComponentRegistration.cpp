// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/runtime/ComponentRegistration.h"

#include "fe/anim/AnimationTypes.h"
#include "fe/audio/AudioTypes.h"
#include "fe/physics2d/PhysicsTypes.h"
#include "fe/physics3d/Physics3DTypes.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/scene/TilemapTypes.h"
#include "fe/scene3d/Scene3DTypes.h"

namespace fe::runtime {

void register_all_component_types() {
    reflect::register_builtin_types();
    reflect::register_tilemap_types();
    reflect::register_anim_types();
    reflect::register_physics_types();
    reflect::register_physics3d_types();
    reflect::register_scene3d_types();
    reflect::register_audio_types();
}

} // namespace fe::runtime
