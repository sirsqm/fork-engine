// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioSystem.h"

#include "fe/audio/AudioClip.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace fe::audio {
namespace {

// Rounds a duration in seconds to a whole number of frames. A non-positive
// duration yields zero frames, so a paused or zero-length step produces no
// output and does not advance any voice.
[[nodiscard]] u32 frames_for(f32 delta_time, u32 sample_rate) noexcept {
    if (delta_time <= 0.0f || sample_rate == 0) {
        return 0;
    }
    return static_cast<u32>(std::lround(static_cast<f64>(delta_time) * sample_rate));
}

} // namespace

AudioSystem::AudioSystem(AudioDevice* device) : device_(device) {}

u32 AudioSystem::sample_rate() const noexcept {
    if (device_ != nullptr && device_->is_open()) {
        return device_->format().sample_rate;
    }
    return sample_rate_;
}

usize AudioSystem::playing_voice_count() const noexcept {
    usize count = 0;
    for (const Entry& entry : entries_) {
        if (entry.voice.active()) {
            ++count;
        }
    }
    return count;
}

void AudioSystem::set_bus_gain(std::string_view name, f32 gain) {
    // The master bus is not a group; its gain is owned by set_master_gain so the
    // value has a single owner. Naming it here is a no-op rather than a second
    // place to set it.
    if (name.empty()) {
        return;
    }
    for (AudioBus& bus : buses_) {
        if (bus.name == name) {
            bus.gain = gain;
            return;
        }
    }
    buses_.push_back(AudioBus{std::string(name), gain});
}

f32 AudioSystem::bus_gain(std::string_view name) const {
    if (name.empty()) {
        return master_gain_;
    }
    for (const AudioBus& bus : buses_) {
        if (bus.name == name) {
            return bus.gain;
        }
    }
    return 1.0f;
}

void AudioSystem::clear_buses() {
    buses_.clear();
}

void AudioSystem::reset_statistics() noexcept {
    mixed_frames_ = 0;
    last_peak_ = 0.0f;
    peak_max_ = 0.0f;
}

void AudioSystem::update(const FrameContext& context, Scene& scene) {
    // Reconcile: keep an entry for each source still in the scene, adopting the
    // voice it had last frame; add an entry for a source seen for the first
    // time; drop an entry whose node left the scene. This is the same
    // gather/carry-state shape SpriteAnimationSystem uses, and it is what keeps
    // playback continuous across frames without the node owning a voice.
    std::vector<Entry> previous = std::move(entries_);
    entries_.clear();
    entries_.reserve(previous.size());

    scene.for_each_node([&](Node& node) {
        auto* source = node.get_component<AudioSource>();
        if (source == nullptr) {
            return;
        }

        Entry entry;
        entry.source = source;
        for (Entry& prior : previous) {
            if (prior.source == source) {
                entry = std::move(prior);
                break;
            }
        }

        auto* clip = dynamic_cast<AudioClip*>(source->clip.get());
        if (entry.voice.clip != clip) {
            // The clip was set or swapped: restart from the top, and honour
            // play_on_start for the new sound. A source created stopped (or with
            // no clip) stays silent until it is explicitly started.
            entry.voice = Voice{};
            entry.voice.clip = clip;
            entry.voice.playing = source->play_on_start && clip != nullptr;
        }

        // Apply the source's current settings every frame, so an inspector tweak
        // is audible on the next update with no extra plumbing.
        entry.voice.gain = source->gain;
        entry.voice.pitch = source->pitch;
        entry.voice.loop = source->loop;
        entry.voice.spatial = source->spatial;
        entry.voice.min_distance = source->min_distance;
        entry.voice.max_distance = source->max_distance;
        // Position comes from the node's world transform, never a transform of
        // the source's own. Computed fresh each frame so a moving node attenuates
        // correctly.
        const Mat4 world = node.world_matrix();
        entry.voice.world_position = Vec3{world.m[12], world.m[13], world.m[14]};
        entry.voice.bus = kNoBus;
        for (u32 i = 0; i < buses_.size(); ++i) {
            if (buses_[i].name == source->bus_name) {
                entry.voice.bus = i;
                break;
            }
        }
        if (entry.voice.clip == nullptr) {
            entry.voice.playing = false;
        }

        // An explicit play() restarts the source from the top and clears the
        // transient request. This is how a gameplay system fires a one-shot cue
        // again (a brick breaking) after the first shot already ended: the voice
        // is rewound and marked unfinished, so it audibly replays.
        if (source->play_requested) {
            source->play_requested = false;
            if (entry.voice.clip != nullptr) {
                entry.voice.position = 0.0;
                entry.voice.finished = false;
                entry.voice.playing = true;
            }
        }

        entries_.push_back(std::move(entry));
    });

    const u32 frames = frames_for(context.delta_time, sample_rate());

    // Flatten into a contiguous voice array for the pure mixer. This is once per
    // frame over a handful of voices, so the copy back is not worth avoiding
    // with a strided view.
    std::vector<Voice> voices;
    voices.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        voices.push_back(entry.voice);
    }

    if (frames > 0 && !voices.empty()) {
        MixParams params;
        params.listener_position = listener_position_;
        params.buses = buses_.empty() ? nullptr : buses_.data();
        params.bus_count = static_cast<u32>(buses_.size());
        params.master_gain = master_gain_;

        mix_buffer_.resize(static_cast<usize>(frames) * channels_);
        last_peak_ =
            mix_voices(voices.data(), voices.size(), mix_buffer_.data(), frames, channels_, sample_rate(), params);
        peak_max_ = std::max(peak_max_, last_peak_);
        mixed_frames_ += frames;

        if (device_ != nullptr && device_->is_playing()) {
            static_cast<void>(device_->submit(mix_buffer_.data(), frames));
        }
    } else {
        last_peak_ = 0.0f;
    }

    // Copy the advanced state (read positions, finished flags) back so the next
    // frame resumes where this one stopped.
    for (usize i = 0; i < entries_.size(); ++i) {
        entries_[i].voice = voices[i];
    }
}

} // namespace fe::audio
