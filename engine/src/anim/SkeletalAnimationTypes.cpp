// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/anim/SkeletalAnimationTypes.h"

#include "fe/anim/AnimationClip.h"
#include "fe/anim/Animator.h"
#include "fe/anim/Skeleton.h"
#include "fe/anim/SkinnedMesh.h"
#include "fe/anim/SkinnedMeshRenderer3D.h"
#include "fe/reflect/Property.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/Resource.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <memory>
#include <string>

// The skeletal-animation component registrations, in the same style as
// AnimationTypes.cpp and Scene3DTypes.cpp.
//
// The two components are registered here, and the two resources they reference
// (Skeleton, AnimationClip) are *not*: a resource is not a component, so it has
// no TypeDesc. A resource reference is stored as its loader key string and
// resolved through the ResourceManager on load, which is exactly how a mesh or a
// texture reference already works.
//
// The Animator's derived state -- the pose and the palette -- is deliberately not
// reflected. It is recomputed every frame by SkeletonAnimationSystem, so
// persisting it would let a saved scene carry a pose that disagrees with the clip
// that produced it. `time`, `speed`, and the loop mode are reflected because they
// *are* authored state: an editor scrubbing a timeline sets them and expects them
// to survive a save.

namespace fe::reflect {
namespace {

// The loop mode as an int, for the same reason TextLabel's `align` is: ValueKind
// has no enum kind, and the setter clamps to the valid range so a hand-edited
// scene cannot put the component in an out-of-range state.
Property make_loop_mode_property() {
    Property property;
    property.name = "loop";
    property.value_kind = ValueKind::Int;
    property.getter = [](const void* object) -> Value {
        return static_cast<i64>(static_cast<const anim::Animator*>(object)->loop);
    };
    property.setter = [](void* object, const Value& value) -> bool {
        const auto* as_int = std::get_if<i64>(&value);
        if (as_int == nullptr) {
            return false;
        }
        auto* animator = static_cast<anim::Animator*>(object);
        if (*as_int <= static_cast<i64>(anim::LoopMode::Once)) {
            animator->loop = anim::LoopMode::Once;
        } else if (*as_int >= static_cast<i64>(anim::LoopMode::PingPong)) {
            animator->loop = anim::LoopMode::PingPong;
        } else {
            animator->loop = anim::LoopMode::Loop;
        }
        return true;
    };
    property.default_value = static_cast<i64>(anim::LoopMode::Loop);
    return property;
}

// The reference key a clip or skeleton is stored under.
//
// A clip's key is "<path>#<clip name>", because one glTF file holds several
// clips and the reference has to name which one. A skeleton's key is its own
// path. Both are produced by the loader and stored verbatim here, so a reload
// resolves the same resource rather than a path that lost the clip name.
[[nodiscard]] std::string clip_key_of(const std::shared_ptr<Resource>& clip) {
    if (clip == nullptr) {
        return {};
    }
    return clip->path();
}

[[nodiscard]] std::string skeleton_key_of(const std::shared_ptr<Resource>& skeleton) {
    if (skeleton == nullptr) {
        return {};
    }
    return skeleton->path();
}

void register_animator(Registry& registry) {
    anim::Animator defaults;

    TypeDesc desc;
    desc.name = anim::Animator::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<anim::Animator>()); };

