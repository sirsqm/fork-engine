// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/ExportManifest.h"

#include "fe/core/FileSystem.h"

#include <nlohmann/json.hpp>

#include <string>

namespace fe::pack {
namespace {

using json = nlohmann::json;

} // namespace

Result<std::string> ExportManifest::to_json() const {
    json document = json::object();
    document["format"] = kFormatName;
    document["format_version"] = kFormatVersion;
    if (!name.empty()) {
        document["name"] = name;
    }
    // Written only when it is not the default, so a desktop export's manifest is
    // byte-for-byte what it was before targets existed.
    if (target != kTargetDesktop) {
        document["target"] = target;
    }
    document["pack"] = pack;
    document["scene"] = scene;
    // Written only when set, so a build without declarative bindings keeps the
    // manifest it had before this field existed.
    if (!bindings.empty()) {
        document["bindings"] = bindings;
    }
    // Omitted when empty, so a data-only build's manifest stays as small and as
    // readable as it was before modules existed.
    if (!modules.empty()) {
        document["modules"] = modules;
    }
    // Written only when set, so a manifest written by a caller that does not know
    // the engine version (a unit test, an older tool) keeps the shape it had
    // before this field existed.
    if (!engine_version.empty()) {
        document["engine_version"] = engine_version;
    }
    // Indented: a manifest is small, and a human reading an export directory
    // should be able to see what it runs without a JSON tool.
    return document.dump(2);
}

Error ExportManifest::from_json(std::string_view text, ExportManifest& out) {
    json document;
    try {
        document = json::parse(text);
    } catch (const json::parse_error& e) {
        return Error(std::string("export manifest: parse error: ") + e.what());
    }
    if (!document.is_object()) {
        return Error("export manifest: the document is not an object");
    }

    const std::string format = document.value("format", std::string{});
    if (format != kFormatName) {
        return Error("export manifest: not a fork export document (format='" + format + "')");
    }
    const i32 version = document.value("format_version", 0);
    if (version != kFormatVersion) {
        return Error("export manifest: format version " + std::to_string(version) + " is not supported");
    }

    ExportManifest parsed;
    parsed.name = document.value("name", std::string{});
    // A document from before targets existed has no target and can only have
    // been a desktop build, so the default is the honest reading rather than a
    // guess.
    parsed.target = document.value("target", std::string(kTargetDesktop));
    if (parsed.target != kTargetDesktop && parsed.target != kTargetWeb && parsed.target != kTargetAndroid) {
        return Error("export manifest: unknown target '" + parsed.target + "'");
    }
    parsed.pack = document.value("pack", std::string{});
    parsed.scene = document.value("scene", std::string{});
    parsed.bindings = document.value("bindings", std::string{});
    // Absent in a document written before the field existed, which reads as an
    // unrecorded version rather than a guess. A wrong type is a document the
    // writer would not have produced.
    if (document.contains("engine_version") && !document.at("engine_version").is_string()) {
        return Error("export manifest: 'engine_version' is not a string");
    }
    parsed.engine_version = document.value("engine_version", std::string{});
    if (parsed.pack.empty()) {
        return Error("export manifest: no pack file named");
    }
    if (parsed.scene.empty()) {
        return Error("export manifest: no startup scene named");
    }

    // A module list that is present must be an array of strings; a wrong shape is
    // a document the writer would not have produced, so reject it rather than
    // silently loading no modules.
    if (document.contains("modules")) {
        const json& modules = document.at("modules");
        if (!modules.is_array()) {
            return Error("export manifest: 'modules' is not an array");
        }
        for (const json& module : modules) {
            if (!module.is_string()) {
                return Error("export manifest: a module entry is not a string");
            }
            parsed.modules.push_back(module.get<std::string>());
        }
    }

    out = std::move(parsed);
    return Error::none();
}

Result<ExportManifest> ExportManifest::load(std::string_view path) {
    auto text = fs::read_text_file(path);
    if (!text) {
        return text.error();
    }
    ExportManifest manifest;
    if (Error error = from_json(text.value(), manifest); error) {
        return error;
    }
    return manifest;
}

Error ExportManifest::save(std::string_view path) const {
    auto text = to_json();
    if (!text) {
        return text.error();
    }
    // write_text_file creates the parent directories, so an export step can
    // write straight into a directory that does not exist yet.
    return fs::write_text_file(path, text.value());
}

} // namespace fe::pack
