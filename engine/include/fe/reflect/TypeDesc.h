// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/reflect/Property.h"
#include "fe/scene/Component.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fe::reflect {

// The description of one registered component type.
//
// The factory is what removes per-type knowledge from the serializer: given a
// type name from a document, the serializer asks the registry for a TypeDesc,
// calls its factory to construct the component, and applies each property.
// Nothing in the serialization layer names Sprite2D or Camera2D any more.
//
// reflect depends on scene for the Component base. That is a deliberate edge:
// reflection exists to serve the scene, the serializer, and the editor, all of
// which are component-centric. Keeping the factory typed as Component* is what
// lets the serializer attach a runtime-known type without a switch.
struct TypeDesc {
    std::string name;
    std::function<std::unique_ptr<Component>()> factory;
    std::vector<Property> properties;

    [[nodiscard]] bool is_valid() const noexcept { return !name.empty() && static_cast<bool>(factory); }
};

} // namespace fe::reflect
