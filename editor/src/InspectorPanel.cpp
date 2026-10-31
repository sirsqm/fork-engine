// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/InspectorPanel.h"

#include "fe/core/Log.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorWidgets.h"
#include "fe/editor/OperationCommand.h"
#include "fe/editor/PropertyEditor.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/reflect/TypeDesc.h"
#include "fe/reflect/Value.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Component.h"
#include "fe/scene/Node.h"
#include "fe/scene/Transform.h"
#include "fe/serialization/Operation.h"

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

// The payload type for an asset dragged out of the browser: the asset's stable
// reference ("guid:<uuid>"). This must match the constant the asset browser
// publishes under; it is the one contract between the two panels, and it is a
// reference rather than a pointer so it survives the frames between drag start
// and drop. See docs/adr/0028-editor-hint-metadata.md.
constexpr const char* kAssetDragPayload = "FE_ASSET_REF";

// Per-component edit target: which node and which component every widget in
// this section writes to. The component is named by its stable id, not its
// index, so an operation resolves to the exact component even when the node
// carries two of the same type (docs/adr/0045-component-identity.md). A widget
// records its own property *path* relative to the component (a leaf name, or a
// member path for a group), so one `SetProperty` operation covers every field
// of every type via the registry (docs/adr/0047-local-operation-model.md).
//
// The same struct also targets a gameplay script: when `script` is non-null the
// edit goes through a `SetScriptProperty` operation naming the node's behaviour
// instead of a component id, so a script's reflected fields are edited by the
// same widgets and are just as undoable (ADR-0063).
struct EditContext {
    EditorContext* context = nullptr;
    Uuid node_id{};
    Uuid component_id{};
    // Set when the rows describe a gameplay script rather than a component. The
    // script pointer is the object the property accessors read and write; the
    // behaviour name is recorded in the operation so it is a no-op if the node's
    // script changes.
    Script* script = nullptr;
    std::string behaviour;
};

// Applies an edit as a serializable operation, so it is undoable and coalesces
// with the previous edit of the same property. `path` is the full path from the
// component (or script) to the edited property.
void commit(EditContext& edit, const std::vector<std::string>& path, const Value& value) {
    if (edit.context == nullptr) {
        return;
    }
    if (edit.script != nullptr) {
        edit.context->edit_script_property(edit.node_id, edit.behaviour, path, value);
        return;
    }
    serialization::Operation op;
    op.kind = serialization::OperationKind::SetProperty;
    op.node_id = edit.node_id;
    op.component_id = edit.component_id;
    op.path = path;
    op.value = value;
    edit.context->execute(std::make_unique<OperationCommand>(std::move(op)));
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

// Commits one whole transform field as a `SetTransform` operation, so the edit is
// undoable and coalesces with the previous edit of the same field.
void commit_transform(EditorContext& context, const Uuid& node_id, const char* field, const Vec3& value) {
    serialization::Operation op;
    op.kind = serialization::OperationKind::SetTransform;
    op.node_id = node_id;
    op.path = {field};
    op.transform_value = value;
    context.execute(std::make_unique<OperationCommand>(std::move(op)));
}

// One row of the transform: three drags that edit one field (position, rotation
// or scale) as a whole, so a drag that moves two axes in a frame is one
// operation and therefore one undo step.
//
// The transform is not a component and has no reflection entry, so this is the
// one place the inspector names a transform field directly. Rotation is stored in
// radians but shown and typed in degrees, which is what a person authors in;
// `degrees` selects that conversion.
void draw_transform_row(EditorContext& context, const Uuid& node_id, Transform& transform, const char* field,
                        const char* label, f32 step, bool degrees) {
    const Vec3* current = transform_field(transform, field);
    if (current == nullptr) {
        return;
    }
    Vec3 edited = *current;
    if (degrees) {
        edited = Vec3{fe::degrees(edited.x), fe::degrees(edited.y), fe::degrees(edited.z)};
    }

    ImGui::Text("%s", label);
    const float slot_width = ImGui::GetContentRegionAvail().x / 3.0f;

    float axis_value[3] = {edited.x, edited.y, edited.z};
    const char* axis_names[3] = {"x", "y", "z"};
    bool changed = false;
    bool gesture_ended = false;
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(slot_width);
        const std::string axis_label = std::string(axis_names[i]) + "##" + field;
        if (ImGui::DragFloat(axis_label.c_str(), &axis_value[i], step) && std::isfinite(axis_value[i])) {
            changed = true;
        }
        // Checked per axis, not once after the loop: the loop's last widget is
        // the Z drag, so a gesture that ended on X or Y would otherwise go
        // unnoticed and the next drag of that axis would merge into this step.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            gesture_ended = true;
        }
        ImGui::PopID();
    }

    if (changed) {
        Vec3 written{axis_value[0], axis_value[1], axis_value[2]};
        if (degrees) {
            written = Vec3{fe::radians(written.x), fe::radians(written.y), fe::radians(written.z)};
        }
        commit_transform(context, node_id, field, written);
    }
    if (gesture_ended) {
        context.break_undo_coalescing();
    }
}

