// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/FileDialogPanel.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/editor/EditorContext.h"
#include "fe/editor/ProjectSettings.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <string>

namespace fe::editor {
namespace {

// The editor's scene file extension and the project file name. Kept here rather
// than in the view model, which is deliberately ignorant of what the editor
// deals in.
constexpr const char* kSceneExtension = ".fescene";

// A modest size that fits the panels' column widths without dominating the
// window. A file dialog is a modal, so a fixed size is fine.
constexpr ImVec2 kDialogSize{640.0f, 440.0f};

} // namespace

const char* FileDialogPanel::filter_for(Purpose purpose) noexcept {
    switch (purpose) {
    case Purpose::OpenScene:
    case Purpose::SaveScene:
        return kSceneExtension;
    case Purpose::OpenProject:
        return ".json";
    }
    return "";
}

const char* FileDialogPanel::popup_id() const noexcept {
    return "##fork_file_dialog";
}

const char* FileDialogPanel::title() const noexcept {
    switch (purpose_) {
    case Purpose::OpenScene:
        return "Open Scene";
    case Purpose::SaveScene:
        return "Save Scene As";
    case Purpose::OpenProject:
        return "Open Project";
    }
    return "File";
}

const char* FileDialogPanel::confirm_label() const noexcept {
    return purpose_ == Purpose::SaveScene ? "Save" : "Open";
}

void FileDialogPanel::open_scene(std::string start_directory) {
    purpose_ = Purpose::OpenScene;
    error_.clear();
    model_.open(FileDialogViewModel::Mode::OpenFile, std::move(start_directory), {}, filter_for(purpose_));
}

void FileDialogPanel::open_save_scene(std::string start_directory, std::string start_name) {
    purpose_ = Purpose::SaveScene;
    error_.clear();
    model_.open(FileDialogViewModel::Mode::SaveFile, std::move(start_directory), std::move(start_name),
                filter_for(purpose_));
}

void FileDialogPanel::open_project(std::string start_directory) {
    purpose_ = Purpose::OpenProject;
    error_.clear();
    model_.open(FileDialogViewModel::Mode::OpenFile, std::move(start_directory),
                std::string(ProjectSettings::kDefaultFileName), filter_for(purpose_));
}

Error FileDialogPanel::confirm(EditorContext& context) {
    if (!model_.is_open()) {
        return Error("file dialog is not open");
    }
    const std::string path = model_.selected_path();
    if (path.empty()) {
        return Error("no file name given");
    }
    // In save mode, a bare name with no extension gets the dialog's extension, so
    // "level" saves as "level.fescene" rather than a file the editor cannot find
    // again. An explicit different extension is respected.
    std::string target = path;
    if (purpose_ == Purpose::SaveScene && fs::extension(target).empty()) {
        target += kSceneExtension;
    }

    switch (purpose_) {
    case Purpose::OpenScene: {
        if (Error error = context.load_scene(target, context.resources()); error) {
            return error;
        }
        break;
    }
    case Purpose::SaveScene: {
        if (Error error = context.save_scene(target); error) {
            return error;
        }
        break;
    }
    case Purpose::OpenProject: {
        if (Error error = context.load_project(target, context.resources()); error) {
            return error;
        }
        // Opening a project adopts its settings but not its scene: load the
        // startup scene if it names one, so "Open Project" actually shows the
        // project rather than leaving the previous document on screen.
        const std::string startup = context.project().resolved_startup_scene();
        if (!startup.empty()) {
            if (Error error = context.load_scene(startup, context.resources()); error) {
                // The project opened; the scene failing is worth reporting but is
                // not a failure to open the project.
                FE_LOG_WARN("editor: project opened but its startup scene failed: %s", error.message().c_str());
            }
        }
        break;
    }
    }

    model_.close();
    error_.clear();
    return Error::none();
}

void FileDialogPanel::draw(EditorContext& context) {
    if (!model_.is_open()) {
        return;
    }

    if (!ImGui::IsPopupOpen(popup_id(), ImGuiPopupFlags_None)) {
        // A fixed size and a centred position make the modal read like a dialog
        // rather than a floating window.
        const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(kDialogSize, ImGuiCond_Appearing);
        ImGui::OpenPopup(popup_id());
    }

    if (!ImGui::BeginPopupModal(popup_id(), nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }

    // The model may have changed under us (a test, or a programmatic open), so
    // re-sync the widget buffers whenever the model reports a change. This is the
    // "read the model, emit widgets" contract: the buffers exist only because
    // ImGui needs stable storage for the text being edited.
    if (model_.changed()) {
        directory_buffer_ = model_.directory();
        file_name_buffer_ = model_.file_name();
        filter_buffer_ = model_.filter();
        model_.clear_changed();
    }

    ImGui::TextUnformatted(title());
    ImGui::Separator();

    // The current directory, with an Up button. A text field would need an
    // "enter" that could be a non-directory; a button plus the listing is
    // unambiguous, and the listing is the primary way to move.
    if (ImGui::Button("Up")) {
        if (!model_.go_up()) {
            // Already at a root; nothing to do.
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", directory_buffer_.c_str());

    // Filter box.
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "Filter", &filter_buffer_)) {
        model_.set_filter(filter_buffer_);
    }

    // The listing, scrollable. A fixed height leaves room for the file-name box
    // and the buttons below.
    const float listing_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f;
    if (ImGui::BeginChild("##listing", ImVec2(0.0f, listing_height > 0.0f ? listing_height : 100.0f), true)) {
        for (const FileDialogViewModel::Row& row : model_.rows()) {
            const bool selected = !row.is_directory && row.name == file_name_buffer_;
            // A directory row is a navigation action; a file row selects. The
            // label carries a folder marker so the two are distinguishable at a
            // glance without an icon font.
            std::string label = row.is_directory ? std::string("[ ] ") + row.name : row.name;
            if (row.is_directory) {
                if (ImGui::Selectable(label.c_str())) {
                    model_.enter(row.name);
                    // enter() clears the model's file name; keep the buffer in
                    // step so the selection highlight does not linger.
                    file_name_buffer_ = model_.file_name();
                    directory_buffer_ = model_.directory();
                }
            } else {
                // Files that do not match the filter are shown but greyed, so the
                // user can still see what is there.
                const ImVec4 colour = row.matches_filter ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                         : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    model_.set_file_name(row.name);
                    file_name_buffer_ = row.name;
                }
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();

    // The file-name box, prefilled with the current name.
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filename", "File name", &file_name_buffer_)) {
        model_.set_file_name(file_name_buffer_);
    }

    if (!error_.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s", error_.c_str());
    }

    const bool can_confirm = model_.can_confirm();
    ImGui::BeginDisabled(!can_confirm);
    if (ImGui::Button(confirm_label())) {
        if (Error error = confirm(context); error) {
            error_ = error.message();
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        model_.close();
        error_.clear();
        ImGui::CloseCurrentPopup();
    }

    // Closing the modal (Esc, or the close button on a popup with one) must also
    // close the model, or the panel would reopen next frame.
    if (!model_.is_open()) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace fe::editor
