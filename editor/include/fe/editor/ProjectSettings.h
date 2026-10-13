// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string>
#include <string_view>

namespace fe::editor {

// The project's settings, as persisted in a project file.
//
// A project is a directory with a settings file in it. The file is the unit the
// editor opens: it names the project, records the directories the engine and the
// editor care about, and is where future settings (default scene, renderer
// choice, per-asset import defaults) will live. Today it carries the settings
// that change behaviour: the asset root and the startup scene.
//
// The settings are stored as UTF-8 JSON, the same choice and for the same
// reason as scenes (ADR-0005): diffable, human-readable, and trivially
// inspectable while the format is still moving. The document is versioned and
// the reader rejects a version newer than it understands rather than silently
// misinterpreting it.
//
// This type is headless and lives in `fork_editor_core`: it parses and writes
// the document and owns no window, so it is fully testable without a display.
class ProjectSettings {
public:
    // The on-disk file name the editor looks for when it is pointed at a
    // project directory rather than a file.
    static constexpr const char* kDefaultFileName = "fork.project.json";

    // The document's format marker and version. The marker catches a file that
    // is JSON but is not a project; the version catches one from the future.
    static constexpr const char* kFormatName = "fork_project";
    static constexpr i32 kFormatVersion = 1;

    // --- Identity ----------------------------------------------------------
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // --- Directories -------------------------------------------------------
    // The project's asset root, as it appears in the file: relative to the
    // project directory when it points inside it, otherwise absolute. The
    // default is "assets".
    [[nodiscard]] const std::string& asset_root() const noexcept { return asset_root_; }
    void set_asset_root(std::string root) { asset_root_ = std::move(root); }

    // The scene the editor opens at startup, relative to the project directory.
    // Empty means "open a new example scene", the pre-project behaviour.
    [[nodiscard]] const std::string& startup_scene() const noexcept { return startup_scene_; }
    void set_startup_scene(std::string scene) { startup_scene_ = std::move(scene); }

    // --- Resolved paths ----------------------------------------------------
    // The directory the project file lives in. Empty until the settings have
    // been associated with a file (by load_project or save_project). All
    // resolved paths are relative to this.
    [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
    void set_directory(std::string directory) { directory_ = std::move(directory); }

    // The asset root as an absolute-or-project-relative path: `asset_root_`
    // joined onto `directory_`. Empty when the asset root is empty.
    [[nodiscard]] std::string resolved_asset_root() const;

    // The startup scene resolved against `directory_`. Empty when unset.
    [[nodiscard]] std::string resolved_startup_scene() const;

    // --- Persistence -------------------------------------------------------
    // Serialises to a JSON string. Paths inside the project directory are
    // written relative to it (via `directory_`), so the document travels with
    // the project instead of naming the machine it was authored on.
    [[nodiscard]] Result<std::string> to_json() const;

    // Parses a JSON string. On success `out` is replaced and its directory is
    // set to `directory` (the directory the text was read from), so resolved
    // paths are correct immediately. The document must carry the project format
    // marker and a version the reader understands.
    [[nodiscard]] static Error from_json(std::string_view text, std::string directory, ProjectSettings& out);

    // Reads and writes the file, using its parent directory as the project
    // directory.
    [[nodiscard]] static Result<ProjectSettings> load(std::string_view path);
    [[nodiscard]] Error save(std::string_view path) const;

private:
    std::string name_;
    std::string asset_root_ = "assets";
    std::string startup_scene_;
    std::string directory_;
};

} // namespace fe::editor
