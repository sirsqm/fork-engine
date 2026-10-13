// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/InspectorPanel.h"

#include "fe/editor/Commands.h"
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
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace fe::editor {
namespace {

using reflect::Hint;
using reflect::Property;
using reflect::Value;
using reflect::ValueKind;

using TextFields = InspectorPanel::TextFields;

// Per-component edit target: which node and which component index every widget
// in this section writes to. A widget records its own property *path* relative
// to the component (a leaf name, or a member path for a group), so one
// `SetPropertyCommand` covers every field of every type via the registry
// (docs/adr/0015-editor-undo-redo.md).
struct EditContext {
    EditorContext* context = nullptr;
    Uuid node_id{};
    usize component_index = 0;
};

// Applies an edit through the command stack, so it is undoable and coalesces
// with the previous edit of the same property. `path` is the full path from the
// component to the edited property.
void commit(EditContext& edit, const std::vector<std::string>& path, const Value& value) {
    if (edit.context == nullptr) {
        return;
    }
    edit.context->execute(std::make_unique<SetPropertyCommand>(edit.node_id, edit.component_index, path, value));
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

// Ends the current coalescing group when the widget that was just drawn finished
// its edit gesture (the drag ended, the field lost focus). Without this two
// separate gestures on the same property would merge into one undo step.
void end_gesture_if_finished(EditContext& edit) {
    if (edit.context != nullptr && ImGui::IsItemDeactivatedAfterEdit()) {
        edit.context->break_undo_coalescing();
    }
}

void draw_row(const Property& property, void* component, EditContext& edit, TextFields& fields,
              std::vector<std::string>& path);

void draw_bool_row(const Property& property, void* component, EditContext& edit, const std::vector<std::string>& path) {
    bool widget = std::get<bool>(property.getter(component));
    if (ImGui::Checkbox(property.name.c_str(), &widget)) {
        commit(edit, path, Value{widget});
    }
    end_gesture_if_finished(edit);
}

void draw_int_row(const Property& property, void* component, EditContext& edit, const std::vector<std::string>& path) {
    int widget = static_cast<int>(std::get<i64>(property.getter(component)));
    if (ImGui::DragInt(property.name.c_str(), &widget)) {
        commit(edit, path, Value{static_cast<i64>(widget)});
    }
    end_gesture_if_finished(edit);
}

void draw_float_row(const Property& property, void* component, EditContext& edit,
                    const std::vector<std::string>& path) {
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
        commit(edit, path, Value{static_cast<f64>(widget)});
    }
    end_gesture_if_finished(edit);
}

void draw_string_row(const Property& property, void* component, EditContext& edit, TextFields& fields,
                     const std::vector<std::string>& path) {
    const std::string current = std::get<std::string>(property.getter(component));
    const bool multiline = property.hint == Hint::Multiline;
    std::string& label = multiline ? fields.multiline_label : fields.scalar_label;
    std::string& text = multiline ? fields.multiline_text : fields.scalar_text;
    sync(label, text, property.name, current);

    const bool changed = multiline ? ImGui::InputTextMultiline(property.name.c_str(), &text)
                                   : ImGui::InputText(property.name.c_str(), &text);
    if (changed) {
        commit(edit, path, Value{text});
    }
    end_gesture_if_finished(edit);
}

// A resource reference is read through `resource_ref` and written through
// `resolve_resource`: the inspector never assigns a raw key, because that would
// discard the resolved resource (see Property.h). A text field for the manager
// key is enough for this milestone; an asset drop target is a later refinement.
//
// This path is deliberately not a `SetPropertyCommand`: resolving a key needs a
// `ResourceManager`, which the command does not carry. Resource references are
// therefore not yet undoable; that is recorded as remaining work.
void draw_resource_row(const Property& property, void* component, EditContext& edit, TextFields& fields) {
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

    ResourceManager* resources = edit.context != nullptr ? edit.context->resources() : nullptr;
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
void draw_vector_group(const Property& property, void* component, EditContext& edit,
                       const std::vector<std::string>& path) {
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
                    std::vector<std::string> member_path = path;
                    member_path.push_back(property.members[i].name);
                    commit(edit, member_path, Value{static_cast<f64>(rgba[i])});
                }
            }
        }
        end_gesture_if_finished(edit);
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
            std::vector<std::string> member_path = path;
            member_path.push_back(member.name);
            commit(edit, member_path, Value{static_cast<f64>(widget)});
        }
        end_gesture_if_finished(edit);
        ImGui::PopID();
    }
}

