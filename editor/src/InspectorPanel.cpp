// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/InspectorPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/PropertyEditor.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/reflect/Value.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <variant>

namespace fe::editor {
namespace {

using reflect::Hint;
using reflect::Property;
using reflect::Value;
using reflect::ValueKind;

using TextFields = InspectorPanel::TextFields;

// True when `property` writes a value of the kind produced by `value`.
[[nodiscard]] bool setter_accepts(const Property& property, const Value& value) {
    if (property.value_kind == ValueKind::ResourceRef) {
        return std::holds_alternative<std::string>(value);
    }
    return reflect::kind_of(value) == property.value_kind;
}

// Calls the setter and reports whether the value was accepted. Callers that
// care can check the result; a rejected write is never applied.
bool apply(const Property& property, void* component, const Value& value) {
    return property.setter && setter_accepts(property, value) && property.setter(component, value);
}

void group_suffix(const Property& property) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", property.group_type.c_str());
}

// Re-seeds a buffer when the label or content changed since last frame.
void sync(std::string& label, std::string& text, const std::string& want_label, const std::string& want_text) {
    if (label != want_label || text != want_text) {
        label = want_label;
        text = want_text;
    }
}

void draw_row(const Property& property, void* component, EditorContext* context, TextFields& fields);

void draw_bool_row(const Property& property, void* component) {
    bool widget = std::get<bool>(property.getter(component));
    if (ImGui::Checkbox(property.name.c_str(), &widget)) {
        apply(property, component, Value{widget});
    }
}

void draw_int_row(const Property& property, void* component) {
    int widget = static_cast<int>(std::get<i64>(property.getter(component)));
    if (ImGui::DragInt(property.name.c_str(), &widget)) {
        apply(property, component, Value{static_cast<i64>(widget)});
    }
}

void draw_float_row(const Property& property, void* component) {
    float widget = static_cast<float>(std::get<f64>(property.getter(component)));

    bool changed = false;
    if (property.hint == Hint::Range) {
        const RangeHint range = parse_range_hint(property.hint_text);
        if (range.valid) {
            changed = ImGui::SliderFloat(property.name.c_str(), &widget, static_cast<float>(range.minimum),
                                         static_cast<float>(range.maximum));
        } else {
            // A malformed range hint degrades to a plain drag, so a typo in a
            // registration yields a usable widget rather than a broken slider.
            changed = ImGui::DragFloat(property.name.c_str(), &widget, 0.01f);
        }
    } else {
        changed = ImGui::DragFloat(property.name.c_str(), &widget, 0.01f);
    }

    // Reject a non-finite result so a stray computation cannot write inf/nan.
    if (changed && std::isfinite(widget)) {
        apply(property, component, Value{static_cast<f64>(widget)});
    }
}

void draw_string_row(const Property& property, void* component, TextFields& fields) {
    const std::string current = std::get<std::string>(property.getter(component));
    const bool multiline = property.hint == Hint::Multiline;
    std::string& label = multiline ? fields.multiline_label : fields.scalar_label;
    std::string& text = multiline ? fields.multiline_text : fields.scalar_text;
    sync(label, text, property.name, current);

    const bool changed = multiline ? ImGui::InputTextMultiline(property.name.c_str(), &text)
                                   : ImGui::InputText(property.name.c_str(), &text);
    if (changed) {
        apply(property, component, Value{text});
    }
}

// A resource reference is read through `resource_ref` and written through
// `resolve_resource`: the inspector never assigns a raw key, because that would
// discard the resolved resource (see Property.h). A text field for the manager
// key is enough for this milestone; an asset drop target is a later refinement.
void draw_resource_row(const Property& property, void* component, EditorContext* context, TextFields& fields) {
    const std::string current = property.resource_ref ? property.resource_ref(component) : std::string{};
    sync(fields.scalar_label, fields.scalar_text, property.name, current);

    ImGui::Text("%s", property.name.c_str());
    if (!property.resource_type.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", property.resource_type.c_str());
    }

    if (!ImGui::InputTextWithHint(property.name.c_str(), "tex:assets/...", &fields.scalar_text)) {
        return;
    }

    ResourceManager* resources = context != nullptr ? context->resources() : nullptr;
    if (resources == nullptr || !property.resolve_resource) {
        // Without a manager there is nothing to resolve against; the typed text
        // stays visible but the reference is not claimed as applied.
        return;
    }
    // On a failed resolve the field keeps the user's text so they can correct
    // it, and no stale resource is stored.
    property.resolve_resource(component, fields.scalar_text, *resources);
}

// A fixed-size float group (Vec2, Vec4, or an RGBA clear colour). With
// Hint::Color it is shown as a swatch plus a row of drags; otherwise just the
// drags. Group members read and write the owning component, so `component` is
// what is passed to each member accessor.
void draw_vector_group(const Property& property, void* component) {
    ImGui::Text("%s", property.name.c_str());
    group_suffix(property);

    if (property.members.empty()) {
        return;
    }

    const bool as_color = property.hint == Hint::Color && property.members.size() >= 3;
    std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
    if (as_color) {
        for (std::size_t i = 0; i < property.members.size() && i < rgba.size(); ++i) {
            rgba[i] = static_cast<float>(std::get<f64>(property.members[i].getter(component)));
        }
        if (ImGui::ColorEdit4("##swatch", rgba.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            for (std::size_t i = 0; i < property.members.size() && i < rgba.size(); ++i) {
                if (std::isfinite(rgba[i])) {
                    apply(property.members[i], component, Value{static_cast<f64>(rgba[i])});
                }
            }
        }
        ImGui::SameLine();
    }

    const float slot_width = ImGui::GetContentRegionAvail().x / static_cast<float>(property.members.size());
    for (std::size_t i = 0; i < property.members.size(); ++i) {
        const Property& member = property.members[i];
        ImGui::PushID(static_cast<int>(i));
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(slot_width);

        float widget = static_cast<float>(std::get<f64>(member.getter(component)));
        if (ImGui::DragFloat(member.name.c_str(), &widget, 0.01f) && std::isfinite(widget)) {
            apply(member, component, Value{static_cast<f64>(widget)});
        }
        ImGui::PopID();
    }
}

// A named struct group: a collapsible header with one row per member.
void draw_named_group(const Property& property, void* component, EditorContext* context, TextFields& fields) {
    ImGui::PushID(property.name.c_str());
    if (ImGui::CollapsingHeader(property.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const Property& member : property.members) {
            draw_row(member, component, context, fields);
        }
    }
    ImGui::PopID();
}

void draw_leaf(const Property& property, void* component, EditorContext* context, TextFields& fields) {
    switch (property.value_kind) {
    case ValueKind::Bool:
        draw_bool_row(property, component);
        return;
    case ValueKind::Int:
        draw_int_row(property, component);
        return;
    case ValueKind::Float:
        draw_float_row(property, component);
        return;
    case ValueKind::String:
        draw_string_row(property, component, fields);
        return;
    case ValueKind::Uuid:
        // Read-only this milestone: an identity field is not retyped by hand,
        // and editing it has scene-wide consequences that undo/redo will own.
        ImGui::Text("%s", property.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", format_value(property.getter(component)).c_str());
        return;
    case ValueKind::ResourceRef:
        draw_resource_row(property, component, context, fields);
        return;
    case ValueKind::Group:
        return; // Handled by draw_row.
    }
}

void draw_row(const Property& property, void* component, EditorContext* context, TextFields& fields) {
    if (property.value_kind != ValueKind::Group) {
        draw_leaf(property, component, context, fields);
    } else if (property.array_length > 0) {
        draw_vector_group(property, component);
    } else {
        draw_named_group(property, component, context, fields);
    }
}

} // namespace

void InspectorPanel::draw(EditorContext& context) {
    // The registry is populated by Engine::initialize via
    // register_builtin_types(). Calling it again is idempotent and covers the
    // case where the panel is drawn without an Engine (a test).
    reflect::register_builtin_types();

    if (!ImGui::Begin(name())) {
        ImGui::End();
        return;
    }

    Node* node = context.hierarchy().selected_node();
    if (node == nullptr) {
        ImGui::TextDisabled("Select a node to inspect its components");
        ImGui::End();
        return;
    }

    ImGui::Text("%s", node->name().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", node->type_name());
    ImGui::Separator();

    if (node->component_count() == 0) {
        ImGui::TextDisabled("No components");
    }

    const reflect::Registry& registry = reflect::Registry::instance();
    for (const auto& owned : node->components()) {
        Component* component = owned.get();
        if (component == nullptr) {
            continue;
        }

        ImGui::PushID(static_cast<const void*>(component));
        ImGui::SeparatorText(component->type_name());

        const reflect::TypeDesc* type = registry.find(component->type_name());
        if (type == nullptr) {
            // A component whose type is not registered is shown, not edited, and
            // never crashes the panel.
            ImGui::TextDisabled("%s", unreflected_label(component->type_name()).c_str());
            ImGui::PopID();
            continue;
        }

        for (const Property& property : type->properties) {
            draw_row(property, component, &context, fields_);
        }
        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace fe::editor
