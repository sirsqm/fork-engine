// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/AnimationTypes.h"

#include "fe/anim/AnimationStateMachine.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"

#include <memory>
#include <string>

// The anim component registrations, in the same style as ComponentTypes.cpp.
//
// The state machine's graph is a variable-length list of states and
// transitions. The reflection model (ADR-0005, ADR-0006) deliberately describes
// scalar leaves and fixed-size groups only: it has no list-of-struct kind,
// because the two consumers it was built for (the scene serializer and the
// inspector) only needed leaves. Rather than widen that model for one component,
// the graph is exposed as a single String property holding the component's own
// compact JSON. The scene serializer round-trips it like any other string, and
// the component parses it in its setter. The inspector shows it as a text box;
// a dedicated graph editor is a follow-up, tracked in ADR-0058.

namespace fe::reflect {
namespace {

// The graph as one string: the getter serializes the machine, the setter parses
// a document back into it. A malformed document is rejected by the setter (it
// returns false), so a hand-edited scene keeps the default rather than loading a
// half-built graph.
Property make_graph_property() {
    Property property;
    property.name = "graph";
    property.value_kind = ValueKind::String;
    property.getter = [](const void* object) -> Value {
        return static_cast<const AnimationStateMachine*>(object)->to_json();
    };
    property.setter = [](void* object, const Value& value) -> bool {
        const auto* as_string = std::get_if<std::string>(&value);
        if (as_string == nullptr) {
            return false;
        }
        return static_cast<AnimationStateMachine*>(object)->from_json(*as_string);
    };
    property.hint = Hint::Multiline;
    property.default_value = std::string{};
    return property;
}

void register_animation_state_machine(Registry& registry) {
    TypeDesc desc;
    desc.name = AnimationStateMachine::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<AnimationStateMachine>()); };
    desc.properties.push_back(make_graph_property());
    registry.register_type(std::move(desc));
}

} // namespace

void register_anim_types() {
    register_animation_state_machine(Registry::instance());
}

} // namespace fe::reflect
