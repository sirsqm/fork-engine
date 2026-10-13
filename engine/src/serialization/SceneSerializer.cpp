// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/serialization/SceneSerializer.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"
#include "fe/resources/ResourceManager.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/scene/Sprite2D.h"
#include "fe/serialization/Json.h"

#include <nlohmann/json.hpp>

#include <string>

namespace fe::serialization::scene {
namespace {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------
// Each component type owns a small pair of functions. Adding a component means
// adding a branch here; that is intentional for this milestone. A registry that
// maps type names to read/write callbacks is the natural next step once the
// component set grows, and it will replace this switch without changing the
// file format.

[[nodiscard]] json component_to_json(const Component& component) {
    const std::string type = component.type_name();

    if (type == Sprite2D::kTypeName) {
        const auto& sprite = static_cast<const Sprite2D&>(component);
        json j;
        j["type"] = type;
        j["size"] = sprite.size;
        j["color"] = sprite.color;
        j["uv"] = sprite.uv;
        j["layer"] = sprite.layer;
        // Store the manager cache key (including the "tex:" scheme) rather than
        // the resource's own file path, so that reloading the scene resolves the
        // same GPU texture instead of the CPU image that shares the file.
        j["texture"] = sprite.texture != nullptr ? Texture2DLoader::reference_key(sprite.texture->path()) : std::string{};
        return j;
    }

    if (type == Camera2D::kTypeName) {
        const auto& camera = static_cast<const Camera2D&>(component);
        json j;
        j["type"] = type;
        j["zoom"] = camera.zoom;
        j["clear_color"] = json::array({camera.clear_r, camera.clear_g, camera.clear_b, camera.clear_a});
        j["primary"] = camera.primary;
        return j;
    }

    // Unknown component type: record its name so the reader can warn instead of
    // silently dropping data.
    return json{{"type", type}};
}

[[nodiscard]] Error component_from_json(const json& j, Node& node, ResourceManager* resources) {
    const std::string type = j.value("type", std::string{});

    if (type == Sprite2D::kTypeName) {
        auto* sprite = node.add_component<Sprite2D>();
        sprite->size = j.value("size", Vec2{});
        sprite->color = j.value("color", Vec4{1.0f, 1.0f, 1.0f, 1.0f});
        sprite->uv = j.value("uv", Vec4{0.0f, 0.0f, 1.0f, 1.0f});
        sprite->layer = j.value("layer", 0);
        const std::string texture_path = j.value("texture", std::string{});
        if (!texture_path.empty() && resources != nullptr) {
            auto texture = resources->load(texture_path);
            if (!texture) {
                // A missing texture is not fatal: the scene still loads, and the
                // sprite simply draws nothing. Report it so the user can fix it.
                FE_LOG_WARN("scene: could not load texture '%s': %s", texture_path.c_str(),
                            texture.error().message().c_str());
            } else {
                sprite->texture = texture.value();
            }
        }
        return Error::none();
    }

    if (type == Camera2D::kTypeName) {
        auto* camera = node.add_component<Camera2D>();
        camera->zoom = j.value("zoom", 1.0f);
        camera->primary = j.value("primary", true);
        const auto clear = j.value("clear_color", std::vector<f32>{0.10f, 0.10f, 0.12f, 1.0f});
        if (clear.size() == 4) {
            camera->clear_r = clear[0];
            camera->clear_g = clear[1];
            camera->clear_b = clear[2];
            camera->clear_a = clear[3];
        }
        return Error::none();
    }

    FE_LOG_WARN("scene: ignoring unknown component type '%s'", type.c_str());
    return Error::none();
}

// ---------------------------------------------------------------------------
// Nodes
// ---------------------------------------------------------------------------

[[nodiscard]] json node_to_json(const Node& node) {
    json j;
    j["id"] = node.id();
    j["name"] = node.name();
    j["type"] = node.type_name();
    j["transform"] = node.transform();

    json components = json::array();
    for (const auto& component : node.components()) {
        components.push_back(component_to_json(*component));
    }
    j["components"] = std::move(components);

    json children = json::array();
    for (const auto& child : node.children()) {
        children.push_back(node_to_json(*child));
    }
    j["children"] = std::move(children);
    return j;
}

[[nodiscard]] Error node_from_json(const json& j, Node& node, ResourceManager* resources) {
    // The node's identity and name are restored in place; the caller created
    // the node so the type is already correct.
    const std::string name = j.value("name", std::string("Node"));
    node.set_name(name);

    // Restore identity so that references to this node survive a save/load
    // cycle. Absent ids (hand-written or older documents) keep the id assigned
    // when the node was created.
    if (j.contains("id")) {
        node.set_id(j.at("id").get<Uuid>());
    }

    if (j.contains("transform")) {
        node.transform() = j.at("transform").get<Transform>();
    }

    if (j.contains("components")) {
        for (const auto& component_json : j.at("components")) {
            if (Error error = component_from_json(component_json, node, resources); error) {
                return error;
            }
        }
    }

    if (j.contains("children")) {
        for (const auto& child_json : j.at("children")) {
            // Plain nodes are the only node type this milestone persists. A
            // future node subtype would be dispatched on child_json["type"].
            Node* child = node.create_child<Node>();
            if (Error error = node_from_json(child_json, *child, resources); error) {
                return error;
            }
        }
    }
    return Error::none();
}

} // namespace

Result<std::string> to_json(const Scene& scene) {
    json document;
    document["format"] = "fork_scene";
    document["format_version"] = kFormatVersion;
    document["name"] = scene.name();

    json roots = json::array();
    for (const auto& root : scene.roots()) {
        roots.push_back(node_to_json(*root));
    }
    document["roots"] = std::move(roots);

    return document.dump(2);
}

Error from_json(std::string_view text, Scene& out_scene, ResourceManager* resources) {
    json document;
    try {
        document = json::parse(text);
    } catch (const json::parse_error& e) {
        return Error(std::string("scene parse error: ") + e.what());
    }

    const std::string format = document.value("format", std::string{});
    if (format != "fork_scene") {
        return Error("not a fork scene document (format='" + format + "')");
    }

    const i32 version = document.value("format_version", 0);
    if (version > kFormatVersion) {
        return Error("scene format version " + std::to_string(version) + " is newer than supported version " +
                     std::to_string(kFormatVersion));
    }

    out_scene.clear();
    out_scene.set_name(document.value("name", std::string("Scene")));

    if (!document.contains("roots")) {
        return Error::none();
    }

    for (const auto& root_json : document.at("roots")) {
        Node* root = out_scene.create_node();
        if (Error error = node_from_json(root_json, *root, resources); error) {
            return error;
        }
    }
    return Error::none();
}

Error save(const Scene& scene, std::string_view path) {
    auto text = to_json(scene);
    if (!text) {
        return text.error();
    }
    return fs::write_text_file(path, text.value());
}

Error load(std::string_view path, Scene& out_scene, ResourceManager* resources) {
    auto text = fs::read_text_file(path);
    if (!text) {
        return text.error();
    }
    return from_json(text.value(), out_scene, resources);
}

} // namespace fe::serialization::scene
