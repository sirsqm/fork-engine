// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/Panel.h"

#include <string>

namespace fe::editor {

// The property inspector: edits the selected node's components through the
// reflection registry.
//
// This is the payoff of the reflection subsystem. The panel holds no
// per-component knowledge: for the selected node it walks the node's
// components, asks `reflect::Registry` for each component's `TypeDesc`, and
// draws one row per `Property` using the property's own getter/setter and
// editor `Hint`. Adding a property to a component's registration makes it
// appear here with no edit to this file.
//
// The data flow is:
//
//   Node -> component -> Registry::find(type_name) -> TypeDesc::properties
//        -> Property (getter/setter/hint) -> widget -> setter
//
// A component whose type is not registered is shown by name with a
// "(not reflected)" note and is not editable; the panel never assumes it can
// reflect a type. Resource references are read through `Property::resource_ref`
// and written through `Property::resolve_resource`, which needs a
// `ResourceManager`, so the context carries one (null while its owning engine is
// absent, which is exactly the headless test case).
class InspectorPanel final : public Panel {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Inspector"; }

    void draw(EditorContext& context) override;

    // Text-field scratch buffers, one pair per kind of editable text (a plain
    // string and a Hint::Multiline string). ImGui's text widgets edit a mutable
    // buffer that has to outlive the frame, so it lives on the panel rather than
    // on the stack. Public so the file-local drawing helpers can take it.
    struct TextFields {
        std::string scalar_label;
        std::string scalar_text;
        std::string multiline_label;
        std::string multiline_text;
    };

private:
    TextFields fields_;
};

} // namespace fe::editor
