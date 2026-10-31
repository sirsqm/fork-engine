// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Log.h"
#include "fe/core/Types.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptDiagnostics.h"
#include "fe/systems/FrameContext.h"

#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fe {

// Owns the live Script instances and drives them once per frame.
//
// The host is the boundary of the scripting seam. It is the only thing that
// calls a script's attach/detach/update, and it is where a failing script is
// contained: a script that returns an Error or throws is logged, disabled, and
// skipped, while the rest keep running and the frame keeps going (ADR-0003,
// ADR-0044).
//
// Instances are keyed by the node's stable Uuid, not by pointer. That is the
// same identity the serializer persists and the operation model (ADR-0047) names
// its objects by, so a script's binding survives a save/load without a parallel
// id scheme. One script per node: attach_script replaces any script already on
// that node.
//
// The host is deliberately small and headless: no window, no RHI, no editor.
// A ScriptSystem drives it from the ordinary SceneSchedule, which is what makes
// scripts run in the frame loop, in play-in-editor, and in a test through the
// identical path.
//
// Contract: a script's update() must not attach or detach scripts. Attaching
// and detaching are structural changes made by the game/editor between frames,
// not from inside a frame.
class ScriptHost {
public:
    ScriptHost() = default;

    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Binds a script to a node, taking ownership. Any script already attached
    // to that node is detached first, so "one script per node" holds, and the
    // replacement keeps its place in the update order.
    //
    // The script's attach() runs here; if it returns an error the new script is
    // destroyed, the node is left with no script, and the error is returned.
    // The node reference must stay valid until the script is detached.
    Error attach_script(Node& node, std::unique_ptr<Script> script) {
        if (!script) {
            return Error("attach_script: null script");
        }

        auto it = index_.find(node.id());
        if (it != index_.end()) {
            const usize position = it->second;
            Entry& entry = scripts_[position];
            if (entry.node != nullptr) {
                entry.script->detach(*entry.node);
            }
            entry.script = std::move(script);
            entry.node = &node;
            entry.failed = false;
            Error error = bind(*entry.script, node);
            if (error) {
                // The replacement refused the node: drop the entry so the node
                // ends up unscripted rather than holding a half-bound script.
                scripts_.erase(scripts_.begin() + static_cast<std::ptrdiff_t>(position));
                reindex();
            }
            return error;
        }

        Error error = bind(*script, node);
        if (error) {
            return error;
        }
        Entry entry;
        entry.node_id = node.id();
        entry.node = &node;
        entry.script = std::move(script);
        index_.emplace(entry.node_id, scripts_.size());
        scripts_.push_back(std::move(entry));
        return Error::none();
    }

    // Unbinds and destroys the node's script, calling detach() first. Returns
    // true if a script was attached. Safe to call for a node with no script.
    bool detach_script(const Uuid& node_id) {
        auto it = index_.find(node_id);
        if (it == index_.end()) {
            return false;
        }
        Entry& entry = scripts_[it->second];
        if (entry.node != nullptr) {
            entry.script->detach(*entry.node);
        }
        entry.script->set_node(nullptr);
        scripts_.erase(scripts_.begin() + static_cast<std::ptrdiff_t>(it->second));
        reindex();
        return true;
    }

    // Convenience overload that looks the node up in `scene` by id. Returns
    // false if the scene has no such node or the node has no script.
    bool detach_script(Scene& scene, const Uuid& node_id) {
        if (scene.find_node(node_id) == nullptr) {
            return false;
        }
        return detach_script(node_id);
    }

    // True if the node currently has a script.
    [[nodiscard]] bool has_script(const Uuid& node_id) const noexcept { return index_.find(node_id) != index_.end(); }

    // The node's script, or nullptr.
    [[nodiscard]] Script* script(const Uuid& node_id) noexcept {
        auto it = index_.find(node_id);
        return it == index_.end() ? nullptr : scripts_[it->second].script.get();
    }

    [[nodiscard]] const Script* script(const Uuid& node_id) const noexcept {
        auto it = index_.find(node_id);
        return it == index_.end() ? nullptr : scripts_[it->second].script.get();
    }

    // Runs every attached, enabled script once, in attachment order. A script
    // that fails is contained and disabled; see the class comment.
    void update(const FrameContext& context) {
        ++frame_;
        for (Entry& entry : scripts_) {
            if (entry.failed || !entry.script->enabled()) {
                continue;
            }
            Error error;
            try {
                error = entry.script->update(context);
            } catch (const std::exception& exception) {
                error = Error(std::string("threw: ") + exception.what());
            } catch (...) {
                error = Error("threw an unknown exception");
            }
            if (error) {
                report_failure(entry, error);
            }
        }
    }

