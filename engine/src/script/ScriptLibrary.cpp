// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/script/ScriptLibrary.h"

#include "fe/core/FileSystem.h"
#include "fe/scene/Node.h"

#include <algorithm>
#include <utility>

namespace fe {
namespace {

// Resolves a module's logical path to a real file: an explicit path is used as
// given; a bare name gets the platform's shared-library suffix (ADR-0061).
[[nodiscard]] std::string resolve_module_path(std::string_view path) {
    if (fs::exists(path)) {
        return std::string(path);
    }
    std::string decorated = DynamicLibrary::decorate(path);
    if (decorated != path && fs::exists(decorated)) {
        return decorated;
    }
    // Neither exists: return the decorated name so the load error names the file
    // the caller most likely meant.
    return decorated;
}

} // namespace

ScriptLibrary::~ScriptLibrary() {
    // Order matters. Each registered behaviour is a std::function whose target
    // (and whose factory lambda) lives inside a module image. Destroying the
    // registry after the images are unloaded would run that code from unmapped
    // memory, so the registry is cleared here while the images are still mapped;
    // the members then unload in reverse declaration order.
    registry_ = ScriptRegistry{};
    modules_.clear();
}

Error ScriptLibrary::load_module(std::string_view path) {
    if (path.empty()) {
        return Error("ScriptLibrary::load_module: empty path");
    }

    const std::string resolved = resolve_module_path(path);
    if (modules_.find(resolved) != modules_.end()) {
        return Error::none();
    }

    Module module;
    Error error = open_module(resolved, module);
    if (error) {
        return error;
    }

    FE_LOG_INFO("script module '%s' loaded with %zu behaviour(s)", resolved.c_str(), module.behaviours.size());
    modules_.emplace(resolved, std::move(module));
    return Error::none();
}

Error ScriptLibrary::reload_module(ScriptHost& host, std::string_view module_id, std::string_view new_image_path) {
    if (module_id.empty()) {
        return Error("ScriptLibrary::reload_module: empty module id");
    }

    const std::string resolved_id = resolve_module_path(module_id);
    auto it = modules_.find(resolved_id);
    if (it == modules_.end()) {
        return Error("ScriptLibrary::reload_module: module is not loaded: " + resolved_id);
    }

    const std::vector<std::string> old_behaviours = it->second.behaviours;
    // An empty replacement path means "reload from the same file" (the ordinary
    // rebuild-in-place case); otherwise the new code is at the given path.
    const std::string image_path = new_image_path.empty() ? resolved_id : resolve_module_path(new_image_path);

    // Step 1: find every node this module scripted, and destroy its script while
    // the old code is still loaded. detach_script runs the script's detach()
    // inside the image, so no instance may outlive the image below. The library's
    // own binding record is the source of truth (not Script::name(), which is a
    // human-facing label), and the node pointer is captured before detaching,
    // which removes the host's binding.
    std::vector<Binding> to_rebind;
    for (const Binding& binding : bindings_) {
        if (std::find(old_behaviours.begin(), old_behaviours.end(), binding.behaviour) == old_behaviours.end()) {
            continue; // provided by a different module; leave it alone
        }
        if (!host.has_script(binding.node_id)) {
            continue; // already detached elsewhere
        }
        Node* node = host.node(binding.node_id);
        host.detach_script(binding.node_id);
        if (node != nullptr) {
            to_rebind.push_back(Binding{binding.node_id, binding.behaviour, node});
        }
    }

    // Step 2: forget the old behaviours and unload the image. Nothing from it is
    // live any more.
    for (const std::string& behaviour : old_behaviours) {
        registry_.remove_script(behaviour);
    }
    it->second.image.unload();
    modules_.erase(it);

    // Step 3: load the new image and register its behaviours.
    Module module;
    Error error = open_module(image_path, module);
    if (error) {
        // The module is dropped: its behaviours are unregistered and the nodes it
        // scripted stay unscripted. A fresh script cannot be built for a behaviour
        // that no longer exists, so rebinding is deliberately skipped.
        for (const Binding& binding : to_rebind) {
            FE_LOG_ERROR("script module '%s' failed to reload; node %s left unscripted: %s", resolved_id.c_str(),
                         binding.node_id.to_string().c_str(), error.message().c_str());
        }
        return error;
    }
    modules_.emplace(resolved_id, std::move(module));

    // Step 4: give every node that had a script from this module a fresh one,
    // built from the new code. The binding list is updated in place so a later
    // reload still finds these nodes.
    for (Binding& binding : bindings_) {
        for (const Binding& rebind : to_rebind) {
            if (binding.node_id == rebind.node_id) {
                binding.node = rebind.node;
            }
        }
    }
    rebind_after_reload(host, to_rebind);
    return Error::none();
}

bool ScriptLibrary::is_module_loaded(std::string_view module_id) const noexcept {
    return modules_.find(std::string(module_id)) != modules_.end();
}

std::vector<std::string> ScriptLibrary::module_ids() const {
    std::vector<std::string> ids;
    ids.reserve(modules_.size());
    for (const auto& entry : modules_) {
        ids.push_back(entry.first);
    }
    return ids;
}

Error ScriptLibrary::attach(ScriptHost& host, Node& node, std::string_view behaviour) {
    std::unique_ptr<Script> script = registry_.create(behaviour);
    if (!script) {
        return Error("ScriptLibrary::attach: no loaded module provides behaviour '" + std::string(behaviour) + "'");
    }

    const Uuid node_id = node.id();
    Error error = host.attach_script(node, std::move(script));
    if (error) {
        return error;
    }

    // Record the binding, replacing any earlier one for this node so "one script
    // per node" holds for the reload list too.
    for (Binding& binding : bindings_) {
        if (binding.node_id == node_id) {
            binding.behaviour = std::string(behaviour);
            binding.node = &node;
            return Error::none();
        }
    }
    bindings_.push_back(Binding{node_id, std::string(behaviour), &node});
    return Error::none();
}

bool ScriptLibrary::detach(ScriptHost& host, const Uuid& node_id) {
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
        if (it->node_id == node_id) {
            bindings_.erase(it);
            break;
        }
    }
    return host.detach_script(node_id);
}

