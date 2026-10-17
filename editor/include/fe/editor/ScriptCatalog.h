// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptRegistry.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Node;
class Scene;

namespace editor {

// The editor's record of which gameplay behaviour is attached to which authored
// node, plus the catalogue of behaviours the editor can offer.
//
// Scripts are not components (ADR-0044), so they are not stored in the scene:
// they live in a `ScriptHost` keyed by the node's stable `Uuid`, and the
// behaviour that constructs one lives in a `ScriptRegistry` by name. The editor
// needs both together to attach a behaviour in the inspector, to keep the
// binding across a save/load, and to enumerate the choices it can offer, so this
// type owns them as a pair.
//
// It is deliberately headless - no ImGui, no window, no engine - so the whole
// of script editing's behaviour (attach, detach, persist, load) is testable
// without a display, and the panel is a thin layer over it.
//
// Persistence is not a method here: `EditorContext` passes a
// `script::ScriptBinding` built over this catalog's host and registry to the
// scene serializer, so a scene saved from the editor records the behaviours and
// a scene loaded into it comes back scripted (ADR-0063).
class ScriptCatalog {
public:
    ScriptCatalog() = default;

    ScriptCatalog(const ScriptCatalog&) = delete;
    ScriptCatalog& operator=(const ScriptCatalog&) = delete;

    // The behaviours the editor can offer. A host registers gameplay behaviours
    // here (for example a module's, ADR-0061) before the inspector is shown.
    [[nodiscard]] ScriptRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] const ScriptRegistry& registry() const noexcept { return registry_; }

    // The live instances, keyed by node id.
    [[nodiscard]] ScriptHost& host() noexcept { return host_; }
    [[nodiscard]] const ScriptHost& host() const noexcept { return host_; }

    // Every registered behaviour name, in registration order. This is what the
    // inspector's attach control lists.
    [[nodiscard]] std::vector<std::string> behaviour_names() const { return registry_.names(); }

    // Attaches a fresh instance of `behaviour` to `node`, replacing any script
    // the node already had. Fails if the name is not registered, or if the new
    // instance refuses the node; on failure the node is left as it was.
    [[nodiscard]] Error attach(Node& node, std::string_view behaviour) {
        std::unique_ptr<Script> script = registry_.create(behaviour);
        if (script == nullptr) {
            return Error("ScriptCatalog::attach: unknown behaviour '" + std::string(behaviour) + "'");
        }
        return host_.attach_script(node, std::move(script));
    }

    // Removes the node's script, if any. Returns true if one was removed. Safe
    // to call for a node with no script.
    bool detach(const Uuid& node_id) { return host_.detach_script(node_id); }

    [[nodiscard]] bool has_script(const Uuid& node_id) const noexcept { return host_.has_script(node_id); }

    [[nodiscard]] Script* script(const Uuid& node_id) noexcept { return host_.script(node_id); }
    [[nodiscard]] const Script* script(const Uuid& node_id) const noexcept { return host_.script(node_id); }

    // The behaviour name bound to the node, or "" when it has none. This is what
    // the inspector shows as the node's current behaviour.
    [[nodiscard]] std::string behaviour_of(const Uuid& node_id) const {
        const Script* bound = host_.script(node_id);
        return bound == nullptr ? std::string{} : std::string(bound->name());
    }

    // The description of the node's behaviour, or nullptr. The inspector reads
    // this to draw the behaviour's reflected properties.
    [[nodiscard]] const ScriptDesc* desc_of(const Uuid& node_id) const {
        const Script* bound = host_.script(node_id);
        return bound == nullptr ? nullptr : registry_.find(bound->name());
    }

    [[nodiscard]] usize script_count() const noexcept { return host_.script_count(); }

    // Drops every binding. Called before a different scene is loaded into the
    // document, so no script outlives the nodes it was bound to.
    void clear() noexcept { host_.clear(); }

private:
    ScriptRegistry registry_;
    ScriptHost host_;
};

} // namespace editor
} // namespace fe
