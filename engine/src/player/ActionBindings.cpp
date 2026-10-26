// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/player/ActionBindings.h"

#include "fe/core/FileSystem.h"
#include "fe/platform/InputActions.h"
#include "fe/platform/InputState.h"
#include "fe/platform/KeyCode.h"

#include <nlohmann/json.hpp>

#include <string>

namespace fe::player {
namespace {

using json = nlohmann::json;

// A `mouse` entry's word for a button. Spelled out rather than numeric so a
// document reads the way a player would describe it, and so a wrong value is a
// sentence rather than a magic number.
[[nodiscard]] Result<MouseButton> mouse_button_from_name(std::string_view name) {
    if (name == "left") {
        return MouseButton::Left;
    }
    if (name == "right") {
        return MouseButton::Right;
    }
    if (name == "middle") {
        return MouseButton::Middle;
    }
    return Error("input bindings: unknown mouse button '" + std::string(name) + "' (expected left, right or middle)");
}

// Reads a region's four numbers from a JSON array. The region must be exactly
// four numbers and all of them normalised; anything else is a document the
// writer would not have produced, so it is rejected rather than clamped or
// guessed at.
[[nodiscard]] Error read_region(const json& value, const std::string& action, f32& x0, f32& y0, f32& x1, f32& y1) {
    if (!value.is_array() || value.size() != 4) {
        return Error("input bindings: action '" + action + "' has a touch region that is not [x0, y0, x1, y1]");
    }
    f32 corners[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (usize i = 0; i < 4; ++i) {
        if (!value[i].is_number()) {
            return Error("input bindings: action '" + action + "' has a non-numeric touch region corner");
        }
        corners[i] = value[i].get<f32>();
        if (corners[i] < 0.0f || corners[i] > 1.0f) {
            // A pixel coordinate here is the mistake this catches: a region is
            // normalised, so a value outside 0..1 means the author wrote the
            // wrong units, which would bind a rectangle that never matches.
            return Error("input bindings: action '" + action +
                         "' has a touch region outside 0..1; regions are normalised, not pixels");
        }
    }
    x0 = corners[0];
    y0 = corners[1];
    x1 = corners[2];
    y1 = corners[3];
    return Error::none();
}

// Reads an optional array of strings, calling `emit` for each. A present but
// non-array value, or a non-string element, is an error: the writer emits an
// array of strings, so anything else is a hand-edited document with a typo.
template <typename Emit>
[[nodiscard]] Error read_string_array(const json& holder, const char* field, const std::string& action, Emit&& emit) {
    if (!holder.contains(field)) {
        return Error::none();
    }
    const json& values = holder.at(field);
    if (!values.is_array()) {
        return Error("input bindings: action '" + action + "' has a '" + field + "' that is not an array");
    }
    for (const json& value : values) {
        if (!value.is_string()) {
            return Error("input bindings: action '" + action + "' has a non-string '" + field + "' entry");
        }
        emit(value.get<std::string>());
    }
    return Error::none();
}

} // namespace

Result<ActionBindings> ActionBindings::from_json(std::string_view text) {
    // The non-throwing overload, because a web build has exceptions disabled and
    // this reader is reachable from the shipped player (the same trap
    // GltfLoader::external_dependencies hit; see ADR-0088's follow-up).
    const json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        return Error("input bindings: parse error");
    }
    if (!document.is_object()) {
        return Error("input bindings: the document is not an object");
    }

    const std::string format = document.value("format", std::string{});
    if (format != kFormatName) {
        return Error("input bindings: not a fork input document (format='" + format + "')");
    }
    const i32 version = document.value("format_version", 0);
    if (version != kFormatVersion) {
        return Error("input bindings: format version " + std::to_string(version) + " is not supported");
    }

    if (!document.contains("actions")) {
        return Error("input bindings: no 'actions' array");
    }
    const json& actions = document.at("actions");
    if (!actions.is_array()) {
        return Error("input bindings: 'actions' is not an array");
    }

