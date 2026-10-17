// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptDesc.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fe {

// Maps a behaviour name to a factory that constructs it.
//
// This is what lets a script be created by name rather than by concrete type,
// which is what the editor and a future data-driven loader need: a scene (or a
// future script asset) says "behaviour = SpinSystem" and something has to turn
// that string into a live Script. It mirrors the shape of reflect::Registry
// (ADR-0006): register once, look up by name everywhere after, and a later
// registration of the same name replaces the earlier one.
//
// It is deliberately a value type the caller owns, not a process-global
// singleton like reflect::Registry. Scripts are game code, and a game may want
// its own set; keeping the registry out of global state also keeps the tests
// independent of each other. A game that wants one shared registry can hold one
// and pass it around.
//
// Threading: registration happens during single-threaded startup; after that
// the registry is read-only and safe to read from any thread.
class ScriptRegistry {
public:
    using Factory = std::function<std::unique_ptr<Script>()>;

    // Registers (or replaces) a behaviour. A later registration of the same
    // name replaces the earlier one, so re-running a test in the same process
    // is well defined.
    void register_script(std::string name, Factory factory) {
        register_desc(ScriptDesc{std::move(name), std::move(factory), {}});
    }

    // Registers (or replaces) a behaviour from a full description, including the
    // reflected properties that make its fields visible to the inspector and the
    // serializer (ADR-0063). A description with no factory is rejected, because
    // nothing could then be constructed from the name.
    void register_desc(ScriptDesc desc) {
        if (!desc.is_valid()) {
            return;
        }
        for (auto& entry : descs_) {
            if (entry.name == desc.name) {
                entry = std::move(desc);
                return;
            }
        }
        descs_.push_back(std::move(desc));
    }

    // Convenience for a concrete Script type with a default constructor.
    template <typename T>
    void register_type(std::string name) {
        static_assert(std::is_base_of_v<Script, T>, "T must derive from fe::Script");
        register_script(std::move(name), [] { return std::unique_ptr<Script>(new T()); });
    }

    // Constructs a default instance of the named behaviour, or nullptr if the
    // name is not registered.
    [[nodiscard]] std::unique_ptr<Script> create(std::string_view name) const {
        for (const auto& entry : descs_) {
            if (entry.name == name) {
                return entry.factory ? entry.factory() : nullptr;
            }
        }
        return nullptr;
    }

    // The description of the named behaviour, or nullptr. This is what the
    // inspector and the serializer read to learn a script's properties.
    [[nodiscard]] const ScriptDesc* find(std::string_view name) const noexcept {
        for (const auto& entry : descs_) {
            if (entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    }

    // Removes a registration, so a name stops resolving to a factory. Returns
    // true if a registration was removed. A module loader uses this to forget
    // the behaviours a module provided when that module is dropped (ADR-0061).
    bool remove_script(std::string_view name) {
        for (auto it = descs_.begin(); it != descs_.end(); ++it) {
            if (it->name == name) {
                descs_.erase(it);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept { return find(name) != nullptr; }

    [[nodiscard]] usize script_count() const noexcept { return descs_.size(); }

    // Every registered behaviour name, in registration order.
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(descs_.size());
        for (const auto& entry : descs_) {
            out.push_back(entry.name);
        }
        return out;
    }

private:
    std::vector<ScriptDesc> descs_;
};

} // namespace fe
