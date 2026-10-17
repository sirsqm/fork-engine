// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/reflect/Property.h"
#include "fe/script/Script.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fe {

// The description of one registered behaviour.
//
// This mirrors `reflect::TypeDesc` (ADR-0006) for gameplay scripts, and it is
// what lets a script be authored like a component rather than like code: the
// factory constructs the instance, and `properties` says which of its fields
// are visible. The inspector draws them and the scene serializer persists them
// through the same `reflect::Property` accessors a component uses, so a script
// property is one registration edit and it both persists and appears in the
// editor (ADR-0063).
//
// A property's accessors take the owning object pointer, which here is the
// `Script`. A script author therefore builds them with the ordinary
// `make_scalar`/`make_vector_group` helpers, naming the concrete script type,
// exactly as a component registration does.
//
// Scripts with no properties are the common case and leave `properties` empty;
// such a script still round-trips (its behaviour name is recorded) but carries
// no data of its own.
struct ScriptDesc {
    std::string name;
    std::function<std::unique_ptr<Script>()> factory;
    std::vector<reflect::Property> properties;

    [[nodiscard]] bool is_valid() const noexcept { return !name.empty() && static_cast<bool>(factory); }
};

} // namespace fe
