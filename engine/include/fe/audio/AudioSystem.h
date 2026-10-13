// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioBus.h"
#include "fe/audio/AudioDevice.h"
#include "fe/audio/AudioMixer.h"
#include "fe/audio/AudioSource.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/systems/System.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

class Scene;

namespace audio {

// Plays the scene's AudioSources through an output device.
//
// One system owns one device and one mixer. Each frame it:
//
//   1. Reconciles the scene's AudioSources with its playing voices: a new source
//      gets a voice, a finished one-shot drops its voice, a source whose clip or
//      settings changed is updated in place.
//   2. Resolves each source's bus name to a bus index and reads the source's
//      world position, so a positional source attenuates and a routed source is
//      scaled by its group.
//   3. Converts `delta_time` into a frame count at the device's sample rate.
//   4. Calls the pure mixer to produce that many frames.
//   5. Submits the mixed block to the device.
//
// The mixer is a plain function (see AudioMixer.h), so this class holds no math
// of its own: it is the plumbing that connects a scene, a clock, and a bus table
// to a device. A test drives it with a NullAudioDevice and asserts on the frames
// submitted.
//
// Lifetime: the system is owned by a SceneSchedule. The device is owned by
// whoever created it (the Engine for the real backend); the system only borrows
// it. The device must outlive the system.
class AudioSystem final : public System {
public:
    // `device` may be null: the system then produces no output but still
    // reconciles voices, which keeps a scene loadable and inspectable with no
    // audio device present.
    explicit AudioSystem(AudioDevice* device = nullptr);

    [[nodiscard]] std::string_view name() const noexcept override { return "AudioSystem"; }

    void update(const FrameContext& context, Scene& scene) override;

    // --- Configuration -----------------------------------------------------
    void set_device(AudioDevice* device) noexcept { device_ = device; }
    [[nodiscard]] AudioDevice* device() const noexcept { return device_; }

    // The sample rate the mixer runs at when no device is attached (or before
    // one is opened). When a device is attached its reported rate is used
    // instead, so the two never disagree.
    void set_sample_rate(u32 sample_rate) noexcept { sample_rate_ = sample_rate; }
    [[nodiscard]] u32 sample_rate() const noexcept;

    // Output channel count. Stereo by default; the mixer up-mixes mono clips.
    void set_channels(u32 channels) noexcept { channels_ = channels == 0 ? 1 : channels; }
    [[nodiscard]] u32 channels() const noexcept { return channels_; }

    // --- Listener and master ----------------------------------------------
    // Where the listener is, for positional attenuation. A source's distance is
    // measured from here. Defaults to the origin, so a scene that never sets it
    // attenuates against (0,0,0).
    void set_listener_position(const Vec3& position) noexcept { listener_position_ = position; }
    [[nodiscard]] const Vec3& listener_position() const noexcept { return listener_position_; }

    // The gain of the master bus, applied to the summed output. 1 is unity.
    void set_master_gain(f32 gain) noexcept { master_gain_ = gain; }
    [[nodiscard]] f32 master_gain() const noexcept { return master_gain_; }

    // --- Buses -------------------------------------------------------------
    // Sets (creating it if needed) the gain of the named group bus. A source
    // whose bus_name matches this name is scaled by the gain. The empty name is
    // the master bus and is rejected here: use set_master_gain for it, so there
    // is one owner of the master value.
    void set_bus_gain(std::string_view name, f32 gain);

    // The gain of the named bus, or 1 when no such bus exists. The empty name
    // returns the master gain.
    [[nodiscard]] f32 bus_gain(std::string_view name) const;

    // Removes every group bus, returning the system to master-only mixing.
    void clear_buses();

    [[nodiscard]] usize bus_count() const noexcept { return buses_.size(); }

    // --- Diagnostics -------------------------------------------------------
    // Every voice the system is tracking, including finished one-shots that are
    // still attached to a source in the scene.
    [[nodiscard]] usize voice_count() const noexcept { return entries_.size(); }

    // Voices that still contribute to the mix. A one-shot drops out of this
    // count once it reaches the end of its clip, which is how the system "stops"
    // a finished voice.
    [[nodiscard]] usize playing_voice_count() const noexcept;

    [[nodiscard]] u64 mixed_frames() const noexcept { return mixed_frames_; }
    [[nodiscard]] f32 last_peak() const noexcept { return last_peak_; }

private:
    // A voice plus the source it was created from, so the system can match them
    // across frames and update settings that changed.
    struct Entry {
        AudioSource* source = nullptr;
        Voice voice{};
    };

    AudioDevice* device_ = nullptr;
    u32 sample_rate_ = 44100;
    u32 channels_ = 2;

    Vec3 listener_position_{};
    f32 master_gain_ = 1.0f;
    std::vector<AudioBus> buses_;

    std::vector<Entry> entries_;
    std::vector<f32> mix_buffer_; // scratch, reused to avoid per-frame allocation

    u64 mixed_frames_ = 0;
    f32 last_peak_ = 0.0f;
};

} // namespace audio
} // namespace fe
