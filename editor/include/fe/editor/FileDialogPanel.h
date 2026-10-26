// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/editor/FileDialogViewModel.h"
#include "fe/editor/Panel.h"

#include <string>
#include <vector>

namespace fe::editor {

// The editor's file dialog: a modal browser over the filesystem, used to open a
// scene, save a scene, or open a project.
//
// This is the toolkit projection of `FileDialogViewModel`, which owns all the
// browsing logic and is tested without a window. The panel contributes the
// ImGui modal, the widgets, and the small amount of editor knowledge the view
// model deliberately does not have: which file kinds the editor deals in, and
// what to do when the user confirms.
//
// Confirming performs the action against the `EditorContext` it is drawn with,
// through the context's public API (`load_scene`, `save_scene`, `load_project`),
// exactly as the hierarchy panel executes commands through the context. The
// panel is therefore a client of the document, not of the engine.
//
// It is a `Panel` (so the dock host owns it and its lifetime) but it draws a
// modal popup rather than a docked window: a file dialog is transient and should
// block the editor while it is up. When no dialog is open, `draw()` does nothing
// and the panel occupies no space.
class FileDialogPanel final : public Panel {
public:
    // What the dialog is choosing. The purpose picks the extension filter and
    // the action performed on confirm.
    enum class Purpose {
        OpenScene,
        SaveScene,
        OpenProject,
    };

    FileDialogPanel() = default;

    [[nodiscard]] const char* name() const noexcept override { return "FileDialog"; }

    // --- Opening (called from the menu) -----------------------------------
    // Opens the dialog to open a scene, starting in `start_directory`.
    void open_scene(std::string start_directory);
    // Opens the dialog to save a scene. `start_directory` is where to browse;
    // `start_name` is the suggested file name (typically the current scene's).
    void open_save_scene(std::string start_directory, std::string start_name);
    // Opens the dialog to open a project file, starting in `start_directory`.
    void open_project(std::string start_directory);

    [[nodiscard]] bool is_open() const noexcept { return model_.is_open(); }

    // The view model, exposed for tests and for the menu to read the current
    // directory. Not for drawing.
    [[nodiscard]] FileDialogViewModel& model() noexcept { return model_; }
    [[nodiscard]] const FileDialogViewModel& model() const noexcept { return model_; }

    // Performs the confirm action for the current selection against `context`
    // without any UI. This is what the "Open"/"Save" button calls, and what a
    // test drives directly, so the confirm path is exercised headlessly. Returns
    // an Error when there is nothing to confirm or the action fails.
    [[nodiscard]] Error confirm(EditorContext& context);

    void draw(EditorContext& context) override;

private:
    [[nodiscard]] const char* popup_id() const noexcept;
    [[nodiscard]] const char* title() const noexcept;
    [[nodiscard]] const char* confirm_label() const noexcept;
    // The extension filter for the current purpose.
    [[nodiscard]] static const char* filter_for(Purpose purpose) noexcept;

    FileDialogViewModel model_;
    Purpose purpose_ = Purpose::OpenScene;
    // The confirm error from the last attempt, shown in the modal so a failed
    // open or save is visible rather than silent. Cleared when the dialog opens.
    std::string error_;
    // The text buffers the widgets edit, synced to the model each frame.
    std::string directory_buffer_;
    std::string file_name_buffer_;
    std::string filter_buffer_;
};

} // namespace fe::editor
