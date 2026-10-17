// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/script/ScriptBinding.h"

#include "fe/core/Log.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/PropertyJson.h"
#include "fe/serialization/SceneSerializer.h"

#include <string>
#include <utility>

namespace fe::script {
namespace {

using json = nlohmann::json;

constexpr const char* kScriptsKey = "scripts";
constexpr const char* kBehaviourKey = "behaviour";
constexpr const char* kPropertiesKey = "properties";

} // namespace

void ScriptBinding::write(const Node& node, json& node_json) const {
    // The host owns the scripts; the registry describes them. A node with no
    // script contributes nothing, so an unscripted scene's document is byte for
    // byte what it was before scripts were persisted.
    json scripts = json::array();
    for (const ScriptHost::BindingView& binding : host_->bindings()) {
        if (binding.node_id != node.id() || binding.script == nullptr) {
            continue;
        }
        const Script& script = *binding.script;
        json entry = json::object();
        entry[kBehaviourKey] = std::string(script.name());

        // A behaviour with no description has no properties to write. Its name
        // is still recorded, so it round-trips as a bare binding.
        if (const ScriptDesc* desc = registry_->find(script.name())) {
            if (!desc->properties.empty()) {
                entry[kPropertiesKey] = serialization::properties_to_json(desc->properties, &script, nullptr);
            }
        }
        scripts.push_back(std::move(entry));
    }

    if (!scripts.empty()) {
        node_json[kScriptsKey] = std::move(scripts);
    }
}

void ScriptBinding::reset() {
    pending_.clear();
    // The host's scripts belong to the scene being replaced. Dropping them here
    // avoids leaving the host pointing at nodes that are about to be destroyed.
    host_->clear();
}

void ScriptBinding::read(const Node& node, const json& node_json) {
    if (!node_json.contains(kScriptsKey) || !node_json.at(kScriptsKey).is_array()) {
        return;
    }
    for (const auto& entry : node_json.at(kScriptsKey)) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string behaviour = entry.value(kBehaviourKey, std::string{});
        if (behaviour.empty()) {
            continue;
        }
        Pending item;
        item.node_id = node.id();
        item.behaviour = behaviour;
        item.properties = entry.contains(kPropertiesKey) ? entry.at(kPropertiesKey) : json::object();
        pending_.push_back(std::move(item));
    }
}

Error ScriptBinding::finish(Scene& scene) {
    for (const Pending& item : pending_) {
        Node* node = scene.find_node(item.node_id);
        if (node == nullptr) {
            FE_LOG_WARN("scene: script '%s' names node %s, which is not in the scene", item.behaviour.c_str(),
                        item.node_id.to_string().c_str());
            continue;
        }

        std::unique_ptr<Script> script = registry_->create(item.behaviour);
        if (script == nullptr) {
            // The build reading the document does not provide this behaviour.
            // Report it by name rather than dropping it silently, the same
            // policy the reader applies to an unknown component type.
            FE_LOG_WARN("scene: ignoring unknown script behaviour '%s'", item.behaviour.c_str());
            continue;
        }

        // Apply the persisted properties before attaching, so attach() sees the
        // authored values rather than the defaults.
        const ScriptDesc* desc = registry_->find(item.behaviour);
        if (desc != nullptr && !desc->properties.empty()) {
            if (Error error = serialization::apply_properties(desc->properties, script.get(), item.properties, nullptr,
                                                              serialization::scene::kFormatVersion);
                error) {
                return error;
            }
        }

        if (Error error = host_->attach_script(*node, std::move(script)); error) {
            FE_LOG_WARN("scene: could not attach script '%s' to node %s: %s", item.behaviour.c_str(),
                        item.node_id.to_string().c_str(), error.message().c_str());
        }
    }
    pending_.clear();
    return Error::none();
}

} // namespace fe::script
