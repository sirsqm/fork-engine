// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Log.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/platform/DynamicLibrary.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptModuleApi.h"
#include "fe/script/ScriptRegistry.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Node;

// Loads C++ gameplay modules and owns the behaviours they provide.
//
// This is the loading half of M7 (ADR-0044, ADR-0061): a module is a shared
// library that registers behaviours into a ScriptRegistry through the versioned
// entry point in ScriptModuleApi.h. The library opens the image with the
// platform layer (DynamicLibrary, the "dynamic libraries" row of the platform
// contract), checks the ABI version, calls the entry point, and records which
// behaviours the module provided.
//
// Hot reload is the point of the class. reload_module() replaces a module's code
// while the engine keeps running: it destroys every live script the module
// provided (detaching first, so no instance outlives the image), unloads the
// image, loads the replacement, and re-attaches a fresh script to each node that
// had one. A module is identified by the path it was first loaded from (its id);
// the replacement image may live at a different path, which is what makes a swap
// possible on a platform that will not let a loaded image be overwritten
// (Windows) and what a test needs to load genuinely different code.
//
// The library owns the ScriptRegistry and the loaded images; the ScriptHost owns
// the live instances. A game holds both, drives the host from a ScriptSystem, and
// attaches, detaches, and reloads through the library.
//
// Threading: loading, reloading, and attaching happen on the frame thread
// between frames, never from inside a script's update(). A script must not
// attach or detach scripts (the ScriptHost contract), so the library is never
// re-entered from a running script.
class ScriptLibrary {
public:
    ScriptLibrary() = default;

    // Destroys the registry's factories before the module images that hold their
    // code. A registered behaviour is a std::function whose target lives inside a
    // module, so destroying it after the image is unloaded would call into
    // unmapped memory (ADR-0061).
    ~ScriptLibrary();

    ScriptLibrary(const ScriptLibrary&) = delete;
    ScriptLibrary& operator=(const ScriptLibrary&) = delete;

    // Opens the module at `path` and registers the behaviours it provides. A
    // path with no shared-library suffix is retried with the platform suffix
    // appended. Loading an already-loaded module is a no-op. The resolved path is
    // the module's id, used by reload_module().
    [[nodiscard]] Error load_module(std::string_view path);

    // Replaces the code of the module loaded from `module_id` with the image at
    // `new_image_path` (default: the same path, the ordinary rebuilt-in-place
    // case). Live scripts the module provided are detached, the old image is
    // unloaded, the new image is loaded, and every node that had one gets a fresh
    // script built from the new code.
    //
    // Returns an Error if the module is not loaded or the new image cannot be
    // loaded. On a load failure the module is dropped — its behaviours are
    // unregistered — and the nodes it scripted are left unscripted rather than
    // holding instances of dead code.
    [[nodiscard]] Error reload_module(ScriptHost& host, std::string_view module_id,
                                      std::string_view new_image_path = {});

    [[nodiscard]] bool is_module_loaded(std::string_view module_id) const noexcept;

    [[nodiscard]] usize module_count() const noexcept { return modules_.size(); }

    // The ids of the loaded modules, in id order.
    [[nodiscard]] std::vector<std::string> module_ids() const;

    // Creates the named behaviour and attaches it to `node`, recording the
    // binding so a later reload rebinds it. Replaces any script already on the
    // node. Returns an Error if no loaded module provides the behaviour.
    [[nodiscard]] Error attach(ScriptHost& host, Node& node, std::string_view behaviour);

    // Detaches the node's script and forgets its binding. Returns true if the
    // node had a script.
    bool detach(ScriptHost& host, const Uuid& node_id);

    [[nodiscard]] ScriptRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] const ScriptRegistry& registry() const noexcept { return registry_; }

    // The number of node-to-behaviour bindings the library is tracking, which is
    // the number of scripts it will rebind on a reload.
    [[nodiscard]] usize attached_count() const noexcept { return bindings_.size(); }

private:
    // A loaded image and the behaviours it registered.
    struct Module {
        DynamicLibrary image;
        std::vector<std::string> behaviours;
    };

    // A node scripted by a module, remembered so a reload can rebind it. The node
    // pointer is cached so a rebind does not have to find it again; the scene
    // owns the node, so it stays valid for as long as the binding exists.
    struct Binding {
        Uuid node_id;
        std::string behaviour;
        Node* node = nullptr;
    };

    // Opens the image, checks the ABI, and calls the entry point, which registers
    // its behaviours directly into registry_. The caller diffs registry_.names()
    // around the call to learn which behaviours were added.
    [[nodiscard]] Error open_module(std::string_view image_path, Module& out);

    // Re-attaches a script to every node in `to_rebind` whose behaviour is now
    // provided, building the instance from the freshly loaded code.
    void rebind_after_reload(ScriptHost& host, const std::vector<Binding>& to_rebind);

    ScriptRegistry registry_;
    std::map<std::string, Module> modules_;
    std::vector<Binding> bindings_;
};

} // namespace fe
