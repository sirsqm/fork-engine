// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioTypes.h"

#include "fe/audio/AudioSource.h"
#include "fe/audio/WavLoader.h"
#include "fe/reflect/PropertyBuilders.h"
#include "fe/reflect/Registry.h"

#include <memory>
#include <string>

// The audio component registrations, in the same style as PhysicsTypes.cpp: each
// block describes a component's properties once, and the serializer and the
// inspector read those descriptions rather than naming the type. This is what
// makes an AudioSource serialize and appear in the inspector with no per-type
// code in either.

namespace fe::reflect {
namespace {

void register_audio_source(Registry& registry) {
    audio::AudioSource defaults;

    TypeDesc desc;
    desc.name = audio::AudioSource::kTypeName;
    desc.factory = [] { return std::unique_ptr<Component>(std::make_unique<audio::AudioSource>()); };

    // The clip reference stores the manager cache key (the "wav:"-schemed path)
    // rather than the resource's own path, so a reload resolves the decoded
    // clip through the ordinary ResourceManager path - exactly how Sprite2D
    // stores a texture key.
    desc.properties.push_back(make_resource_ref<audio::AudioSource, std::shared_ptr<Resource>>(
        "clip", &audio::AudioSource::clip, "AudioClip",
        [](const std::shared_ptr<Resource>& clip) -> std::string {
            return clip != nullptr ? audio::WavLoader::reference_key(clip->path()) : std::string{};
        },
        [](std::shared_ptr<Resource>& slot, std::shared_ptr<Resource> loaded) { slot = std::move(loaded); }));

    desc.properties.push_back(
        make_scalar<audio::AudioSource, f32>("gain", &audio::AudioSource::gain, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<audio::AudioSource, f32>("pitch", &audio::AudioSource::pitch, ValueKind::Float, &defaults));
    desc.properties.push_back(
        make_scalar<audio::AudioSource, bool>("loop", &audio::AudioSource::loop, ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<audio::AudioSource, bool>("play_on_start", &audio::AudioSource::play_on_start,
                                                                    ValueKind::Bool, &defaults));
    desc.properties.push_back(
        make_scalar<audio::AudioSource, bool>("spatial", &audio::AudioSource::spatial, ValueKind::Bool, &defaults));
    desc.properties.push_back(make_scalar<audio::AudioSource, f32>("min_distance", &audio::AudioSource::min_distance,
                                                                   ValueKind::Float, &defaults));
    desc.properties.push_back(make_scalar<audio::AudioSource, f32>("max_distance", &audio::AudioSource::max_distance,
                                                                   ValueKind::Float, &defaults));

    registry.register_type(std::move(desc));
}

} // namespace

void register_audio_types() {
    Registry& registry = Registry::instance();
    register_audio_source(registry);
}

} // namespace fe::reflect
