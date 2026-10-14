// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/systems/FrameContext.h"

#include <string_view>

namespace fe {

class Node;

// A unit of per-node gameplay behaviour.
//
// A Script is a sibling of System, not a Component. The split matters:
//
//   * A System (ADR-0008) is engine-shipped, data-oriented behaviour that owns
//     its own traversal of the scene. It is how animation, physics, and audio
//     run, and it is deliberately not a per-node callback.
//   * A Script is game-specific behaviour attached to one node, driven once per
//     frame by a ScriptHost. It exists so a game can add "this door opens when
//     the player has the key" without editing the engine.
//
// A Script is not reflection-serialized and not a component: components are
// engine data (ADR-0002, ADR-0005), scripts are game code. The seam is
// language-agnostic (ADR-0044): nothing here names C++, and a second language
// would implement this interface rather than change it.
//
// Lifetime: a Script is owned by a ScriptHost and must not outlive the node it
// is attached to. Detach before destroying the node.
class Script {
public:
    virtual ~Script() = default;

    Script(const Script&) = delete;
    Script& operator=(const Script&) = delete;
    Script(Script&&) = delete;
    Script& operator=(Script&&) = delete;

    // The behaviour name, used in logs and by the editor.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Called once when the script is bound to a live node. Returning an error
    // aborts the attach; the script is destroyed and the node keeps no script.
    virtual Error attach(Node& node) {
        (void)node;
        return Error::none();
    }

    // Called once before the script is unbound, while the node is still valid.
    virtual void detach(Node& node) { (void)node; }

    // Called once per frame by the ScriptHost while the script is enabled.
    //
    // The host contains failures: an error returned here, or an exception
    // thrown from here, disables this script and is reported, but does not stop
    // the other scripts or the frame (ADR-0003, ADR-0044).
    [[nodiscard]] virtual Error update(const FrameContext& context) = 0;

    // The node this script is attached to, or nullptr while detached.
    [[nodiscard]] Node* node() const noexcept { return node_; }

    // A disabled script is skipped by the host but still owned. The host uses
    // this to quarantine a script that failed.
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

protected:
    // Scripts are constructed by a factory (a ScriptRegistry entry, or the game)
    // and never copied or moved. Declaring this keeps a default constructor
    // available to a subclass even though the copy/move constructors above are
    // deleted. Same shape as Component.
    Script() = default;

private:
    friend class ScriptHost;
    void set_node(Node* node) noexcept { node_ = node; }

    Node* node_ = nullptr;
    bool enabled_ = true;
};

} // namespace fe
