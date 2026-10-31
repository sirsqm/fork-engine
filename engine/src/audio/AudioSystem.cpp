// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioSystem.h"

#include "fe/audio/AudioClip.h"
#include "fe/platform/InputState.h"
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

bool AudioSystem::reconcile_stream(Entry& entry, const AudioSource& source) {
    // A stream is only meaningful when the source opts in and its clip resource
    // has a path a WAV stream can open. Everything else (no clip, an in-memory
    // clip with no file, streaming off) closes any open stream and lets the
    // ordinary clip voice handle it: if the stream cannot be opened the source
    // falls back to its already-loaded clip rather than going silent.
    const bool wants = source.stream && source.clip != nullptr && !source.clip->path().empty();
    if (!wants) {
        entry.stream.reset();
        entry.stream_path.clear();
        entry.stream_window.clear();
        return false;
    }

    // Reopen only when the underlying path changed, so the read cursor (kept in
    // the voice's position) survives a per-frame update. The stream's own format
    // is the authority on its rate and channels, exactly as a clip's is.
    const std::string& path = source.clip->path();
    if (entry.stream != nullptr && entry.stream_path == path) {
        return true;
    }
    auto opened = WavStream::open(path);
    if (!opened) {
        entry.stream.reset();
        entry.stream_path.clear();
        return false;
    }
    entry.stream = std::shared_ptr<AudioStream>(std::move(opened.value()));
    entry.stream_path = path;
    entry.stream_window.clear();
    return true;
}

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
    // A browser creates its audio context suspended and only a user gesture may
    // resume it, so a started device is silent until then. Resume on the first
    // gesture of the frame, which is the earliest the platform permits. The call
    // is idempotent and a no-op on a device that never needed a gesture, so this
    // costs nothing on desktop.
    if (device_ != nullptr && device_->is_suspended() && context.input != nullptr && context.input->any_pressed()) {
        // The result says whether the device became able to play; nothing here
        // needs it, and the next frame's submit() shows the effect anyway.
        static_cast<void>(device_->resume_from_gesture());
    }

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

        // Decide whether this source plays streamed or fully loaded, opening or
        // closing the stream to match. A streaming source is driven by the
        // stream branch in the mix below; the clip path is what a fully loaded
        // source uses.
        const bool streaming = reconcile_stream(entry, *source);

        // Apply the source's current settings every frame, so an inspector tweak
        // is audible on the next update with no extra plumbing.
        entry.voice.gain = source->gain;
        entry.voice.pitch = source->pitch;
        entry.voice.loop = source->loop;
        entry.voice.spatial = source->spatial;
        entry.voice.min_distance = source->min_distance;
        entry.voice.max_distance = source->max_distance;
        entry.voice.cone_inner_degrees = source->cone_inner_degrees;
        entry.voice.cone_outer_degrees = source->cone_outer_degrees;
        entry.voice.cone_outer_gain = source->cone_outer_gain;
        entry.voice.stream_channels = streaming ? entry.stream->format().channels : 0;
        // Position comes from the node's world transform, never a transform of
        // the source's own. Computed fresh each frame so a moving node attenuates
        // correctly.
        const Mat4 world = node.world_matrix();
        entry.voice.world_position = Vec3{world.m[12], world.m[13], world.m[14]};

        // The cone axis is the node's forward (-Z of its world transform), the
        // same convention Camera3D::forward() uses, so a source is aimed by
        // rotating its node. A source whose cone is omnidirectional (the default)
        // leaves this zero, which the mixer reads as "no cone", so the vector
        // maths is skipped entirely.
        const bool cone_active = source->spatial && source->cone_inner_degrees < 180.0f &&
                                 source->cone_outer_degrees > source->cone_inner_degrees;
        entry.voice.cone_direction =
            cone_active ? transform_direction(world, Vec3{0.0f, 0.0f, -1.0f}).normalized() : Vec3{};

        // Velocity, for the Doppler shift, derived from how far the node moved
        // since the previous frame. The first frame an entry exists has no
        // previous sample, so the velocity is zero and no shift is applied; a
        // later frame differences against the sample stored then. A non-positive
        // delta (a paused step) leaves the previous position in place and reports
        // no velocity, so a pause does not register as a huge jump.
        entry.voice.velocity = Vec3{};
        if (entry.has_previous_position && context.delta_time > 0.0f) {
            entry.voice.velocity = (entry.voice.world_position - entry.previous_position) / context.delta_time;
        }
        entry.previous_position = entry.voice.world_position;
        entry.has_previous_position = true;

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
        // is rewound and marked unfinished, so it audibly replays. A streaming
        // source rewinds its cursor the same way.
        if (source->play_requested) {
            source->play_requested = false;
            if (entry.voice.clip != nullptr) {
                entry.voice.position = 0.0;
                entry.voice.finished = false;
                entry.voice.playing = true;
                if (entry.stream != nullptr) {
                    static_cast<void>(entry.stream->seek(0));
                }
            }
        }

        entries_.push_back(std::move(entry));
    });

    const u32 frames = frames_for(context.delta_time, sample_rate());

    // Flatten the non-streaming voices into a contiguous array for the pure
    // mixer. A streaming voice is not in it: its samples are decoded on demand
    // rather than read from a clip, so it is mixed separately below. This is
    // once per frame over a handful of voices, so the copy back is not worth
    // avoiding with a strided view.
    std::vector<Voice> voices;
    voices.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        if (entry.stream == nullptr) {
            voices.push_back(entry.voice);
        }
    }

    if (frames > 0 && (!voices.empty() || !entries_.empty())) {
        MixParams params;
        params.listener_position = listener_position_;
        params.listener_forward = listener_forward_;
        params.listener_up = listener_up_;
        params.listener_velocity = listener_velocity_;
        params.speed_of_sound = speed_of_sound_;
        params.doppler_enabled = doppler_enabled_;
        params.buses = buses_.empty() ? nullptr : buses_.data();
        params.bus_count = static_cast<u32>(buses_.size());
        params.master_gain = master_gain_;

        mix_buffer_.resize(static_cast<usize>(frames) * channels_);
        last_peak_ =
            mix_voices(voices.data(), voices.size(), mix_buffer_.data(), frames, channels_, sample_rate(), params);

        // Streaming voices are added on top of the clip sum. Each decodes the
        // window its own rate needs (bounded by stream_block_frames_), then
        // resamples to the output rate with the same nearest-sample rule the
        // clip path uses, so the two are audibly consistent.
        for (Entry& entry : entries_) {
            if (entry.stream == nullptr || !entry.voice.playing || entry.voice.finished) {
                continue;
            }
            const AudioFormat stream_format = entry.stream->format();
            const f64 ratio = static_cast<f64>(stream_format.sample_rate) / static_cast<f64>(sample_rate());
            const u64 wanted = static_cast<u64>(static_cast<f64>(frames) * ratio) + 2u;
            const u32 request = static_cast<u32>(std::min<u64>(wanted, stream_block_frames_));

            entry.stream_window.clear();
            const u32 got = entry.stream->read(request, entry.stream_window);
            if (got == 0) {
                if (entry.voice.loop) {
                    static_cast<void>(entry.stream->seek(0));
                    continue;
                }
                entry.voice.finished = true;
                continue;
            }

            const f32 stream_peak = add_streamed_voice(entry.voice, entry.stream_window.data(), got, frames,
                                                       mix_buffer_.data(), channels_, ratio, params);
            last_peak_ = std::max(last_peak_, stream_peak);
            entry.voice.position = static_cast<f64>(entry.stream->position());
        }

        peak_max_ = std::max(peak_max_, last_peak_);
        mixed_frames_ += frames;

        if (device_ != nullptr && device_->is_playing()) {
            static_cast<void>(device_->submit(mix_buffer_.data(), frames));
        }
    } else {
        last_peak_ = 0.0f;
    }

    // Copy the advanced state (read positions, finished flags) back so the next
    // frame resumes where this one stopped. Streaming entries are matched by
    // pointer, since only the non-streaming ones were in `voices`.
    usize voice_index = 0;
    for (Entry& entry : entries_) {
        if (entry.stream == nullptr) {
            entry.voice = voices[voice_index++];
        }
    }
}

} // namespace fe::audio
