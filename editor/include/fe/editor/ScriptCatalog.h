// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/reflect/Value.h"
#include "fe/resources/ResourceManager.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptLibrary.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/serialization/ScriptTarget.h"

#include <optional>
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
//
// The catalog also implements `serialization::ScriptTarget`, the seam the
// operation model uses to read and write a live script's properties. That is
// what lets a script property edit be an ordinary serializable operation - and
// so undoable - without the operation model depending on the script module.
//
// It owns a `ScriptLibrary` (ADR-0061), so the behaviours a game provides can be
// loaded from a shared library rather than only registered in-process, and a
// rebuilt module can be hot-reloaded without restarting the editor (ADR-0066).
// The registry the library owns is the same one the inspector lists and the
// serializer resolves against, so a module's behaviour appears in the attach
// control the moment its module loads.
class ScriptCatalog : public serialization::ScriptTarget {
public:
    ScriptCatalog() = default;

    ScriptCatalog(const ScriptCatalog&) = delete;
    ScriptCatalog& operator=(const ScriptCatalog&) = delete;

    // The behaviours the editor can offer. A host registers gameplay behaviours
    // here (for example a module's, ADR-0061) before the inspector is shown.
    [[nodiscard]] ScriptRegistry& registry() noexcept { return library_.registry(); }
    [[nodiscard]] const ScriptRegistry& registry() const noexcept { return library_.registry(); }

    // The live instances, keyed by node id.
    [[nodiscard]] ScriptHost& host() noexcept { return host_; }
    [[nodiscard]] const ScriptHost& host() const noexcept { return host_; }

    // --- Gameplay modules (ADR-0061, ADR-0066) -----------------------------
    // Loads a gameplay module from a shared library, so its behaviours join the
    // registry the inspector offers. Returns an Error naming the file when the
    // image will not load or reports a mismatched ABI.
    [[nodiscard]] Error load_module(std::string_view path) { return library_.load_module(path); }

    // The loaded module ids, in id order.
    [[nodiscard]] std::vector<std::string> module_ids() const { return library_.module_ids(); }
    [[nodiscard]] usize module_count() const noexcept { return library_.module_count(); }
    [[nodiscard]] bool is_module_loaded(std::string_view id) const noexcept { return library_.is_module_loaded(id); }

    // The module ids whose image changed on disk since it was loaded, and each
    // one's recorded time updated so a change is reported once. This is the
    // editor's "a module was rebuilt, reload it" step; the caller adopts each id
    // with reload_module(). Mirrors ResourceManager::poll_changed (ADR-0046).
    //
    // `settle_polls` debounces a build (ADR-0067): a module is reported only once
    // its write time has held for that many consecutive polls, so an image a
    // build is still writing is not reloaded mid-write. The default of 1 reports
    // as soon as the time stops moving.
    [[nodiscard]] std::vector<std::string> poll_changed_modules(u32 settle_polls = 1) {
        return library_.poll_changed(settle_polls);
    }

    // Replaces a loaded module's code and rebinds every node it scripted to a
    // fresh instance built from the new code (ADR-0061). `new_image_path` is the
    // ordinary rebuilt-in-place case when empty.
    [[nodiscard]] Error reload_module(std::string_view module_id, std::string_view new_image_path = {}) {
        return library_.reload_module(host_, module_id, new_image_path);
    }

    // Rebuilds the reload bookkeeping from the scripts currently attached, so a
    // set of scripts attached outside the library (a scene load through the
    // serializer, ADR-0063) is still rebound by a later reload.
    void adopt_bindings() { library_.adopt_bindings(host_); }

    // Every registered behaviour name, in registration order. This is what the
    // inspector's attach control lists.
    [[nodiscard]] std::vector<std::string> behaviour_names() const { return registry().names(); }

    // Attaches a fresh instance of `behaviour` to `node`, replacing any script
    // the node already had. Fails if the name is not registered, or if the new
    // instance refuses the node; on failure the node is left as it was.
    [[nodiscard]] Error attach(Node& node, std::string_view behaviour) {
        return library_.attach(host_, node, behaviour);
    }

    // Removes the node's script, if any. Returns true if one was removed. Safe
    // to call for a node with no script.
    bool detach(const Uuid& node_id) { return library_.detach(host_, node_id); }

