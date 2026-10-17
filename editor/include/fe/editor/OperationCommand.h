// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Uuid.h"
#include "fe/editor/Command.h"
#include "fe/serialization/Operation.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Scene;
class ResourceManager;

namespace serialization {
class ScriptTarget;
}

namespace editor {

// The editor's `Command` form of a serializable `Operation`.
//
// The editor keeps its `CommandStack` (docs/adr/0015-editor-undo-redo.md) and
// its `Command` interface; this adapter is what lets the panels build an
// `Operation` - the transmitted, replayable form of an edit
// (docs/adr/0047-local-operation-model.md) - and still push it onto that stack.
//
// `execute` applies the operation and, on the first call, captures the inverse
// *from the document* (see `serialization::inverse`). `undo` applies the
// recorded inverse. Deriving the inverse from the document rather than stashing
// ad-hoc state is what makes the operation set the same thing a replica would
// replay. A delete-like operation (DeleteNode, RemoveComponent) has no
// single-operation inverse; the object it removed is held here, and the inverse
// is the operation that restores that exact object at its old position.
//
// Consecutive edits of the same property coalesce: `merge_with` adopts the
// newer operation's value while keeping the original inverse, so one undo
// reverts a whole drag, exactly as `SetPropertyCommand` did.
class OperationCommand final : public Command {
public:
    // `resources` is borrowed and may be null (the headless case): a
    // `SetResource` operation cannot resolve without it and is a no-op, and a
    // `SetProperty` operation never needs it.
    //
    // `scripts` is the same seam for a `SetScriptProperty` operation: it is the
    // live script host a script property edit reaches, and may be null when no
    // script host is in play, in which case such an operation is a no-op. It must
    // outlive the command (the editor's catalog does).
    explicit OperationCommand(serialization::Operation operation, ResourceManager* resources = nullptr,
                              serialization::ScriptTarget* scripts = nullptr);
    ~OperationCommand() override;

    void execute(Scene& scene) override;
    void undo(Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override;

    [[nodiscard]] std::string coalesce_key() const override;
    [[nodiscard]] bool merge_with(const Command& newer) override;

    [[nodiscard]] const serialization::Operation& operation() const noexcept { return operation_; }

private:
    serialization::Operation operation_;
    ResourceManager* resources_ = nullptr;
    serialization::ScriptTarget* scripts_ = nullptr;

    // The inverse of the current operation, computed on the first execute.
    serialization::Operation inverse_;
    bool has_inverse_ = false;

    // A delete-like operation's removed object, held while the command is
    // undone so a restore keeps identity. When set, `undo` restores it.
    serialization::AppliedObjects captured_;
    bool has_capture_ = false;
};

} // namespace editor
} // namespace fe
