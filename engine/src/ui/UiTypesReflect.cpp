// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/ui/UiTypesReflect.h"

#include "fe/reflect/Property.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/FontLoader.h"
#include "fe/resources/Resource.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/ui/UiElement.h"

#include <memory>
#include <string>
#include <vector>

// The UI component registration, in the same style as TextLabelTypes.cpp.
//
// The enums (size mode, anchor, direction, align, justify, scale mode) are exposed
// as ints because ValueKind has no enum kind, the same trade TextLabel::align and
// Collider2D::shape make. Every setter rejects an out-of-range value rather than
// coercing it, so a hand-edited scene cannot put a component in a state the
// inspector cannot represent.
//
// A `UiSize` becomes two properties ("width_mode" and "width_value") rather than a
// group, because the value's meaning depends on the mode and two named properties
// read more clearly in a document than one anonymous pair. The accessors reach
// into the pair directly, since a member pointer cannot name half a struct.
//
// The font and texture are resource references stored under their loaders'
// schemes, so a UI saved from the editor resolves them back through the ordinary
// manager path on load.

namespace fe::reflect {
namespace {

using ui::EdgeInsets;
using ui::UiAlign;
using ui::UiAnchor;
using ui::UiCanvas;
using ui::UiDirection;
using ui::UiElement;
using ui::UiJustify;
using ui::UiScaleMode;
using ui::UiSize;
using ui::UiSizeMode;

// An int-backed property for an enum member of `Class`, rejecting a value outside
// [0, count). Templated over the class so `UiElement` and `UiCanvas` share it.
template <typename Class, typename Enum>
[[nodiscard]] Property make_enum_property(std::string name, Enum Class::* field, i64 default_value, i64 count) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Int;
    property.getter = [field](const void* object) -> Value {
        return static_cast<i64>(static_cast<const Class*>(object)->*field);
    };
    property.setter = [field, count](void* object, const Value& value) -> bool {
        const auto* as_int = std::get_if<i64>(&value);
        if (as_int == nullptr || *as_int < 0 || *as_int >= count) {
            return false;
        }
        static_cast<Class*>(object)->*field = static_cast<Enum>(*as_int);
        return true;
    };
    property.default_value = default_value;
    return property;
}

// The mode half of a `UiSize` member.
[[nodiscard]] Property make_size_mode_property(std::string name, UiSize UiElement::* field) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Int;
    property.getter = [field](const void* object) -> Value {
        return static_cast<i64>((static_cast<const UiElement*>(object)->*field).mode);
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        const auto* as_int = std::get_if<i64>(&value);
        if (as_int == nullptr || *as_int < 0 || *as_int >= 4) {
            return false;
        }
        (static_cast<UiElement*>(object)->*field).mode = static_cast<UiSizeMode>(*as_int);
        return true;
    };
    property.default_value = static_cast<i64>(UiSizeMode::Fixed);
    return property;
}

// The value half of a `UiSize` member.
[[nodiscard]] Property make_size_value_property(std::string name, UiSize UiElement::* field) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::Float;
    property.getter = [field](const void* object) -> Value {
        return static_cast<f64>((static_cast<const UiElement*>(object)->*field).value);
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        const auto* as_float = std::get_if<f64>(&value);
        if (as_float == nullptr) {
            return false;
        }
        (static_cast<UiElement*>(object)->*field).value = static_cast<f32>(*as_float);
        return true;
    };
    property.default_value = 0.0;
    return property;
}

// A font resource reference. The key is the font loader's scheme-prefixed key, so
// a reload resolves the same font rather than a path that lost its scheme -- the
// same reason TextLabelTypes stores it that way.
[[nodiscard]] Property make_font_ref(std::string name, std::shared_ptr<Resource> UiElement::* field) {
    return make_resource_ref<UiElement, std::shared_ptr<Resource>>(
        std::move(name), field, "Font",
        [](const std::shared_ptr<Resource>& font) -> std::string {
            return font != nullptr ? FontLoader::reference_key(font->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); });
}

[[nodiscard]] Property make_canvas_font_ref(std::string name, std::shared_ptr<Resource> UiCanvas::* field) {
    return make_resource_ref<UiCanvas, std::shared_ptr<Resource>>(
        std::move(name), field, "Font",
        [](const std::shared_ptr<Resource>& font) -> std::string {
            return font != nullptr ? FontLoader::reference_key(font->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); });
}

[[nodiscard]] Property make_texture_ref(std::string name, std::shared_ptr<Resource> UiElement::* field) {
    return make_resource_ref<UiElement, std::shared_ptr<Resource>>(
        std::move(name), field, "Texture2D",
        [](const std::shared_ptr<Resource>& texture) -> std::string {
            return texture != nullptr ? Texture2DLoader::reference_key(texture->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); });
}

