// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioBus.h"
#include "fe/audio/AudioDevice.h"
#include "fe/audio/AudioMixer.h"
#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioStream.h"
#include "fe/core/Math.h"
#include "fe/core/Types.h"
#include "fe/systems/System.h"

#include <memory>
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

    // The frame window a streaming source decodes per update, so a large
    // request never materialises the whole sound. The mixer still consumes
    // exactly the frames the frame's delta time needs; this bounds how much is
    // decoded at once when that request is larger than the window.
    void set_stream_block_frames(u32 frames) noexcept { stream_block_frames_ = frames == 0 ? 1 : frames; }
    [[nodiscard]] u32 stream_block_frames() const noexcept { return stream_block_frames_; }

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

    // Which way the listener faces, for stereo panning. A positional source to
    // the listener's right is louder in the right ear and one to the left louder
    // in the left ear (see pan_gain). The default is the zero vector, which turns
    // panning off: a scene that sets only a position - every 2D game - mixes
    // exactly as it did before panning existed, so this is opt-in.
    //
    // Set with the ear's forward and up axes; the right axis is their cross
    // product. A 3D camera supplies both from its world rotation
    // (`Camera3D::forward` and its up axis).
    void set_listener_orientation(const Vec3& forward, const Vec3& up) noexcept {
        listener_forward_ = forward;
        listener_up_ = up;
    }
    [[nodiscard]] const Vec3& listener_forward() const noexcept { return listener_forward_; }
    [[nodiscard]] const Vec3& listener_up() const noexcept { return listener_up_; }

    // Whether a facing has been set, so a caller can tell "panning off" from
    // "panned against the default". True once set_listener_orientation is given a
    // non-zero forward vector.
    [[nodiscard]] bool has_listener_orientation() const noexcept { return listener_forward_.length_squared() > 0.0f; }

    // The listener's linear velocity, for the Doppler shift. The zero vector
    // (the default) means the ear is still, so a moving source is the only thing
    // that shifts pitch. A caller that has the ear's velocity (a fast-moving
    // camera) sets it; a scene that never does hears the source's own motion
    // alone, which is the common case and the audible half of the effect.
    void set_listener_velocity(const Vec3& velocity) noexcept { listener_velocity_ = velocity; }
    [[nodiscard]] const Vec3& listener_velocity() const noexcept { return listener_velocity_; }

    // The speed of sound the Doppler shift is computed against, in world units
    // per second. The default (343) is the speed of sound in air for a
    // metre-unit scene. A scene that authors its own scale sets this to match.
    void set_speed_of_sound(f32 speed) noexcept { speed_of_sound_ = speed; }
    [[nodiscard]] f32 speed_of_sound() const noexcept { return speed_of_sound_; }

    // Whether the Doppler shift is applied at all. Off by default, so every
    // existing scene mixes byte for byte; a game that wants the effect opts in
    // once, and then every source that sets `doppler` is shifted.
    void set_doppler_enabled(bool enabled) noexcept { doppler_enabled_ = enabled; }
    [[nodiscard]] bool doppler_enabled() const noexcept { return doppler_enabled_; }

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

    // The largest absolute sample the mixer has ever produced. Unlike last_peak()
    // it is cumulative, so it is the robust "did this system ever make a sound"
    // signal: the final frame of a run is often silent (a one-shot has ended),
    // which would make a single-frame peak read as zero even though audio played.
    // A headless game run logs this as its proof that the mixer ran.
    [[nodiscard]] f32 peak_max() const noexcept { return peak_max_; }

    // Clears the cumulative diagnostics (mixed_frames and peak_max).
    void reset_statistics() noexcept;

private:
    // A voice plus the source it was created from, so the system can match them
    // across frames and update settings that changed. A streaming source also
    // carries the open stream, so the decoded window survives across frames and
    // the cursor advances with the mix.
    struct Entry {
        AudioSource* source = nullptr;
        Voice voice{};
        std::shared_ptr<AudioStream> stream{};
        std::string stream_path{};
        std::vector<f32> stream_window{}; // scratch: the block decoded this frame

        // The node's world position last frame, so this frame's velocity can be
        // derived from the delta. `has_previous_position` is false on the frame
        // the entry is created, when there is no previous sample to difference
        // against and the velocity is taken as zero.
        Vec3 previous_position{};
        bool has_previous_position = false;
    };

    // Rebuilds the entry's stream to match the source (opening or closing it as
    // the `stream` flag or the clip path changes). Returns false when a stream
    // was requested but could not be opened, so the source is treated as silent.
    bool reconcile_stream(Entry& entry, const AudioSource& source);

    AudioDevice* device_ = nullptr;
    u32 sample_rate_ = 44100;
    u32 channels_ = 2;
    u32 stream_block_frames_ = 4096;

    Vec3 listener_position_{};
    Vec3 listener_forward_{};
    Vec3 listener_up_{0.0f, 1.0f, 0.0f};
    Vec3 listener_velocity_{};
    f32 speed_of_sound_ = 343.0f;
    bool doppler_enabled_ = false;
    f32 master_gain_ = 1.0f;
    std::vector<AudioBus> buses_;

    std::vector<Entry> entries_;
    std::vector<f32> mix_buffer_; // scratch, reused to avoid per-frame allocation

    u64 mixed_frames_ = 0;
    f32 last_peak_ = 0.0f;
    f32 peak_max_ = 0.0f;
};

} // namespace audio
} // namespace fe