// The node's transform, drawn above its components: every node has one and
// nothing can be placed without it. This is the section that makes a node
// movable from the editor at all.
void draw_transform_section(EditorContext& context, Node& node) {
    Transform& transform = node.transform();
    const Uuid node_id = node.id();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(!context.is_editable());
        draw_transform_row(context, node_id, transform, "position", "Position", 0.01f, false);
        draw_transform_row(context, node_id, transform, "rotation", "Rotation", 0.5f, true);
        draw_transform_row(context, node_id, transform, "scale", "Scale", 0.01f, false);
        ImGui::EndDisabled();
    }
    ImGui::Separator();
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
    const ResolvedRange range = resolve_range_hint(property.range);
    int widget = static_cast<int>(std::get<i64>(property.getter(component)));

    bool changed = false;
    if (range.valid) {
        // A range hint bounds the value. The clamp is applied to the stored
        // value so a drag that overshoots the end stops at the bound.
        changed = ImGui::SliderInt(property.name.c_str(), &widget, static_cast<int>(range.minimum),
                                   static_cast<int>(range.maximum));
    } else {
        changed = ImGui::DragInt(property.name.c_str(), &widget);
    }
    if (changed) {
        const i64 bounded = static_cast<i64>(clamp_to_range(static_cast<double>(widget), range));
        commit(edit, path, Value{bounded});
    }
    end_gesture_if_finished(edit);
}