void register_ui_element(Registry& registry) {
    UiElement defaults;

    TypeDesc desc;
    desc.name = UiElement::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<UiElement>()); };

    // --- Sizing ---
    desc.properties.push_back(make_size_mode_property("width_mode", &UiElement::width));
    desc.properties.push_back(make_size_value_property("width_value", &UiElement::width));
    desc.properties.push_back(make_size_mode_property("height_mode", &UiElement::height));
    desc.properties.push_back(make_size_value_property("height_value", &UiElement::height));

    // --- Spacing ---
    desc.properties.push_back(make_vector_group<UiElement, EdgeInsets>("margin", &UiElement::margin, "EdgeInsets",
                                                                       {"left", "top", "right", "bottom"}));
    desc.properties.push_back(make_vector_group<UiElement, EdgeInsets>("padding", &UiElement::padding, "EdgeInsets",
                                                                       {"left", "top", "right", "bottom"}));

    // --- Placement ---
    desc.properties.push_back(make_enum_property<UiElement, UiAnchor>("anchor", &UiElement::anchor, 0, 9));
    desc.properties.push_back(make_vector_group<UiElement, Vec2>("offset", &UiElement::offset, "Vec2", {"x", "y"}));

    // --- Flow ---
    desc.properties.push_back(make_enum_property<UiElement, UiDirection>("direction", &UiElement::direction, 0, 3));
    desc.properties.push_back(make_scalar<UiElement, f32>("gap", &UiElement::gap, ValueKind::Float, &defaults));
    desc.properties.push_back(make_enum_property<UiElement, UiAlign>("align", &UiElement::align, 0, 4));
    desc.properties.push_back(make_enum_property<UiElement, UiJustify>("justify", &UiElement::justify, 0, 4));

    // --- Appearance ---
    desc.properties.push_back(
        with_color(make_vector_group<UiElement, Vec4>("fill", &UiElement::fill, "Vec4", {"r", "g", "b", "a"})));
    desc.properties.push_back(make_texture_ref("texture", &UiElement::texture));
    desc.properties.push_back(
        make_vector_group<UiElement, Vec4>("texture_uv", &UiElement::texture_uv, "Vec4", {"u0", "v0", "u1", "v1"}));
    desc.properties.push_back(with_color(
        make_vector_group<UiElement, Vec4>("texture_color", &UiElement::texture_color, "Vec4", {"r", "g", "b", "a"})));
    desc.properties.push_back(
        make_scalar<UiElement, std::string>("text", &UiElement::text, ValueKind::String, &defaults));
    desc.properties.push_back(make_font_ref("font", &UiElement::font));
    desc.properties.push_back(
        make_scalar<UiElement, f32>("text_size", &UiElement::text_size, ValueKind::Float, &defaults));
    desc.properties.push_back(with_color(
        make_vector_group<UiElement, Vec4>("text_color", &UiElement::text_color, "Vec4", {"r", "g", "b", "a"})));
    desc.properties.push_back(make_enum_property<UiElement, UiAlign>("text_align", &UiElement::text_align, 1, 4));
    desc.properties.push_back(make_enum_property<UiElement, UiAlign>("text_vertical", &UiElement::text_vertical, 1, 4));

    // --- Order and state ---
    desc.properties.push_back(make_scalar<UiElement, i32>("layer", &UiElement::layer, ValueKind::Int, &defaults));
    desc.properties.push_back(make_scalar<UiElement, bool>("visible", &UiElement::visible, ValueKind::Bool, &defaults));
    desc.properties.push_back(
        make_scalar<UiElement, bool>("interactable", &UiElement::interactable, ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

void register_ui_canvas(Registry& registry) {
    UiCanvas defaults;

    TypeDesc desc;
    desc.name = UiCanvas::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<UiCanvas>()); };

    desc.properties.push_back(
        make_scalar<UiCanvas, f32>("design_width", &UiCanvas::design_width, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<UiCanvas, f32>("design_height", &UiCanvas::design_height, ValueKind::Float, &defaults));
    desc.properties.push_back(make_enum_property<UiCanvas, UiScaleMode>("scale_mode", &UiCanvas::scale_mode, 0, 4));
    desc.properties.push_back(make_vector_group<UiCanvas, EdgeInsets>("padding", &UiCanvas::padding, "EdgeInsets",
                                                                      {"left", "top", "right", "bottom"}));
    desc.properties.push_back(make_scalar<UiCanvas, i32>("layer", &UiCanvas::layer, ValueKind::Int, &defaults));
    desc.properties.push_back(make_canvas_font_ref("font", &UiCanvas::font));
    desc.properties.push_back(
        make_scalar<UiCanvas, f32>("text_size", &UiCanvas::text_size, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<UiCanvas, bool>("visible", &UiCanvas::visible, ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

} // namespace

void register_ui_types() {
    Registry& registry = Registry::instance();
    register_ui_element(registry);
    register_ui_canvas(registry);
}

} // namespace fe::reflect