    desc.properties.push_back(make_resource_ref<anim::Animator, std::shared_ptr<Resource>>(
        "skeleton", &anim::Animator::skeleton, anim::Skeleton::kTypeName, skeleton_key_of,
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));
    desc.properties.push_back(make_resource_ref<anim::Animator, std::shared_ptr<Resource>>(
        "clip", &anim::Animator::clip, anim::AnimationClip::kTypeName, clip_key_of,
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));
    desc.properties.push_back(make_resource_ref<anim::Animator, std::shared_ptr<Resource>>(
        "blend_clip", &anim::Animator::blend_clip, anim::AnimationClip::kTypeName, clip_key_of,
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));
    desc.properties.push_back(
        make_scalar<anim::Animator, f32>("time", &anim::Animator::time, ValueKind::Float, &defaults));
    desc.properties.push_back(with_range(
        make_scalar<anim::Animator, f32>("blend_weight", &anim::Animator::blend_weight, ValueKind::Float, &defaults),
        0.0, 1.0));
    desc.properties.push_back(
        make_scalar<anim::Animator, f32>("speed", &anim::Animator::speed, ValueKind::Float, &defaults));
    desc.properties.push_back(make_loop_mode_property());
    desc.properties.push_back(
        make_scalar<anim::Animator, bool>("playing", &anim::Animator::playing, ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<anim::Animator, bool>("apply_root_motion", &anim::Animator::apply_root_motion,
                                                                ValueKind::Bool, &defaults));

    registry.register_type(std::move(desc));
}

// The surface members of `SkinnedMeshRenderer3D` live one level deeper than
// `MeshRenderer3D`'s do: the component holds a `MeshRenderer3D` named `surface`
// and the fields are `surface.material.base_color`, not `material.base_color`.
// The reflection builders nest one level (`make_nested_scalar` takes a
// component member and a field of it), which is enough for `MeshRenderer3D` but
// not for the extra hop here, so the accessors below are written out against the
// owning component. They are the same shape the builders produce, and they keep
// the two components' wire form identical, which is the property that matters:
// a scene file describing a static mesh's material describes a skinned one
// character for character.
template <typename FieldT>
[[nodiscard]] Property surface_scalar(std::string name, FieldT MeshRenderer3D::* field, ValueKind kind,
                                      const anim::SkinnedMeshRenderer3D& defaults) {
    Property property;
    property.name = std::move(name);
    property.value_kind = kind;
    property.getter = [field](const void* object) -> Value {
        return detail::to_value((static_cast<const anim::SkinnedMeshRenderer3D*>(object)->surface).*field);
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        FieldT out{};
        if (!detail::from_value(value, out)) {
            return false;
        }
        (static_cast<anim::SkinnedMeshRenderer3D*>(object)->surface).*field = out;
        return true;
    };
    property.default_value = property.getter(&defaults);
    return property;
}

template <typename FieldT>
[[nodiscard]] Property material_scalar(std::string name, FieldT Material::* field, ValueKind kind,
                                       const anim::SkinnedMeshRenderer3D& defaults) {
    Property property;
    property.name = std::move(name);
    property.value_kind = kind;
    property.getter = [field](const void* object) -> Value {
        return detail::to_value((static_cast<const anim::SkinnedMeshRenderer3D*>(object)->surface.material).*field);
    };
    property.setter = [field](void* object, const Value& value) -> bool {
        FieldT out{};
        if (!detail::from_value(value, out)) {
            return false;
        }
        (static_cast<anim::SkinnedMeshRenderer3D*>(object)->surface.material).*field = out;
        return true;
    };
    property.default_value = property.getter(&defaults);
    return property;
}

// A float group over a `surface` member of `SkinnedMeshRenderer3D`, matching
// `make_nested_vector_group` one level deeper.
template <typename VectorT>
[[nodiscard]] Property surface_vector_group(std::string name, VectorT MeshRenderer3D::* field, std::string type_name,
                                            std::vector<std::string> component_names) {
    std::vector<Property> members;
    members.reserve(component_names.size());
    for (u32 i = 0; i < component_names.size(); ++i) {
        Property member;
        member.name = component_names[i];
        member.value_kind = ValueKind::Float;
        member.getter = [field, i](const void* object) -> Value {
            const VectorT& value = (static_cast<const anim::SkinnedMeshRenderer3D*>(object)->surface).*field;
            return static_cast<f64>(reinterpret_cast<const f32*>(&value)[i]);
        };
        member.setter = [field, i](void* object, const Value& value) -> bool {
            f32 out = 0.0f;
            if (!detail::from_value(value, out)) {
                return false;
            }
            VectorT& target = (static_cast<anim::SkinnedMeshRenderer3D*>(object)->surface).*field;
            reinterpret_cast<f32*>(&target)[i] = out;
            return true;
        };
        members.push_back(std::move(member));
    }
    return make_group(std::move(name), std::move(type_name), static_cast<u32>(component_names.size()),
                      std::move(members));
}

template <typename VectorT>
[[nodiscard]] Property material_vector_group(std::string name, VectorT Material::* field, std::string type_name,
                                             std::vector<std::string> component_names) {
    std::vector<Property> members;
    members.reserve(component_names.size());
    for (u32 i = 0; i < component_names.size(); ++i) {
        Property member;
        member.name = component_names[i];
        member.value_kind = ValueKind::Float;
        member.getter = [field, i](const void* object) -> Value {
            const VectorT& value = (static_cast<const anim::SkinnedMeshRenderer3D*>(object)->surface.material).*field;
            return static_cast<f64>(reinterpret_cast<const f32*>(&value)[i]);
        };
        member.setter = [field, i](void* object, const Value& value) -> bool {
            f32 out = 0.0f;
            if (!detail::from_value(value, out)) {
                return false;
            }
            VectorT& target = (static_cast<anim::SkinnedMeshRenderer3D*>(object)->surface.material).*field;
            reinterpret_cast<f32*>(&target)[i] = out;
            return true;
        };
        members.push_back(std::move(member));
    }
    return make_group(std::move(name), std::move(type_name), static_cast<u32>(component_names.size()),
                      std::move(members));
}

// A key-valued texture reference on `surface.material`.
[[nodiscard]] Property material_texture_ref(std::string name, std::string Material::* field) {
    Property property;
    property.name = std::move(name);
    property.value_kind = ValueKind::ResourceRef;
    property.resource = ResourceHint{"Texture2D"};
    property.resource_ref = [field](const void* object) -> std::string {
        return (static_cast<const anim::SkinnedMeshRenderer3D*>(object)->surface.material).*field;
    };
    property.resolve_resource = [field](void* object, std::string_view key,
                                        ResourceManager& manager) -> std::shared_ptr<Resource> {
        (static_cast<anim::SkinnedMeshRenderer3D*>(object)->surface.material).*field = std::string(key);
        if (key.empty()) {
            return nullptr;
        }
        // Loaded so a missing map is reported; the value keeps only the key, the
        // same contract `make_nested_key_ref` documents.
        auto loaded = manager.load(key);
        return loaded ? loaded.value() : nullptr;
    };
    return property;
}

void register_skinned_mesh_renderer(Registry& registry) {
    anim::SkinnedMeshRenderer3D defaults;

    TypeDesc desc;
    desc.name = anim::SkinnedMeshRenderer3D::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<anim::SkinnedMeshRenderer3D>()); };

    desc.properties.push_back(make_resource_ref<anim::SkinnedMeshRenderer3D, std::shared_ptr<Resource>>(
        "mesh", &anim::SkinnedMeshRenderer3D::mesh, anim::SkinnedMesh::kTypeName,
        [](const std::shared_ptr<Resource>& mesh) -> std::string {
            return mesh != nullptr ? mesh->path() : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));

    // The surface, exposed exactly as MeshRenderer3D exposes its own: the
    // override flag, the authored fields, and the material group. The accessors
    // reach through `surface`, so the wire form is the same and the inspector
    // shows the same widgets. The precedence rule itself is not restated --
    // `resolve_surface()` calls the one `MeshRenderer3D` defines.
    desc.properties.push_back(
        surface_scalar("override_material", &MeshRenderer3D::override_material, ValueKind::Bool, defaults));
    desc.properties.push_back(with_color(
        surface_vector_group<Vec4>("base_color", &MeshRenderer3D::base_color, "Vec4", {"r", "g", "b", "a"})));
    desc.properties.push_back(surface_scalar("specular", &MeshRenderer3D::specular, ValueKind::Float, defaults));
    desc.properties.push_back(make_group(
        "material", "Material", 0,
        {with_color(material_vector_group<Vec4>("base_color", &Material::base_color, "Vec4", {"r", "g", "b", "a"})),
         material_scalar("metallic", &Material::metallic, ValueKind::Float, defaults),
         material_scalar("roughness", &Material::roughness, ValueKind::Float, defaults),
         with_color(material_vector_group<Vec3>("emissive", &Material::emissive, "Vec3", {"r", "g", "b"})),
         material_scalar("double_sided", &Material::double_sided, ValueKind::Bool, defaults),
         material_texture_ref("base_color_texture", &Material::base_color_texture),
         material_texture_ref("normal_texture", &Material::normal_texture),
         material_texture_ref("emissive_texture", &Material::emissive_texture)}));

    registry.register_type(std::move(desc));
}

} // namespace

void register_skeletal_animation_types() {
    Registry& registry = Registry::instance();
    register_animator(registry);
    register_skinned_mesh_renderer(registry);
}

} // namespace fe::reflect