void draw_float_row(const Property& property, void* component, EditContext& edit,
                    const std::vector<std::string>& path) {
    const ResolvedRange range = resolve_range_hint(property.range);
    float widget = static_cast<float>(std::get<f64>(property.getter(component)));

    bool changed = false;
    if (range.valid) {
        // A hint with a step is a drag (a scrubby field); without one it is a
        // slider. Both are bounded, which is the point of the hint.
        if (range.step > 0.0) {
            changed = ImGui::DragFloat(property.name.c_str(), &widget, static_cast<float>(range.step),
                                       static_cast<float>(range.minimum), static_cast<float>(range.maximum));
        } else {
            changed = ImGui::SliderFloat(property.name.c_str(), &widget, static_cast<float>(range.minimum),
                                         static_cast<float>(range.maximum));
        }
    } else {
        changed = ImGui::DragFloat(property.name.c_str(), &widget, 0.01f);
    }

    // Reject a non-finite result so a stray computation cannot write inf/nan,
    // then clamp into the hint's bounds.
    if (changed && std::isfinite(widget)) {
        const double bounded = clamp_to_range(static_cast<double>(widget), range);
        commit(edit, path, Value{bounded});
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
// discard the resolved resource (see Property.h). It is edited three ways - a
// text field for the manager key, a drop target for an asset dragged from the
// browser, and a clear button - and every edit goes through a `SetResource`
// operation, so a resource reference is now undoable like any other property and
// carries as a serializable operation
// (see docs/adr/0047-local-operation-model.md).
void draw_resource_row(const Property& property, void* component, EditContext& edit, TextFields& fields,
                       const std::vector<std::string>& path) {
    const bool is_script = edit.script != nullptr;
    const std::string current = property.resource_ref ? property.resource_ref(component) : std::string{};
    sync(fields.scalar_label, fields.scalar_text, property.name, current);

    ImGui::Text("%s", property.name.c_str());
    if (property.resource && !property.resource->type_name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", property.resource->type_name.c_str());
    }

    ResourceManager* resources = edit.context != nullptr ? edit.context->resources() : nullptr;

    // A component reference goes through a `SetResource` operation naming a
    // component id; a script reference goes through `SetScriptResource`, which
    // names the behaviour instead (a script is not a component, ADR-0044). Both
    // resolve through the manager, so both are undoable and carry as a
    // serializable operation (ADR-0047, ADR-0063).
    auto commit_resource = [&](const std::string& reference) {
        if (resources == nullptr || edit.context == nullptr) {
            return;
        }
        if (is_script) {
            edit.context->edit_script_resource(edit.node_id, edit.behaviour, path, reference);
            return;
        }
        serialization::Operation op;
        op.kind = serialization::OperationKind::SetResource;
        op.node_id = edit.node_id;
        op.component_id = edit.component_id;
        op.property_name = property.name;
        op.reference = reference;
        edit.context->execute(std::make_unique<OperationCommand>(std::move(op), resources));
    };

    // The text field seeds from the current key and, on commit, routes the typed
    // reference through the operation (an empty string clears the slot).
    if (ImGui::InputTextWithHint(property.name.c_str(), "guid:... or tex:assets/...", &fields.scalar_text)) {
        commit_resource(fields.scalar_text);
    }
    end_gesture_if_finished(edit);

    // A drop target for an asset dragged out of the browser: the payload is the
    // asset's "guid:<uuid>" reference, which is exactly what resolve_resource
    // understands.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
            const std::string reference(static_cast<const char*>(payload->Data),
                                        static_cast<std::size_t>(payload->DataSize));
            commit_resource(reference);
        }
        ImGui::EndDragDropTarget();
    }

    // A clear affordance, shown only when there is something to clear.
    if (!current.empty() && edit.context != nullptr && edit.context->is_editable()) {
        ImGui::SameLine();
        const std::string clear_label = std::string("Clear##clear_") + property.name;
        if (ImGui::SmallButton(clear_label.c_str()) && resources != nullptr) {
            commit_resource(std::string{});
        }
    }

    if (resources == nullptr) {
        // Without a manager there is nothing to resolve against; the field shows
        // the current key but is not editable. This is the headless case.
        ImGui::SameLine();
        ImGui::TextDisabled("(no resource manager)");
    }
}

