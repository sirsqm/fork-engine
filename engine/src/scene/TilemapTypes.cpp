// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene/TilemapTypes.h"

#include "fe/reflect/Property.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/Resource.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene/Tilemap.h"

#include <memory>
#include <string>

// The tilemap component registration, in the same style as ComponentTypes.cpp.
//
// The grid is a variable-length list of ints. The reflection model (ADR-0005,
// ADR-0006) deliberately describes scalar leaves and fixed-size groups only: it
// has no list kind, because the two consumers it was built for (the scene
// serializer and the inspector) only needed leaves. Rather than widen that model
// for one component, the grid is exposed as a single String property holding the
// component's own compact JSON. The scene serializer round-trips it like any
// other string, and the component parses it in its setter. This is the trade
// AnimationStateMachine already made for its graph (ADR-0059); a dedicated tile
// editor is a follow-up. See ADR-0062.
//
// The tilemap lives in the scene module, not a module above it, because a tile is
// drawn by the same renderer a sprite is. Registering it here keeps the render
// extraction seam unaware of which module produced an instance.

namespace fe::reflect {
namespace {

// The grid as one string: the getter serializes the map, the setter parses a
// document back into it. A malformed document is rejected by the setter (it
// returns false), so a hand-edited scene keeps the default rather than loading a
// half-built grid.
Property make_grid_property() {
    Property property;
    property.name = "grid";
    property.value_kind = ValueKind::String;
    property.getter = [](const void* object) -> Value { return static_cast<const Tilemap*>(object)->to_json(); };
    property.setter = [](void* object, const Value& value) -> bool {
        const auto* as_string = std::get_if<std::string>(&value);
        if (as_string == nullptr) {
            return false;
        }
        return static_cast<Tilemap*>(object)->from_json(*as_string);
    };
    property.hint = Hint::Multiline;
    property.default_value = std::string{};
    return property;
}

void register_tilemap(Registry& registry) {
    Tilemap defaults;

    TypeDesc desc;
    desc.name = Tilemap::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<Tilemap>()); };

    desc.properties.push_back(make_vector_group<Tilemap, Vec2>("tile_size", &Tilemap::tile_size, "Vec2", {"x", "y"}));
    desc.properties.push_back(
        with_color(make_vector_group<Tilemap, Vec4>("color", &Tilemap::color, "Vec4", {"r", "g", "b", "a"})));
    desc.properties.push_back(make_scalar<Tilemap, i32>("layer", &Tilemap::layer, ValueKind::Int, &defaults));
    desc.properties.push_back(make_scalar<Tilemap, u32>("columns", &Tilemap::columns, ValueKind::Int, &defaults));
    desc.properties.push_back(make_scalar<Tilemap, u32>("rows", &Tilemap::rows, ValueKind::Int, &defaults));
    desc.properties.push_back(make_resource_ref<Tilemap, std::shared_ptr<Resource>>(
        "texture", &Tilemap::texture, "Texture2D",
        [](const std::shared_ptr<Resource>& texture) -> std::string {
            // Store the manager cache key (including the "tex:" scheme) rather
            // than the resource's own path, so that reloading resolves the same
            // GPU texture rather than the CPU image sharing the file.
            return texture != nullptr ? Texture2DLoader::reference_key(texture->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));

    // The grid is applied last. It is the only source of `width` and `height`
    // (those are not properties), so a document cannot set them into disagreeing
    // with the cells: the cells arrive with their own dimensions.
    desc.properties.push_back(make_grid_property());

    registry.register_type(std::move(desc));
}

} // namespace

void register_tilemap_types() {
    register_tilemap(Registry::instance());
}

} // namespace fe::reflect
