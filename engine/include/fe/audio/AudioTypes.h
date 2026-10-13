// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the audio component types (AudioSource) with the reflection
// registry.
//
// Kept separate from register_builtin_types() so that the reflection layer does
// not have to depend on the audio module. The engine calls this during startup,
// exactly as it calls register_physics_types(); a tool or a test that uses audio
// without an Engine calls this directly. Idempotent, like the built-in
// registration.
void register_audio_types();

} // namespace fe::reflect