    ActionBindings parsed;
    // Set by a lambda that has no other way to fail (a no-exceptions build
    // cannot throw out of one); checked immediately after each walk.
    Error parse_error;
    for (const json& entry : actions) {
        if (!entry.is_object()) {
            return Error("input bindings: an action entry is not an object");
        }
        const std::string name = entry.value("name", std::string{});
        if (name.empty()) {
            return Error("input bindings: an action entry has no name");
        }
        // `replace` is optional and must be a boolean when present: a document
        // that writes `"replace": "yes"` is a typo, not a binding.
        ActionDecl decl;
        decl.name = name;
        if (entry.contains("replace")) {
            if (!entry.at("replace").is_boolean()) {
                return Error("input bindings: action '" + name + "' has a non-boolean 'replace'");
            }
            decl.replace = entry.at("replace").get<bool>();
        }
        parsed.actions.push_back(std::move(decl));

        // Keys, by UI Events code. A code the engine does not model is an error
        // rather than a dropped binding, so a typo fails the load instead of
        // producing a control that silently does nothing.
        if (Error error = read_string_array(entry, "keys", name,
                                            [&](std::string code) {
                                                if (key_from_code(code) == Key::Unknown) {
                                                    // Throwing out of the lambda is not an option in a
                                                    // no-exceptions build, so the error is recorded and the
                                                    // caller checks it after the walk.
                                                    parse_error = Error("input bindings: action '" + name +
                                                                        "' names unknown key code '" + code + "'");
                                                    return;
                                                }
                                                parsed.keys.push_back(KeyBinding{name, std::move(code)});
                                            });
            error) {
            return error;
        }
        if (parse_error) {
            return parse_error;
        }

        if (Error error = read_string_array(entry, "mouse", name,
                                            [&](std::string button) {
                                                auto resolved = mouse_button_from_name(button);
                                                if (!resolved) {
                                                    parse_error = resolved.error();
                                                    return;
                                                }
                                                parsed.mouse.push_back(MouseBinding{name, std::move(button)});
                                            });
            error) {
            return error;
        }
        if (parse_error) {
            return parse_error;
        }

        if (entry.contains("touch")) {
            const json& regions = entry.at("touch");
            if (!regions.is_array()) {
                return Error("input bindings: action '" + name + "' has a 'touch' that is not an array");
            }
            for (const json& region : regions) {
                TouchRegion parsed_region{name, 0.0f, 0.0f, 0.0f, 0.0f};
                if (Error error = read_region(region, name, parsed_region.x0, parsed_region.y0, parsed_region.x1,
                                              parsed_region.y1);
                    error) {
                    return error;
                }
                parsed.touch_regions.push_back(parsed_region);
            }
        }
    }

    return parsed;
}

Result<ActionBindings> ActionBindings::load(std::string_view path) {
    auto text = fs::read_text_file(path);
    if (!text) {
        return text.error();
    }
    return from_json(text.value());
}

void ActionBindings::apply(ActionMap& map) const {
    // Resolve every action name to a handle, reusing one already declared so a
    // document applied after the engine's own declarations rebinds the same
    // action rather than adding a second of the same name. `find` is the
    // ActionMap contract for exactly this (a name resolves to one handle).
    std::vector<std::pair<std::string, ActionMap::Id>> resolved;
    resolved.reserve(actions.size());
    for (const ActionDecl& decl : actions) {
        ActionMap::Id id = map.find(decl.name);
        if (id == ActionMap::kNone) {
            id = map.add(decl.name);
        }
        // `replace` clears only the kinds this action then binds, so a document
        // that redefines a touch layout does not also drop the action's keys. The
        // clearing happens before the bindings are installed, below.
        if (decl.replace) {
            if (binds_key(decl.name)) {
                map.clear_key_bindings(id);
            }
            if (binds_mouse(decl.name)) {
                map.clear_mouse_bindings(id);
            }
            if (binds_touch(decl.name)) {
                map.clear_touch_regions(id);
            }
        }
        resolved.emplace_back(decl.name, id);
    }

    const auto handle_of = [&resolved](const std::string& name) -> ActionMap::Id {
        for (const auto& [candidate, id] : resolved) {
            if (candidate == name) {
                return id;
            }
        }
        return ActionMap::kNone;
    };

    for (const KeyBinding& binding : keys) {
        map.bind_key(handle_of(binding.action), key_from_code(binding.code));
    }
    for (const MouseBinding& binding : mouse) {
        auto button = mouse_button_from_name(binding.button);
        if (button) {
            map.bind_mouse_button(handle_of(binding.action), button.value());
        }
    }
    for (const TouchRegion& region : touch_regions) {
        map.bind_touch_region(handle_of(region.action), region.x0, region.y0, region.x1, region.y1);
    }
}

bool ActionBindings::binds_key(const std::string& action) const {
    for (const KeyBinding& binding : keys) {
        if (binding.action == action) {
            return true;
        }
    }
    return false;
}

bool ActionBindings::binds_mouse(const std::string& action) const {
    for (const MouseBinding& binding : mouse) {
        if (binding.action == action) {
            return true;
        }
    }
    return false;
}

bool ActionBindings::binds_touch(const std::string& action) const {
    for (const TouchRegion& region : touch_regions) {
        if (region.action == action) {
            return true;
        }
    }
    return false;
}

} // namespace fe::player
