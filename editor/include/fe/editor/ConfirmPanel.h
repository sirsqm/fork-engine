// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/editor/Panel.h"

namespace fe::editor {

// A modal yes/no prompt, shown before a destructive action (replacing the
// document, quitting with unsaved changes).
//
// The panel is a pure projection of the context's confirmation state: it draws
// the message the host set and, on a click, emits `ConfirmDiscard` or
// `CancelDiscard` through the context's file-request channel. The host owns the
// decision of *what* the pending action is and performs it once confirmed, so
// the panel never touches the document. This is the same split every other panel
// uses, applied to a modal.
class ConfirmPanel final : public Panel {
public:
    ConfirmPanel() = default;

    [[nodiscard]] const char* name() const noexcept override { return "Confirm"; }

    void draw(EditorContext& context) override;
};

} // namespace fe::editor
