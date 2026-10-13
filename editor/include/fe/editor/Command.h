// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Scene;

namespace editor {

// One reversible edit to a scene.
//
// A command is the unit of undo. `execute()` performs the edit and `undo()`
// reverses it; the pair must be exact, so that undoing a command restores the
// scene to the state it had immediately before the command ran. A command owns
// whatever it removed (a node's subtree, a component) for as long as it is
// undone, which is what lets a delete be undone with the object's identity
// (its `Uuid`) intact.
//
// Commands are built and executed through `CommandStack`, never applied by hand.
// See docs/adr/0011-editor-undo-redo.md for why this model was chosen over a
// scene snapshot or an in-scene change log.
class Command {
public:
    virtual ~Command() = default;

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    // Performs the edit. Called once when the command is executed, and again on
    // every redo. Must be idempotent with respect to `undo()`: execute, undo,
    // execute leaves the scene in the same state as the first execute.
    virtual void execute(Scene& scene) = 0;

    // Reverses `execute()`. Called on undo.
    virtual void undo(Scene& scene) = 0;

    // A short human-readable name for the edit ("Create Node", "Set Sprite2D
    // layer"), for a menu label or a log line.
    [[nodiscard]] virtual std::string_view label() const noexcept = 0;

    // The key under which consecutive edits of the same kind merge into one
    // undo step. An empty key means "never coalesce". A continuous edit (a drag
    // that changes one property many times) reports a stable key; a discrete
    // edit (create, delete) reports none, so each is its own step.
    [[nodiscard]] virtual std::string coalesce_key() const { return {}; }

    // Merges a newer command of the same coalesce key into this one, so that a
    // single undo reverts the whole gesture. Returns false when the commands
    // cannot merge (the stack then pushes the newer command as a new step).
    // The default refuses, so a command opts in to coalescing explicitly.
    [[nodiscard]] virtual bool merge_with(const Command& newer) {
        (void)newer;
        return false;
    }

protected:
    Command() = default;
};

// The editor's undo/redo history: an ordered list of executed commands with a
// cursor marking the current position.
//
// Commands before the cursor are applied; commands after it are the redo
// history. Executing a new command discards everything after the cursor (the
// standard linear model - a new edit invalidates the redos that assumed the old
// state). `undo()` at the beginning and `redo()` at the end are no-ops.
//
// The stack owns its commands and never holds a raw `Scene`. A caller passes
// the scene to `execute()`, `undo()`, and `redo()`.
class CommandStack {
public:
    CommandStack() = default;

    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;

    // Executes `command` against `scene` and records it. If the command
    // coalesces with the current top (same non-empty key and a successful
    // merge), it is merged into the top and not pushed as a new step. Any redo
    // history is discarded. Returns true when the command was applied.
    bool execute(Scene& scene, std::unique_ptr<Command> command);

    // Reverses the command at the cursor and moves the cursor back. Returns
    // false (a no-op) when there is nothing to undo.
    bool undo(Scene& scene);

    // Re-applies the command after the cursor and moves the cursor forward.
    // Returns false (a no-op) when there is nothing to redo.
    bool redo(Scene& scene);

    [[nodiscard]] bool can_undo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool can_redo() const noexcept { return cursor_ < commands_.size(); }

    // The label of the command that undo() would reverse, or "" when there is
    // nothing to undo.
    [[nodiscard]] std::string_view undo_label() const noexcept;
    // The label of the command that redo() would re-apply, or "" when there is
    // nothing to redo.
    [[nodiscard]] std::string_view redo_label() const noexcept;

    // Ends the current coalescing group, so the next command starts a new undo
    // step even if it shares a key. The editor calls this when an edit gesture
    // ends (the mouse button is released after a drag).
    void break_coalescing() noexcept { coalescing_ = false; }

    // Discards all history. Called when the document is replaced (new/load), so
    // undo cannot reach back across documents.
    void clear() noexcept;

    // Number of commands currently applied (the cursor position).
    [[nodiscard]] usize undo_count() const noexcept { return cursor_; }
    // Number of commands available to redo.
    [[nodiscard]] usize redo_count() const noexcept { return commands_.size() - cursor_; }

private:
    // True when `command` may merge into the current top of the stack.
    [[nodiscard]] bool can_coalesce_with(const Command& command) const;

    std::vector<std::unique_ptr<Command>> commands_;
    usize cursor_ = 0;
    // Whether the top command is still open to coalescing. Set false by
    // break_coalescing() and after a merge, so two different gestures that share
    // a key do not merge into one another.
    bool coalescing_ = false;
};

} // namespace editor
} // namespace fe
