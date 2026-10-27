// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

class ActionMap;

namespace player {

// A game's input bindings as data: which actions exist, and what fires them.
//
// Why this exists
// ---------------
// `ActionMap` already lets a game bind a touch region to an action
// (`bind_touch_region`), and the built-in behaviours bind a conventional set
// (ADR-0077). But those bindings are written in C++. A game that wants "the left
// third steers, the right third jumps" has to say so in a source file, rebuild,
// and cannot change it without a compiler — which is the wrong shape for a
// shipped game, a rebinding screen, or a per-device layout.
//
// This is the `touch-gesture-regions` seam (docs/SHIPPING_QUEUE.md): the regions a game
// declares once, in data, and the loader that turns them into the bindings
// `ActionMap` already understands. It adds no new input mechanism — it is the
// same `bind_key`/`bind_mouse_button`/`bind_touch_region` calls, made from a
// document instead of from a function body, so a binding a game declares here is
// indistinguishable downstream from one the engine declared itself.
//
// Vocabulary
// ----------
// Keys are named by their UI Events `code` ("KeyA", "ArrowLeft", "Space"), the
// same physical naming `key_from_code` uses. That is deliberate: it is the
// spelling a browser reports and the spelling `code_from_key` writes back, so a
// document is portable between the web backend, a future rebinding screen, and
// the desktop backend's own physical-key mapping (ADR-0075). A code the engine
// does not model is an error, not a silently dropped binding: a typo should fail
// the load rather than produce a game where one control does nothing.
//
// Touch regions are normalised (0..1, origin top-left, y downward) and written
// as `[x0, y0, x1, y1]`. Normalised is what makes one declaration serve portrait
// and landscape and survive a resize (ADR-0077); a document that writes pixel
// coordinates is rejected rather than silently binding the wrong rectangle.
//
// The document
// ------------
//     {
//       "format": "fork_input",
//       "format_version": 1,
//       "actions": [
//         { "name": "MoveLeft", "keys": ["KeyA", "ArrowLeft"],
//           "touch": [[0.0, 0.0, 0.34, 1.0]] },
//         { "name": "Launch", "keys": ["Space"], "mouse": ["left"],
//           "touch": [[0.34, 0.0, 0.66, 1.0]] }
//       ]
//     }
//
// An action's `keys`, `mouse` and `touch` are each optional; an action with none
// of them is declared but unbound, which is how a game reserves a name a later
// document (or a rebinding screen) will bind.
//
// `"replace": true` on an action clears that action's bindings *of each kind it
// then binds* before adding the document's own. Without it a document can only
// add to what the engine declared; with it, a game replaces the built-in layout
// for the actions it names — its own touch regions, its own keys — while leaving
// the kinds it does not mention (and every action it does not name) untouched.
class ActionBindings {
public:
    // The document's marker and version. A reader rejects a wrong marker (a
    // document that is not bindings) and a version from the future, rather than
    // guessing at either.
    static constexpr const char* kFormatName = "fork_input";
    static constexpr i32 kFormatVersion = 1;

    // One key binding: an action name and the UI Events code that fires it.
    struct KeyBinding {
        std::string action;
        std::string code;
    };

    // One mouse binding: an action name and "left", "right" or "middle".
    struct MouseBinding {
        std::string action;
        std::string button;
    };

    // One touch region: an action name and a normalised rectangle.
    struct TouchRegion {
        std::string action;
        f32 x0;
        f32 y0;
        f32 x1;
        f32 y1;
    };

    // One action's declaration: its name and whether it replaces the bindings of
    // the kinds it binds (rather than adding to them).
    struct ActionDecl {
        std::string name;
        bool replace = false;
    };

    // The bindings the document declared, in the order it declared them. Kept as
    // data (rather than applied straight into a map) so a caller can inspect,
    // merge or reapply them, and so the parse is testable without an ActionMap.
    std::vector<KeyBinding> keys;
    std::vector<MouseBinding> mouse;
    std::vector<TouchRegion> touch_regions;

    // The action names the document declared, in order, including ones it bound
    // nothing to. `apply` declares exactly these, so a document is self-contained:
    // it does not have to assume the game declared the names first.
    std::vector<ActionDecl> actions;

    // Parses a document. Fails on malformed JSON, a wrong marker or version, a
    // wrong shape, an unknown key code, a non-normalised region, or a missing
    // action name — a document that would bind something the author did not mean.
    // Never returns a partial result, so a caller that got an Error has nothing
    // half-read to reason about.
    [[nodiscard]] static Result<ActionBindings> from_json(std::string_view text);

    // Reads and parses the file at `path`.
    [[nodiscard]] static Result<ActionBindings> load(std::string_view path);

    // Declares every action the document names on `map` (reusing an existing
    // handle of the same name, so applying a document after the engine's own
    // declarations rebinds rather than duplicates) and then installs its
    // bindings. Applying the same document twice is idempotent for the action
    // set; it appends duplicate bindings, which `ActionMap` treats as the same
    // binding held twice and answers identically.
    void apply(ActionMap& map) const;

private:
    // Whether the document binds this action of each kind, which is what decides
    // what `replace` clears.
    [[nodiscard]] bool binds_key(const std::string& action) const;
    [[nodiscard]] bool binds_mouse(const std::string& action) const;
    [[nodiscard]] bool binds_touch(const std::string& action) const;
};

} // namespace player
} // namespace fe
