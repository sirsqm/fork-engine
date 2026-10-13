// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/reflect/ComponentTypes.h"

#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/Resource.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Sprite2D.h"

#include <memory>
#include <string>

// The component registrations.
//
// Each block describes one component's properties once. The serializer, and
// later the inspector, read these descriptions instead of naming the component
// types. Adding a property here is the single edit that makes it both persist
// and (next milestone) appear in the inspector.
//
// Group members read and write the owning component, so `default_object`
// captures the value a freshly constructed component reports: that is what the
// serializer writes when a document omits a field.

namespace fe::reflect {
namespace {

void register_sprite2d(Registry& registry) {
    Sprite2D defaults;

    TypeDesc desc;
    desc.name = Sprite2D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<Sprite2D>()); };

    desc.properties.push_back(make_vector_group<Sprite2D, Vec2>("size", &Sprite2D::size, "Vec2", {"x", "y"}));
    desc.properties.push_back(
        make_vector_group<Sprite2D, Vec4>("color", &Sprite2D::color, "Vec4", {"r", "g", "b", "a"}));
    desc.properties.push_back(make_vector_group<Sprite2D, Vec4>("uv", &Sprite2D::uv, "Vec4", {"u0", "v0", "u1", "v1"}));
    desc.properties.push_back(make_scalar<Sprite2D, i32>("layer", &Sprite2D::layer, ValueKind::Int, &defaults));
    desc.properties.push_back(make_resource_ref<Sprite2D, std::shared_ptr<Resource>>(
        "texture", &Sprite2D::texture, "Texture2D",
        [](const std::shared_ptr<Resource>& texture) -> std::string {
            // Store the manager cache key (including the "tex:" scheme) rather
            // than the resource's own path, so that reloading resolves the same
            // GPU texture rather than the CPU image sharing the file.
            return texture != nullptr ? Texture2DLoader::reference_key(texture->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));

    registry.register_type(std::move(desc));
}

void register_camera2d(Registry& registry) {
    Camera2D defaults;

    TypeDesc desc;
    desc.name = Camera2D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<Camera2D>()); };

    desc.properties.push_back(make_scalar<Camera2D, f32>("zoom", &Camera2D::zoom, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_float_group("clear_color", "Vec4", {"r", "g", "b", "a"},
                         {float_getter<Camera2D>(&Camera2D::clear_r), float_getter<Camera2D>(&Camera2D::clear_g),
                          float_getter<Camera2D>(&Camera2D::clear_b), float_getter<Camera2D>(&Camera2D::clear_a)},
                         {float_setter<Camera2D>(&Camera2D::clear_r), float_setter<Camera2D>(&Camera2D::clear_g),
                          float_setter<Camera2D>(&Camera2D::clear_b), float_setter<Camera2D>(&Camera2D::clear_a)}));
    desc.properties.push_back(make_scalar<Camera2D, bool>("primary", &Camera2D::primary, ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

} // namespace

void register_builtin_types() {
    Registry& registry = Registry::instance();
    register_sprite2d(registry);
    register_camera2d(registry);
}

} // namespace fe::reflect
