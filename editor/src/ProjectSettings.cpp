// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/ProjectSettings.h"

#include "fe/core/FileSystem.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace fe::editor {
namespace {

using json = nlohmann::json;

} // namespace

std::string ProjectSettings::resolved_asset_root() const {
    if (asset_root_.empty()) {
        return {};
    }
    // An absolute root points outside the project (a shared asset tree), so it
    // is used as-is; a relative one is resolved against the project directory.
    if (directory_.empty() || fs::is_absolute(asset_root_)) {
        return fs::normalize(asset_root_);
    }
    return fs::normalize(fs::join(directory_, asset_root_));
}

std::string ProjectSettings::resolved_startup_scene() const {
    if (startup_scene_.empty()) {
        return {};
    }
    if (directory_.empty() || fs::is_absolute(startup_scene_)) {
        return fs::normalize(startup_scene_);
    }
    return fs::normalize(fs::join(directory_, startup_scene_));
}

Result<std::string> ProjectSettings::to_json() const {
    json document;
    document["format"] = kFormatName;
    document["format_version"] = kFormatVersion;
    document["name"] = name_;
    // Store paths relative to the project directory when they point inside it,
    // so the file is portable; a path outside it stays absolute.
    document["asset_root"] = fs::relative_to(directory_, resolved_asset_root());
    if (!startup_scene_.empty()) {
        document["startup_scene"] = fs::relative_to(directory_, resolved_startup_scene());
    }
    return document.dump(2);
}

Error ProjectSettings::from_json(std::string_view text, std::string directory, ProjectSettings& out) {
    json document;
    try {
        document = json::parse(text);
    } catch (const json::parse_error& e) {
        return Error("project settings parse error: " + std::string(e.what()));
    }

    const std::string format = document.value("format", std::string{});
    if (format != kFormatName) {
        return Error("not a fork project document (format='" + format + "')");
    }

    const i32 version = document.value("format_version", 0);
    if (version > kFormatVersion) {
        return Error("project format version " + std::to_string(version) + " is newer than supported version " +
                     std::to_string(kFormatVersion));
    }
    if (version < 1) {
        return Error("project format version " + std::to_string(version) + " is invalid");
    }

    ProjectSettings parsed;
    parsed.name_ = document.value("name", std::string{});
    // A file that omits the asset root keeps the default ("assets"); a file that
    // sets it to "" genuinely means "no assets".
    parsed.asset_root_ = document.value("asset_root", std::string{"assets"});
    parsed.startup_scene_ = document.value("startup_scene", std::string{});
    parsed.directory_ = std::move(directory);

    out = std::move(parsed);
    return Error::none();
}

Result<ProjectSettings> ProjectSettings::load(std::string_view path) {
    auto text = fs::read_text_file(path);
    if (!text) {
        return text.error();
    }
    const std::string directory = fs::parent_path(fs::normalize(path));
    ProjectSettings settings;
    if (Error error = from_json(text.value(), directory, settings); error) {
        return error;
    }
    return settings;
}

Error ProjectSettings::save(std::string_view path) const {
    // Saving to a path sets the project directory to that path's directory, so a
    // freshly authored project writes portable relative paths without a
    // separate set_directory call.
    ProjectSettings written = *this;
    written.directory_ = fs::parent_path(fs::normalize(path));

    auto text = written.to_json();
    if (!text) {
        return text.error();
    }
    return fs::write_text_file(path, text.value());
}

} // namespace fe::editor
