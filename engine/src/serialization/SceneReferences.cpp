// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/serialization/SceneReferences.h"

#include "fe/core/FileSystem.h"
#include "fe/reflect/ComponentTypes.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/AssetDatabase.h"
#include "fe/resources/AssetId.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fe::serialization::scene {
namespace {

using json = nlohmann::json;

// The property names a reference is spelled under. Kept next to the walkers
// below so the document shape and the reader cannot drift apart silently.
constexpr const char* kComponentsKey = "components";
constexpr const char* kTypeKey = "type";
constexpr const char* kChildrenKey = "children";
constexpr const char* kScriptsKey = "scripts";
constexpr const char* kPropertiesKey = "properties";

// Appends the reference a ResourceRef property names, if the document carries
// one. The property descriptor says *that* this field is a resource; the JSON
// holds the value, so the two are matched by name rather than read from a live
// component.
void collect_property_reference(const reflect::Property& property, const json& component_json,
                                std::vector<std::string>& out) {
    if (property.kind() == reflect::ValueKind::Group) {
        if (!component_json.contains(property.name)) {
            return;
        }
        const json& group_json = component_json.at(property.name);
        if (property.array_length > 0) {
            if (!group_json.is_array()) {
                return;
            }
            for (usize i = 0; i < property.members.size() && i < group_json.size(); ++i) {
                collect_property_reference(property.members[i], group_json[i], out);
            }
            return;
        }
        if (!group_json.is_object()) {
            return;
        }
        for (const reflect::Property& member : property.members) {
            collect_property_reference(member, group_json, out);
        }
        return;
    }

    if (property.kind() != reflect::ValueKind::ResourceRef) {
        return;
    }
    if (!component_json.contains(property.name) || !component_json.at(property.name).is_string()) {
        return;
    }
    const std::string reference = component_json.at(property.name).get<std::string>();
    if (!reference.empty()) {
        out.push_back(reference);
    }
}

// A behaviour's property kinds are known only once its module is loaded, which a
// packer does not do, so every string a script's "properties" object carries is a
// candidate. The caller resolves them against its database and keeps the ones
// that name a real asset, so a false candidate costs nothing and a real
// reference is never missed.
void collect_script_strings(const json& value, std::vector<std::string>& out) {
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (!text.empty()) {
            out.push_back(text);
        }
        return;
    }
    if (value.is_array()) {
        for (const json& element : value) {
            collect_script_strings(element, out);
        }
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            collect_script_strings(it.value(), out);
        }
    }
}

void collect_node_references(const json& node_json, SceneReferences& out) {
    if (node_json.contains(kComponentsKey) && node_json.at(kComponentsKey).is_array()) {
        for (const json& component_json : node_json.at(kComponentsKey)) {
            if (!component_json.is_object()) {
                continue;
            }
            const std::string type_name = component_json.value(kTypeKey, std::string{});
            const reflect::TypeDesc* type = reflect::Registry::instance().find(type_name);
            if (type == nullptr) {
                // An unregistered type is not a failure here: the packer traces
                // what it can describe and packs the rest of the tree under the
                // caller's fallback policy. Reporting it would make a pack fail
                // for a type this build does not know, which is the wrong
                // trade-off for an optimisation.
                continue;
            }
            for (const reflect::Property& property : type->properties) {
                collect_property_reference(property, component_json, out.required);
            }
        }
    }

    if (node_json.contains(kScriptsKey) && node_json.at(kScriptsKey).is_array()) {
        for (const json& script_json : node_json.at(kScriptsKey)) {
            if (script_json.is_object() && script_json.contains(kPropertiesKey)) {
                collect_script_strings(script_json.at(kPropertiesKey), out.candidates);
            }
        }
    }

    if (node_json.contains(kChildrenKey) && node_json.at(kChildrenKey).is_array()) {
        for (const json& child_json : node_json.at(kChildrenKey)) {
            collect_node_references(child_json, out);
        }
    }
}

} // namespace

Result<SceneReferences> collect_resource_references(std::string_view scene_json) {
    // The builtin 2D types are the floor, exactly as they are for the reader, so
    // a plain scene is traced with no setup. A caller that uses a 3D or audio
    // component must register that module's types first; `runtime::
    // register_all_component_types()` is the aggregate for a caller that wants
    // all of them (the pack and export tools call it before tracing).
    reflect::register_builtin_types();

    json document;
    try {
        document = json::parse(scene_json);
    } catch (const json::parse_error& e) {
        return Error(std::string("scene references: parse error: ") + e.what());
    }
    if (!document.is_object()) {
        return Error("scene references: the document is not an object");
    }

    SceneReferences references;
    if (document.contains("roots") && document.at("roots").is_array()) {
        for (const json& root_json : document.at("roots")) {
            collect_node_references(root_json, references);
        }
    }
    return references;
}

std::string reference_to_asset_path(std::string_view reference, const AssetDatabase& assets) {
    if (reference.empty()) {
        return {};
    }

    AssetId id;
    if (parse_reference(reference, id)) {
        const AssetDatabase::Asset* asset = assets.find(id);
        return asset != nullptr ? asset->path : std::string{};
    }

    // A "<scheme>:<path>" reference names an import; the pack keys by path, so
    // the scheme is stripped. A bare path is already the key.
    std::string_view path = reference;
    const std::size_t colon = path.find(':');
    if (colon != std::string_view::npos && colon > 1) {
        path = path.substr(colon + 1);
    }
    return std::string(path);
}

Result<std::vector<std::string>> resolve_scene_asset_paths(const SceneReferences& references,
                                                           std::string_view pack_root, const AssetDatabase& assets) {
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;
    const auto keep = [&](const std::string& path) {
        if (seen.insert(path).second) {
            paths.push_back(path);
        }
    };
    // A reference resolves if the database has indexed it or, failing that, if the
    // file is simply present under the pack root - an unimported asset is still
    // packed, by path, under the default scheme, exactly as the unpruned writer
    // packs it.
    const auto resolves = [&](const std::string& path) {
        return !path.empty() &&
               (assets.find_by_path(path) != nullptr || fs::exists(fs::join(std::string(pack_root), path)));
    };

    for (const std::string& reference : references.required) {
        const std::string path = reference_to_asset_path(reference, assets);
        if (!resolves(path)) {
            return Error("scene reference '" + reference + "' is not an asset under '" + std::string(pack_root) + "'");
        }
        keep(path);
    }
    for (const std::string& reference : references.candidates) {
        const std::string path = reference_to_asset_path(reference, assets);
        if (resolves(path)) {
            keep(path);
        }
    }
    return paths;
}

} // namespace fe::serialization::scene