    [[nodiscard]] bool has_script(const Uuid& node_id) const noexcept { return host_.has_script(node_id); }

    [[nodiscard]] Script* script(const Uuid& node_id) noexcept { return host_.script(node_id); }
    [[nodiscard]] const Script* script(const Uuid& node_id) const noexcept { return host_.script(node_id); }

    // The behaviour name bound to the node, or "" when it has none, is the
    // `behaviour_of` override below (the inspector shows it as the node's
    // current behaviour).

    // The description of the node's behaviour, or nullptr. The inspector reads
    // this to draw the behaviour's reflected properties.
    [[nodiscard]] const ScriptDesc* desc_of(const Uuid& node_id) const {
        const Script* bound = host_.script(node_id);
        return bound == nullptr ? nullptr : registry().find(bound->name());
    }

    [[nodiscard]] usize script_count() const noexcept { return host_.script_count(); }

    // --- serialization::ScriptTarget ---------------------------------------
    // The operation model's view of a live script: which behaviour a node runs,
    // and a property read/write by reflected path. A read or write on an
    // unscripted node, or on a path that does not resolve, reports absence rather
    // than guessing.

    [[nodiscard]] std::string behaviour_of(const Uuid& node_id) const override {
        const Script* bound = host_.script(node_id);
        return bound == nullptr ? std::string{} : std::string(bound->name());
    }

    [[nodiscard]] std::optional<reflect::Value> read(const Uuid& node_id,
                                                     const std::vector<std::string>& path) const override {
        const Script* bound = host_.script(node_id);
        const ScriptDesc* desc = desc_of(node_id);
        if (bound == nullptr || desc == nullptr) {
            return std::nullopt;
        }
        const reflect::Property* property = reflect::resolve_property(desc->properties, path);
        if (property == nullptr) {
            return std::nullopt;
        }
        return reflect::read_property(*property, bound);
    }

    [[nodiscard]] bool write(const Uuid& node_id, const std::vector<std::string>& path,
                             const reflect::Value& value) override {
        Script* bound = host_.script(node_id);
        const ScriptDesc* desc = desc_of(node_id);
        if (bound == nullptr || desc == nullptr) {
            return false;
        }
        const reflect::Property* property = reflect::resolve_property(desc->properties, path);
        if (property == nullptr) {
            return false;
        }
        return reflect::write_property(*property, bound, value);
    }

    [[nodiscard]] std::optional<std::string> read_resource(const Uuid& node_id,
                                                           const std::vector<std::string>& path) const override {
        const Script* bound = host_.script(node_id);
        const ScriptDesc* desc = desc_of(node_id);
        if (bound == nullptr || desc == nullptr) {
            return std::nullopt;
        }
        const reflect::Property* property = reflect::resolve_property(desc->properties, path);
        if (property == nullptr || !property->resource_ref) {
            return std::nullopt;
        }
        return property->resource_ref(bound);
    }

    [[nodiscard]] bool write_resource(const Uuid& node_id, const std::vector<std::string>& path,
                                      std::string_view reference, ResourceManager& resources) override {
        Script* bound = host_.script(node_id);
        const ScriptDesc* desc = desc_of(node_id);
        if (bound == nullptr || desc == nullptr) {
            return false;
        }
        const reflect::Property* property = reflect::resolve_property(desc->properties, path);
        if (property == nullptr || !property->resolve_resource) {
            return false;
        }
        (void)property->resolve_resource(bound, reference, resources);
        return true;
    }

    // Drops every binding. Called before a different scene is loaded into the
    // document, so no script outlives the nodes it was bound to - including the
    // reload bookkeeping, so a stale binding to a node that is gone is not
    // rebound by a later module reload.
    void clear() noexcept {
        host_.clear();
        library_.clear_bindings();
    }

private:
    // The library owns the registry and the loaded images; the host owns the
    // live instances. Declared in this order so the host, which holds scripts
    // whose code lives in the images, is destroyed *first* (members destroy in
    // reverse declaration order) - the same ordering constraint the library
    // documents for itself, since destroying a script after its image is
    // unmapped would call into freed memory.
    ScriptLibrary library_;
    ScriptHost host_;
};

} // namespace editor
} // namespace fe