// A named struct group: a collapsible header with one row per member.
void draw_named_group(const Property& property, void* component, EditContext& edit, TextFields& fields,
                      std::vector<std::string>& path) {
    ImGui::PushID(property.name.c_str());
    if (ImGui::CollapsingHeader(property.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const Property& member : property.members) {
            draw_row(member, component, edit, fields, path);
        }
    }
    ImGui::PopID();
}

void draw_leaf(const Property& property, void* component, EditContext& edit, TextFields& fields,
               const std::vector<std::string>& path) {
    switch (property.value_kind) {
    case ValueKind::Bool:
        draw_bool_row(property, component, edit, path);
        return;
    case ValueKind::Int:
        draw_int_row(property, component, edit, path);
        return;
    case ValueKind::Float:
        draw_float_row(property, component, edit, path);
        return;
    case ValueKind::String:
        draw_string_row(property, component, edit, fields, path);
        return;
    case ValueKind::Uuid:
        // Read-only this milestone: an identity field is not retyped by hand,
        // and editing it has scene-wide consequences.
        ImGui::Text("%s", property.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", format_value(property.getter(component)).c_str());
        return;
    case ValueKind::ResourceRef:
        draw_resource_row(property, component, edit, fields);
        return;
    case ValueKind::Group:
        return; // Handled by draw_row.
    }
}

// `path` is the path of the container that owns `property`. The property's own
// full path (for the undo command) is `path` plus the property's name.
void draw_row(const Property& property, void* component, EditContext& edit, TextFields& fields,
              std::vector<std::string>& path) {
    std::vector<std::string> full = path;
    full.push_back(property.name);

    if (property.value_kind != ValueKind::Group) {
        draw_leaf(property, component, edit, fields, full);
    } else if (property.array_length > 0) {
        draw_vector_group(property, component, edit, full);
    } else {
        draw_named_group(property, component, edit, fields, full);
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

    // The component's index, so every edit targets exactly this component even
    // when a node carries more than one of the same type.
    const auto& components = node->components();
    for (usize index = 0; index < components.size(); ++index) {
        Component* component = components[index].get();
        if (component == nullptr) {
            continue;
        }

        ImGui::PushID(static_cast<const void*>(component));
        ImGui::SeparatorText(component->type_name());

        // A "remove component" affordance next to the header. Routed through the
        // command stack like every other edit.
        if (context.is_editable()) {
            ImGui::SameLine();
            const std::string button_label = std::string("Remove##remove_") + component->type_name();
            if (ImGui::SmallButton(button_label.c_str())) {
                context.execute(std::make_unique<RemoveComponentCommand>(node->id(), index));
                ImGui::PopID();
                break; // The component vector changed; stop iterating it.
            }
        }

        const reflect::TypeDesc* type = registry.find(component->type_name());
        if (type == nullptr) {
            // A component whose type is not registered is shown, not edited, and
            // never crashes the panel.
            ImGui::TextDisabled("%s", unreflected_label(component->type_name()).c_str());
            ImGui::PopID();
            continue;
        }

        EditContext edit;
        edit.context = &context;
        edit.node_id = node->id();
        edit.component_index = index;

        std::vector<std::string> empty_path;
        for (const Property& property : type->properties) {
            draw_row(property, component, edit, fields_, empty_path);
        }
        ImGui::PopID();
    }

    // An "add component" palette: every registered type that the node does not
    // already have. Reflected the same way the rows are, so a newly registered
    // component appears here with no edit to this file.
    if (context.is_editable() && ImGui::Button("Add Component")) {
        ImGui::OpenPopup("##add_component");
    }
    if (ImGui::BeginPopup("##add_component")) {
        for (const reflect::TypeDesc& type : registry.types()) {
            bool already_present = false;
            for (const auto& owned : node->components()) {
                if (owned != nullptr && owned->type_name() == type.name) {
                    already_present = true;
                    break;
                }
            }
            if (already_present) {
                continue;
            }
            if (ImGui::MenuItem(type.name.c_str())) {
                context.execute(std::make_unique<AddComponentCommand>(node->id(), type.name, node->component_count()));
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace fe::editor