Error ScriptLibrary::open_module(std::string_view path, Module& out) {
    Result<DynamicLibrary> loaded = DynamicLibrary::load(path);
    if (!loaded) {
        return loaded.error();
    }
    out.image = std::move(loaded.value());

    // Check the ABI before calling anything: a mismatched module must be refused,
    // not entered.
    using AbiVersionFn = u32 (*)();
    auto abi_version = out.image.symbol_as<AbiVersionFn>("fe_script_module_abi_version");
    if (abi_version == nullptr) {
        out.image.unload();
        return Error("script module '" + std::string(path) + "' does not export fe_script_module_abi_version");
    }
    const u32 version = abi_version();
    if (version != kScriptModuleAbiVersion) {
        out.image.unload();
        return Error("script module '" + std::string(path) + "' has ABI version " + std::to_string(version) +
                     ", expected " + std::to_string(kScriptModuleAbiVersion));
    }

    auto entry = out.image.symbol_as<ScriptModuleEntry>("fe_script_module_register");
    if (entry == nullptr) {
        out.image.unload();
        return Error("script module '" + std::string(path) + "' does not export fe_script_module_register");
    }

    // The entry point registers directly into registry_. Diff the names around
    // the call so the module records exactly the behaviours it added.
    const std::vector<std::string> before = registry_.names();
    const u32 registered = entry(registry_);
    const std::vector<std::string> after = registry_.names();
    for (const std::string& name : after) {
        if (std::find(before.begin(), before.end(), name) == before.end()) {
            out.behaviours.push_back(name);
        }
    }

    if (registered != out.behaviours.size()) {
        FE_LOG_WARN("script module '%s' reported %u behaviour(s) but registered %zu", std::string(path).c_str(),
                    registered, out.behaviours.size());
    }
    return Error::none();
}

void ScriptLibrary::rebind_after_reload(ScriptHost& host, const std::vector<Binding>& to_rebind) {
    for (const Binding& rebind : to_rebind) {
        if (rebind.node == nullptr || host.has_script(rebind.node_id)) {
            continue;
        }
        std::unique_ptr<Script> script = registry_.create(rebind.behaviour);
        if (!script) {
            FE_LOG_WARN("script module reload did not re-provide behaviour '%s'; node %s stays unscripted",
                        rebind.behaviour.c_str(), rebind.node_id.to_string().c_str());
            continue;
        }
        const Error error = host.attach_script(*rebind.node, std::move(script));
        if (error) {
            FE_LOG_ERROR("script module reload could not rebind node %s: %s", rebind.node_id.to_string().c_str(),
                         error.message().c_str());
        }
    }
}

} // namespace fe