// A fixed-size float group (Vec2, Vec3, Vec4, or a clear colour). With a colour
// hint it is shown as a swatch plus a row of drags; otherwise just the drags.
// The hint's channel count (three for RGB, four for RGBA) comes from the group's
// member count, so one code path serves a Vec3 colour and a Vec4 one. Group
// members read and write the owning component, so `component` is what is passed
// to each member accessor.
void draw_vector_group(const Property& property, void* component, EditContext& edit,
                       const std::vector<std::string>& path) {
    ImGui::Text("%s", property.name.c_str());
    group_suffix(property);

    if (property.members.empty()) {
        return;
    }

    const usize color_channels = property.color.has_value() ? color_channel_count(property.members.size()) : 0;
    const bool as_color = color_channels > 0;
    std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
    if (as_color) {
        for (std::size_t i = 0; i < property.members.size() && i < rgba.size(); ++i) {
            rgba[i] = static_cast<float>(std::get<f64>(property.members[i].getter(component)));
        }
        const ImGuiColorEditFlags flags =
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
            (color_channels == 3 ? ImGuiColorEditFlags_NoAlpha : ImGuiColorEditFlags_None);
        const bool edited = color_channels == 3 ? ImGui::ColorEdit3("##swatch", rgba.data(), flags)
                                                : ImGui::ColorEdit4("##swatch", rgba.data(), flags);
        if (edited) {
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
        draw_resource_row(property, component, edit, fields, path);
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
        ImGui::Dummy(ImVec2{0.0f, 6.0f});
        ui::empty_hint("Select a node to inspect its components");
        ImGui::End();
        return;
    }

    // The node's identity block: its name as a heading, then its type as a badge,
    // then the stable id in the muted colour. The id is what an operation names,
    // so it is worth showing when it needs to be quoted; it is quiet enough not
    // to compete with the name.
    ImGui::Dummy(ImVec2{0.0f, 4.0f});
    ImGui::TextUnformatted(node->name().c_str());
    ImGui::SameLine();
    ui::badge(node->type_name(), ui::palette().surface_raised);
    ImGui::Separator();
    ImGui::TextDisabled("%s", node->id().to_string().c_str());

    draw_transform_section(context, *node);

    if (node->component_count() == 0) {
        ImGui::Dummy(ImVec2{0.0f, 4.0f});
        ui::empty_hint("No components on this node");
    }

    const reflect::Registry& registry = reflect::Registry::instance();

    // The component is addressed by its stable id, so every edit targets exactly
    // this component even when a node carries more than one of the same type.
    const auto& components = node->components();
    for (usize index = 0; index < components.size(); ++index) {
        Component* component = components[index].get();
        if (component == nullptr) {
            continue;
        }

        ImGui::PushID(static_cast<const void*>(component));
        ImGui::SeparatorText(component->type_name());

        // A "remove component" affordance next to the header. Routed through the
        // command stack as a serializable operation like every other edit.
        if (context.is_editable()) {
            ImGui::SameLine();
            const std::string button_label = std::string("Remove##remove_") + component->type_name();
            if (ImGui::SmallButton(button_label.c_str())) {
                serialization::Operation op;
                op.kind = serialization::OperationKind::RemoveComponent;
                op.node_id = node->id();
                op.component_id = component->id();
                context.execute(std::make_unique<OperationCommand>(std::move(op)));
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
        edit.component_id = component->id();

        std::vector<std::string> empty_path;
        for (const Property& property : type->properties) {
            draw_row(property, component, edit, fields_, empty_path);
        }
        ImGui::PopID();
    }

    // A "Scripts" section: the behaviour bound to this node, its reflected
    // properties, and an attach/remove control. Scripts are not components
    // (ADR-0044), so they are drawn from the document's `ScriptCatalog` rather
    // than from the node, but a script's properties are ordinary
    // `reflect::Property` rows (ADR-0063) so the same widgets edit them.
    ImGui::SeparatorText("Scripts");
    ScriptCatalog& catalog = context.scripts();
    const std::string bound = catalog.behaviour_of(node->id());

    if (bound.empty()) {
        ImGui::TextDisabled("No script");
    } else {
        ImGui::Text("%s", bound.c_str());
        if (context.is_editable()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove##remove_script")) {
                catalog.detach(node->id());
            }
        }
        if (const ScriptDesc* desc = catalog.desc_of(node->id())) {
            if (Script* script = catalog.script(node->id())) {
                // A script's fields are drawn by the same widgets a component's
                // are, through an EditContext that names the script instead of a
                // component id. An edit becomes a `SetScriptProperty` operation,
                // so it is undoable and coalesces like any other property edit
                // (ADR-0047, ADR-0063).
                EditContext edit;
                edit.context = &context;
                edit.node_id = node->id();
                edit.script = script;
                edit.behaviour = bound;

                std::vector<std::string> empty_path;
                for (const Property& property : desc->properties) {
                    draw_row(property, script, edit, fields_, empty_path);
                }
            }
        }
    }

    // An "attach script" palette: every registered behaviour the node is not
    // already running. Registered by name, so a module's behaviour appears here
    // with no edit to this file (ADR-0061).
    if (context.is_editable() && ImGui::Button("Attach Script")) {
        ImGui::OpenPopup("##attach_script");
    }
    if (ImGui::BeginPopup("##attach_script")) {
        for (const std::string& behaviour : catalog.behaviour_names()) {
            if (behaviour == bound) {
                continue;
            }
            if (ImGui::MenuItem(behaviour.c_str())) {
                if (Error error = catalog.attach(*node, behaviour)) {
                    FE_LOG_WARN("editor: could not attach '%s': %s", behaviour.c_str(), error.message().c_str());
                }
            }
        }
        ImGui::EndPopup();
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
                serialization::Operation op;
                op.kind = serialization::OperationKind::AddComponent;
                op.node_id = node->id();
                op.component_id = Uuid::generate();
                op.index = node->component_count();
                op.name = type.name;
                context.execute(std::make_unique<OperationCommand>(std::move(op)));
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace fe::editor