    [[nodiscard]] usize script_count() const noexcept { return scripts_.size(); }

    // A live binding, for a consumer that enumerates what is scripted. The host
    // still owns the script; the view borrows it and is valid until the next
    // attach, detach, or clear. The scene serializer reads this to persist which
    // node runs which behaviour (ADR-0063).
    struct BindingView {
        Uuid node_id;
        Script* script = nullptr;
    };

    // Every live binding, in update order. Ordered so a document written twice
    // from the same host is identical.
    [[nodiscard]] std::vector<BindingView> bindings() const {
        std::vector<BindingView> out;
        out.reserve(scripts_.size());
        for (const Entry& entry : scripts_) {
            out.push_back(BindingView{entry.node_id, entry.script.get()});
        }
        return out;
    }

    // The node a script is bound to, or nullptr. The scene owns the node; the
    // host only reports the binding. A module loader uses this to rebind a script
    // after a hot reload (ADR-0061).
    [[nodiscard]] Node* node(const Uuid& node_id) const noexcept {
        auto it = index_.find(node_id);
        return it == index_.end() ? nullptr : scripts_[it->second].node;
    }

    // The number of scripts currently disabled by a failure.
    [[nodiscard]] usize failed_count() const noexcept {
        usize count = 0;
        for (const Entry& entry : scripts_) {
            if (entry.failed) {
                ++count;
            }
        }
        return count;
    }

    // The record of contained failures, for a tool to read. The host is the only
    // writer; a console panel or a test reads it. Bounded, and never cleared by
    // the host, so a failure survives until a reader clears it.
    [[nodiscard]] ScriptDiagnostics& diagnostics() noexcept { return diagnostics_; }
    [[nodiscard]] const ScriptDiagnostics& diagnostics() const noexcept { return diagnostics_; }

    // Unbinds and destroys every script. The host is reusable afterwards.
    void clear() {
        for (Entry& entry : scripts_) {
            if (entry.node != nullptr) {
                entry.script->detach(*entry.node);
            }
            entry.script->set_node(nullptr);
        }
        scripts_.clear();
        index_.clear();
    }

private:
    // Attachment order, so update() is deterministic and matches what the user
    // attached. The map is the lookup by id; the vector is the order.
    struct Entry {
        Uuid node_id;
        Node* node = nullptr;
        std::unique_ptr<Script> script;
        bool failed = false;
    };

    // Runs attach() and reports a refusal with the node's name for context.
    // On success the script's back-pointer is set, so node() is valid for the
    // whole time the script is bound; on a refusal it stays null.
    static Error bind(Script& script, Node& node) {
        Error error = script.attach(node);
        if (error) {
            FE_LOG_ERROR("script '%s' refused node '%s': %s", std::string(script.name()).c_str(), node.name().c_str(),
                         error.message().c_str());
            return error;
        }
        script.set_node(&node);
        return Error::none();
    }

    void reindex() {
        index_.clear();
        for (usize i = 0; i < scripts_.size(); ++i) {
            index_.emplace(scripts_[i].node_id, i);
        }
    }

    void report_failure(Entry& entry, const Error& error) {
        const char* node_name = entry.node != nullptr ? entry.node->name().c_str() : "<detached>";
        FE_LOG_ERROR("script '%s' on node '%s' (%s) failed and was disabled: %s",
                     std::string(entry.script->name()).c_str(), node_name, entry.node_id.to_string().c_str(),
                     error.message().c_str());
        // Keep the failure where a tool can read it, not only in the log: the
        // log line scrolls away, and this is the one thing a game author needs
        // to see when a behaviour misbehaves (M7 debugging support).
        diagnostics_.record(entry.node_id, node_name, entry.script->name(), error.message(), frame_);
        entry.script->set_enabled(false);
        entry.failed = true;
    }

    std::vector<Entry> scripts_;
    std::unordered_map<Uuid, usize> index_;
    ScriptDiagnostics diagnostics_;
    // Counts update() calls, so a recorded failure can name the frame it
    // happened on. Not a clock: the host has no frame context of its own, and a
    // monotonic count is enough to order failures and to tell "the same frame"
    // from "a later one".
    u64 frame_ = 0;
};

} // namespace fe
