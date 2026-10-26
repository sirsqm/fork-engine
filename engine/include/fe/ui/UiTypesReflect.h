// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

namespace fe::reflect {

// Registers the in-game UI components with the reflection registry, so the scene
// serializer persists them and the inspector can edit them, exactly as
// register_text_types does for a label (ADR-0086).
//
// Both `UiCanvas` and `UiElement` are registered: a canvas is a component like any
// other, so a UI is authored in the same scene graph, saved by the same
// serializer, and undone by the same operation log as the rest of the scene. That
// is the whole reason UI is components rather than a parallel widget tree.
void register_ui_types();

} // namespace fe::reflect
