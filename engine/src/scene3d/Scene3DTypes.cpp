// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/scene3d/Scene3DTypes.h"

#include "fe/mesh/Mesh.h"
#include "fe/mesh/ObjLoader.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/Resource.h"
#include "fe/resources/Texture2DLoader.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"
#include "fe/scene3d/PointLight3D.h"
#include "fe/scene3d/SpotLight3D.h"

#include <memory>
#include <optional>

// The 3D scene component registrations, in the same style as ComponentTypes.cpp
// and PhysicsTypes.cpp: each block describes a component's properties once, and
// the serializer and the inspector read those descriptions rather than naming
// the types.

namespace fe::reflect {
namespace {

void register_camera3d(Registry& registry) {
    Camera3D defaults;

    TypeDesc desc;
    desc.name = Camera3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<Camera3D>()); };

    desc.properties.push_back(
        make_scalar<Camera3D, f32>("fov_degrees", &Camera3D::fov_degrees, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<Camera3D, f32>("near_plane", &Camera3D::near_plane, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<Camera3D, f32>("far_plane", &Camera3D::far_plane, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_float_group("clear_color", "Vec4", {"r", "g", "b", "a"},
                         {float_getter<Camera3D>(&Camera3D::clear_r), float_getter<Camera3D>(&Camera3D::clear_g),
                          float_getter<Camera3D>(&Camera3D::clear_b), float_getter<Camera3D>(&Camera3D::clear_a)},
                         {float_setter<Camera3D>(&Camera3D::clear_r), float_setter<Camera3D>(&Camera3D::clear_g),
                          float_setter<Camera3D>(&Camera3D::clear_b), float_setter<Camera3D>(&Camera3D::clear_a)}));
    desc.properties.push_back(make_scalar<Camera3D, bool>("primary", &Camera3D::primary, ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

void register_mesh_renderer3d(Registry& registry) {
    MeshRenderer3D defaults;

    TypeDesc desc;
    desc.name = MeshRenderer3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<MeshRenderer3D>()); };

    desc.properties.push_back(make_vector_group<MeshRenderer3D, Vec4>("base_color", &MeshRenderer3D::base_color, "Vec4",
                                                                      {"r", "g", "b", "a"}));
    desc.properties.push_back(
        make_scalar<MeshRenderer3D, f32>("specular", &MeshRenderer3D::specular, ValueKind::Float, &defaults));
    desc.properties.push_back(make_resource_ref<MeshRenderer3D, std::shared_ptr<Resource>>(
        "mesh", &MeshRenderer3D::mesh, "Mesh",
        [](const std::shared_ptr<Resource>& mesh) -> std::string {
            // Store the loader's reference key rather than the resource's own
            // path, so that reloading resolves the same cached mesh.
            return mesh != nullptr ? ObjLoader::reference_key(mesh->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));
    desc.properties.push_back(make_resource_ref<MeshRenderer3D, std::shared_ptr<Resource>>(
        "texture", &MeshRenderer3D::texture, "Texture2D",
        [](const std::shared_ptr<Resource>& texture) -> std::string {
            return texture != nullptr ? Texture2DLoader::reference_key(texture->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));

    // The authored material, edited as one nested group: the override flag and,
    // beside it, the material's own fields. Describing the material here rather
    // than as a registered type keeps it a plain value the component owns; the
    // serializer and the inspector both read this one description.
    desc.properties.push_back(make_scalar<MeshRenderer3D, bool>("override_material", &MeshRenderer3D::override_material,
                                                                ValueKind::Bool, &defaults));
    desc.properties.push_back(make_group(
        "material", "Material", 0,
        {with_color(make_nested_vector_group<MeshRenderer3D, Material, Vec4>(
             "base_color", &MeshRenderer3D::material, &Material::base_color, "Vec4", {"r", "g", "b", "a"})),
         make_nested_scalar<MeshRenderer3D, Material, f32>("metallic", &MeshRenderer3D::material, &Material::metallic,
                                                           ValueKind::Float, &defaults),
         make_nested_scalar<MeshRenderer3D, Material, f32>("roughness", &MeshRenderer3D::material, &Material::roughness,
                                                           ValueKind::Float, &defaults),
         with_color(make_nested_vector_group<MeshRenderer3D, Material, Vec3>(
             "emissive", &MeshRenderer3D::material, &Material::emissive, "Vec3", {"r", "g", "b"})),
         make_nested_scalar<MeshRenderer3D, Material, bool>("double_sided", &MeshRenderer3D::material,
                                                            &Material::double_sided, ValueKind::Bool, &defaults),
         make_nested_key_ref<MeshRenderer3D, Material>("base_color_texture", &MeshRenderer3D::material,
                                                       &Material::base_color_texture, "Texture2D"),
         make_nested_key_ref<MeshRenderer3D, Material>("normal_texture", &MeshRenderer3D::material,
                                                       &Material::normal_texture, "Texture2D"),
         make_nested_key_ref<MeshRenderer3D, Material>("emissive_texture", &MeshRenderer3D::material,
                                                       &Material::emissive_texture, "Texture2D")}));

    registry.register_type(std::move(desc));
}

void register_directional_light3d(Registry& registry) {
    DirectionalLight3D defaults;

    TypeDesc desc;
    desc.name = DirectionalLight3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<DirectionalLight3D>()); };

    desc.properties.push_back(make_vector_group<DirectionalLight3D, Vec3>("direction", &DirectionalLight3D::direction,
                                                                          "Vec3", {"x", "y", "z"}));
    desc.properties.push_back(
        make_vector_group<DirectionalLight3D, Vec3>("color", &DirectionalLight3D::color, "Vec3", {"r", "g", "b"}));
    desc.properties.push_back(
        make_scalar<DirectionalLight3D, f32>("intensity", &DirectionalLight3D::intensity, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<DirectionalLight3D, f32>("ambient", &DirectionalLight3D::ambient, ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

void register_point_light3d(Registry& registry) {
    PointLight3D defaults;

    TypeDesc desc;
    desc.name = PointLight3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<PointLight3D>()); };

    desc.properties.push_back(
        make_vector_group<PointLight3D, Vec3>("color", &PointLight3D::color, "Vec3", {"r", "g", "b"}));
    desc.properties.push_back(
        make_scalar<PointLight3D, f32>("intensity", &PointLight3D::intensity, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<PointLight3D, f32>("range", &PointLight3D::range, ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

void register_spot_light3d(Registry& registry) {
    SpotLight3D defaults;

    TypeDesc desc;
    desc.name = SpotLight3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<SpotLight3D>()); };

    desc.properties.push_back(
        make_vector_group<SpotLight3D, Vec3>("color", &SpotLight3D::color, "Vec3", {"r", "g", "b"}));
    desc.properties.push_back(
        make_scalar<SpotLight3D, f32>("intensity", &SpotLight3D::intensity, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<SpotLight3D, f32>("range", &SpotLight3D::range, ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<SpotLight3D, f32>("inner_cone_degrees", &SpotLight3D::inner_cone_degrees,
                                                            ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<SpotLight3D, f32>("outer_cone_degrees", &SpotLight3D::outer_cone_degrees,
                                                            ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

} // namespace

void register_scene3d_types() {
    Registry& registry = Registry::instance();
    register_camera3d(registry);
    register_mesh_renderer3d(registry);
    register_directional_light3d(registry);
    register_point_light3d(registry);
    register_spot_light3d(registry);
}

} // namespace fe::reflect

namespace fe {

MeshRenderer3D::Surface MeshRenderer3D::resolve_surface_from(const std::optional<Material>* imported) const noexcept {
    Surface surface;

    if (override_material) {
        surface.material = material;
        surface.specular = material.specular_strength();
        surface.shininess = material.shininess();
        return surface;
    }

    if (imported != nullptr && imported->has_value()) {
        surface.material = imported->value();
        surface.specular = surface.material.specular_strength();
        surface.shininess = surface.material.shininess();
        return surface;
    }

    // The pre-material shape: a base colour, a specular strength, and an optional
    // texture. This is the path every scene authored before materials existed
    // takes, so it must produce the same draw it always did. Roughness is left at
    // its glTF default and the specular strength is taken from the authored
    // field, which is the control that always existed.
    surface.material.base_color = base_color;
    surface.specular = specular;
    surface.shininess = 32.0f;
    return surface;
}

MeshRenderer3D::Surface MeshRenderer3D::resolve_surface() const noexcept {
    // The imported material, when the mesh has one. Reading it off the mesh keeps
    // the glTF loader free of any dependency on the scene.
    const std::optional<Material>* imported = nullptr;
    if (const auto* typed = dynamic_cast<const Mesh*>(mesh.get()); typed != nullptr && typed->has_material()) {
        imported = &typed->material();
    }
    return resolve_surface_from(imported);
}

} // namespace fe
