// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/TextLabelTypes.h"

#include "fe/reflect/Property.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/FontLoader.h"
#include "fe/resources/Resource.h"
#include "fe/scene/TextLabel.h"

#include <memory>
#include <string>

// The text component registration, in the same style as TilemapTypes.cpp.
//
// `align` is exposed as an int because ValueKind has no enum kind, the same
// trade Collider2D's `shape` makes: the setter clamps to the valid range, so a
// hand-edited scene cannot put the component in an out-of-range state.
//
// The font is a resource reference stored under the font loader's scheme, so a
// label saved from the editor resolves its font back through the ordinary
// manager path on load.

namespace fe::reflect {
namespace {

Property make_align_property() {
    Property property;
    property.name = "align";
    property.value_kind = ValueKind::Int;
    property.getter = [](const void* object) -> Value {
        return static_cast<i64>(static_cast<const TextLabel*>(object)->align);
    };
    property.setter = [](void* object, const Value& value) -> bool {
        const auto* as_int = std::get_if<i64>(&value);
        if (as_int == nullptr) {
            return false;
        }
        auto* label = static_cast<TextLabel*>(object);
        if (*as_int <= static_cast<i64>(TextLabel::Align::Left)) {
            label->align = TextLabel::Align::Left;
        } else if (*as_int >= static_cast<i64>(TextLabel::Align::Right)) {
            label->align = TextLabel::Align::Right;
        } else {
            label->align = TextLabel::Align::Center;
        }
        return true;
    };
    property.default_value = static_cast<i64>(TextLabel::Align::Left);
    return property;
}

void register_text(Registry& registry) {
    TextLabel defaults;

    TypeDesc desc;
    desc.name = TextLabel::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<TextLabel>()); };

    desc.properties.push_back(
        make_scalar<TextLabel, std::string>("text", &TextLabel::text, ValueKind::String, &defaults));
    desc.properties.push_back(make_scalar<TextLabel, f32>("size", &TextLabel::size, ValueKind::Float, &defaults));
    desc.properties.push_back(
        with_color(make_vector_group<TextLabel, Vec4>("color", &TextLabel::color, "Vec4", {"r", "g", "b", "a"})));
    desc.properties.push_back(make_align_property());
    desc.properties.push_back(make_scalar<TextLabel, i32>("layer", &TextLabel::layer, ValueKind::Int, &defaults));
    desc.properties.push_back(make_resource_ref<TextLabel, std::shared_ptr<Resource>>(
        "font", &TextLabel::font, "Font",
        [](const std::shared_ptr<Resource>& font) -> std::string {
            // Store the manager cache key (including the "font:" scheme) rather
            // than the resource's own path, so a reload resolves the same font
            // rather than a path that lost its scheme.
            return font != nullptr ? FontLoader::reference_key(font->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));

    registry.register_type(std::move(desc));
}

} // namespace

void register_text_types() {
    register_text(Registry::instance());
}

} // namespace fe::reflect
