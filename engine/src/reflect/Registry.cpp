// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/reflect/Registry.h"

#include "fe/scene/Component.h"

#include <algorithm>
#include <utility>

namespace fe::reflect {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::register_type(TypeDesc desc) {
    auto it = std::find_if(types_.begin(), types_.end(),
                           [&desc](const TypeDesc& existing) { return existing.name == desc.name; });
    if (it != types_.end()) {
        *it = std::move(desc);
        return;
    }
    types_.push_back(std::move(desc));
}

const TypeDesc* Registry::find(std::string_view name) const noexcept {
    auto it = std::find_if(types_.begin(), types_.end(), [name](const TypeDesc& type) { return type.name == name; });
    return it == types_.end() ? nullptr : &*it;
}

std::unique_ptr<Component> Registry::default_construct(std::string_view name) const {
    const TypeDesc* type = find(name);
    if (type == nullptr || !type->factory) {
        return nullptr;
    }
    return type->factory();
}

} // namespace fe::reflect
