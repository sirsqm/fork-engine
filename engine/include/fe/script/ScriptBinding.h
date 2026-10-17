// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/serialization/ScriptBinding.h"

#include <string>
#include <vector>

namespace fe::script {

// Persists a scene's gameplay scripts through the scene serializer's
// `ScriptBinding` seam (ADR-0063).
//
// Scripts are not components, so they are not written by the component path.
// This class is the bridge: given the `ScriptHost` that owns the live instances
// and the `ScriptRegistry` that describes them, it writes each bound behaviour
// and its reflected properties into the node's "scripts" array, and reads them
// back by constructing a fresh instance from the registry and attaching it to
// the node the binding names.
//
// A behaviour is identified by name, not by pointer, so a document written by
// one build loads in another that provides the same behaviour — which is the
// whole point of registering scripts by name. A behaviour the reading build does
// not provide is reported and skipped, exactly as an unknown component type is,
// so a scene authored against a richer build is not silently corrupted.
//
// The registry and host are borrowed and must outlive this object. The scene
// serializer calls `write` per node while writing, and `reset`/`read`/`finish`
// while reading.
class ScriptBinding final : public serialization::ScriptBinding {
public:
    ScriptBinding(ScriptHost& host, const ScriptRegistry& registry) : host_(&host), registry_(&registry) {}

    // Writes the behaviour and properties of each script bound to `node`. Does
    // nothing when the node has no script, so an unscripted scene keeps the
    // document shape it had before scripts were persisted.
    void write(const Node& node, nlohmann::json& node_json) const override;

    // Begins a new document. Clears any bindings recorded from a previous read,
    // and clears the host: its scripts are bound to nodes of the scene that is
    // about to be replaced, so keeping them would leave the host pointing at
    // dead nodes. A caller loading into a fresh host is unaffected.
    void reset() override;

    void read(const Node& node, const nlohmann::json& node_json) override;

    [[nodiscard]] Error finish(Scene& scene) override;

private:
    // One script to construct and attach once the scene is complete. The node id
    // is resolved through the scene at finish time, because the node may not
    // have been read when the binding was recorded.
    struct Pending {
        Uuid node_id;
        std::string behaviour;
        nlohmann::json properties;
    };

    ScriptHost* host_;
    const ScriptRegistry* registry_;
    std::vector<Pending> pending_;
};

} // namespace fe::script
