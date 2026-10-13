// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/reflect/TypeDesc.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Component;

namespace reflect {

// The global registry of reflected types.
//
// Every component type that the serializer, the inspector, or a future script
// binding needs to see registers a TypeDesc here once, at startup. From then on
// those consumers work purely through the registry: an unknown type name is a
// lookup miss rather than a missing branch in three different switch statements.
//
// The registry is process-global and is populated before the first scene is
// loaded. It is intentionally not owned by Scene or Engine: a type is a
// property of the build, not of a particular scene, and the editor will look
// types up without necessarily holding either.
//
// Threading: registration happens during single-threaded startup. After that
// the registry is read-only and safe to read from any thread.
class Registry {
public:
    // The process-wide registry.
    [[nodiscard]] static Registry& instance();

    // Registers (or replaces) a type. A later registration of the same name
    // replaces the earlier one, which keeps re-running a test binary in the
    // same process well defined.
    void register_type(TypeDesc desc);

    [[nodiscard]] const TypeDesc* find(std::string_view name) const noexcept;
    [[nodiscard]] bool contains(std::string_view name) const noexcept { return find(name) != nullptr; }

    // Every registered type, in registration order.
    [[nodiscard]] const std::vector<TypeDesc>& types() const noexcept { return types_; }
    [[nodiscard]] usize type_count() const noexcept { return types_.size(); }

    // Constructs a default instance of the named type, or nullptr if the type
    // is not registered.
    [[nodiscard]] std::unique_ptr<Component> default_construct(std::string_view name) const;

private:
    Registry() = default;

    std::vector<TypeDesc> types_;
};

} // namespace reflect
} // namespace fe
