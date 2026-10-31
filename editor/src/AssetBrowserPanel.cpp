// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/AssetBrowserPanel.h"

#include "fe/editor/EditorContext.h"
#include "fe/editor/EditorWidgets.h"

#include "imgui.h"

#include <string>

namespace fe::editor {
namespace {

// The payload type for dragging an asset out of the browser: the asset's stable
// reference ("guid:<uuid>"), which is what a drop target (a sprite's texture
// field, say) stores. A reference, not a pointer, so it stays valid across the
// frames between drag start and drop and across a scene reload.
constexpr const char* kAssetDragPayload = "FE_ASSET_REF";

} // namespace

void AssetBrowserPanel::draw(EditorContext& context) {
    if (ImGui::Begin(name())) {
        model_.rebuild(context.resources());

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##asset_filter", "Search assets", filter_buffer_.data(), filter_buffer_.size())) {
            model_.set_filter(std::string(filter_buffer_.data()));
        }

        // The count is the panel's header: how many assets, and how many of them
        // are shown once a filter is on.
        ImGui::TextDisabled("%zu asset%s", model_.visible_entries().size(),
                            model_.visible_entries().size() == 1 ? "" : "s");
        if (!model_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("of %zu", model_.entry_count());
        }
        ImGui::Separator();

        for (const AssetBrowserViewModel::Entry& entry : model_.visible_entries()) {
            ImGui::PushID(entry.id.to_string().c_str());

            const bool selected = model_.has_selection() && model_.selected_id() == entry.id;
            // The name and its extension are drawn as two colours, so the type
            // reads at a glance without a second column.
            if (ImGui::Selectable(("  " + entry.name).c_str(), selected)) {
                model_.select(entry.id);
            }

            // The directory is the disambiguator between two assets with the
            // same file name, so it is the tooltip rather than a second column.
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("%s", entry.path.c_str());
                if (!entry.scheme.empty()) {
                    ImGui::TextDisabled("scheme: %s", entry.scheme.c_str());
                }
                if (!entry.settings_kind.empty()) {
                    ImGui::TextDisabled("import: %s", entry.settings_kind.c_str());
                }
                ImGui::TextDisabled("drag onto a field to assign");
                ImGui::EndTooltip();
            }

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                const std::string reference = to_reference(entry.id);
                ImGui::SetDragDropPayload(kAssetDragPayload, reference.data(), reference.size());
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::EndDragDropSource();
            }

            ImGui::PopID();
        }

        if (model_.visible_entries().empty()) {
            ui::empty_hint(model_.empty() ? "No assets in this project" : "No matching assets");
        }

        // The selected asset's stable reference, shown so it can be copied into
        // a scene file or a script. This is the browser's one piece of output
        // that outlives the session.
        if (const AssetBrowserViewModel::Entry* selected = model_.selected_entry(); selected != nullptr) {
            ui::section_heading("Selected");
            ui::labelled("Name", selected->name);
            ui::labelled("Path", selected->path);
            ui::labelled("Ref", model_.selected_reference());
        }
    }
    ImGui::End();
}

} // namespace fe::editor
