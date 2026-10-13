// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Uuid.h"
#include "fe/scene/Transform.h"

#include <nlohmann/json.hpp>

// JSON conversions for the engine's value types.
//
// These are defined in namespace fe so nlohmann's ADL-based conversion finds
// them automatically. Keeping the conversions next to the types (rather than
// inside each serializer) means every serializer, and later the editor's
// property inspector, shares one canonical representation.
//
// The scene headers themselves do not include this file: serialization is a
// separate concern, and only code that actually persists data pays for the
// JSON dependency.

namespace fe {

inline void to_json(nlohmann::json& j, const Vec2& v) {
    j = nlohmann::json::array({v.x, v.y});
}

inline void from_json(const nlohmann::json& j, Vec2& v) {
    v.x = j.at(0).get<f32>();
    v.y = j.at(1).get<f32>();
}

inline void to_json(nlohmann::json& j, const Vec3& v) {
    j = nlohmann::json::array({v.x, v.y, v.z});
}

inline void from_json(const nlohmann::json& j, Vec3& v) {
    v.x = j.at(0).get<f32>();
    v.y = j.at(1).get<f32>();
    v.z = j.at(2).get<f32>();
}

inline void to_json(nlohmann::json& j, const Vec4& v) {
    j = nlohmann::json::array({v.x, v.y, v.z, v.w});
}

inline void from_json(const nlohmann::json& j, Vec4& v) {
    v.x = j.at(0).get<f32>();
    v.y = j.at(1).get<f32>();
    v.z = j.at(2).get<f32>();
    v.w = j.at(3).get<f32>();
}

inline void to_json(nlohmann::json& j, const Transform& t) {
    j = nlohmann::json{{"position", t.position}, {"rotation", t.rotation}, {"scale", t.scale}};
}

inline void from_json(const nlohmann::json& j, Transform& t) {
    // Default-construct the missing pieces so a hand-edited scene file that
    // omits, say, "scale" still loads with an identity scale.
    t.position = j.value("position", Vec3{});
    t.rotation = j.value("rotation", Vec3{});
    t.scale = j.value("scale", Vec3{1.0f, 1.0f, 1.0f});
}

inline void to_json(nlohmann::json& j, const Uuid& id) {
    j = id.to_string();
}

inline void from_json(const nlohmann::json& j, Uuid& id) {
    const std::string text = j.get<std::string>();
    if (!Uuid::try_parse(text, id)) {
        throw nlohmann::json::type_error::create(302, "invalid uuid: " + text, &j);
    }
}

} // namespace fe
