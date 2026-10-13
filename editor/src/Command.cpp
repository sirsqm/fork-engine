// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/editor/Command.h"

#include "fe/scene/Scene.h"

namespace fe::editor {

bool CommandStack::can_coalesce_with(const Command& command) const {
    if (!coalescing_ || cursor_ == 0 || cursor_ != commands_.size()) {
        return false;
    }
    const Command& top = *commands_[cursor_ - 1];
    const std::string key = command.coalesce_key();
    return !key.empty() && key == top.coalesce_key();
}

bool CommandStack::execute(Scene& scene, std::unique_ptr<Command> command) {
    if (command == nullptr) {
        return false;
    }

    if (can_coalesce_with(*command) && commands_[cursor_ - 1]->merge_with(*command)) {
        // The gesture continued: the newer command was folded into the top of
        // the stack. Merging only adopts the newer value, so the top command is
        // re-executed to write it; nothing new is recorded.
        commands_[cursor_ - 1]->execute(scene);
        return true;
    }

    command->execute(scene);
    // A new command invalidates the redo history: those redos assumed the state
    // this edit just replaced.
    commands_.resize(cursor_);
    commands_.push_back(std::move(command));
    cursor_ = commands_.size();
    coalescing_ = true;
    return true;
}

bool CommandStack::undo(Scene& scene) {
    if (!can_undo()) {
        return false;
    }
    --cursor_;
    commands_[cursor_]->undo(scene);
    // A subsequent edit after an undo starts a new step; it must not merge into
    // the command that is now at the top.
    coalescing_ = false;
    return true;
}

bool CommandStack::redo(Scene& scene) {
    if (!can_redo()) {
        return false;
    }
    commands_[cursor_]->execute(scene);
    ++cursor_;
    coalescing_ = false;
    return true;
}

std::string_view CommandStack::undo_label() const noexcept {
    if (!can_undo()) {
        return {};
    }
    return commands_[cursor_ - 1]->label();
}

std::string_view CommandStack::redo_label() const noexcept {
    if (!can_redo()) {
        return {};
    }
    return commands_[cursor_]->label();
}

void CommandStack::clear() noexcept {
    commands_.clear();
    cursor_ = 0;
    coalescing_ = false;
}

} // namespace fe::editor
