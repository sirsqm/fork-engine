// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioFormat.h"
#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string_view>
#include <vector>

namespace fe::audio {

// An output device: a stream of interleaved float frames at a fixed sample rate
// that the engine pushes mixed audio into.
//
// The interface is deliberately small and push-oriented. The device knows how
// to open, start, stop, and accept mixed frames; it does not know about voices,
// clips, or resources. This is the same trick the RHI plays for graphics: the
// consumer above the seam (the mixer) speaks one language and each backend
// translates.
//
// Two implementations exist:
//
//   * `MiniAudioDevice` - the real backend, which opens the platform's audio
//     output through miniaudio and drains a lock-free queue on the audio thread.
//   * `NullAudioDevice` - a deterministic sink with no sound card, used by the
//     unit tests and CI. It is a test double, not a shipping backend: it records
//     submitted frames instead of playing them, so the mix path is assertable
//     with no audio hardware.
//
// The engine opens devices for stereo output, but the interface does not require
// it: the channel count travels in AudioFormat. Mono sources are up-mixed by the
// mixer, not by the device.
class AudioDevice {
public:
    virtual ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // A short, stable name for logs and diagnostics ("miniaudio", "null").
    [[nodiscard]] virtual std::string_view backend() const noexcept = 0;

    // Opens the stream at the requested sample rate and channel count. Returns
    // the format the device actually opened with: a backend may not honour the
    // request exactly, and the caller must use the reported format rather than
    // assume the requested one. Opening twice is an error.
    [[nodiscard]] virtual Result<AudioFormat> open(u32 sample_rate, u32 channels) = 0;

    // Starts and stops playback. A device opens stopped so the caller can
    // configure everything before the first frame reaches the speakers.
    [[nodiscard]] virtual Error start() = 0;
    virtual void stop() noexcept = 0;

    // Queues `frame_count` interleaved frames for playback. `frames` holds
    // `frame_count * format().channels` samples. Returns how many frames were
    // accepted; a backend under back-pressure may accept fewer than requested,
    // and the caller may retry the remainder.
    [[nodiscard]] virtual u32 submit(const f32* frames, u32 frame_count) = 0;

    // The format the device was opened with. Only meaningful once open() has
    // succeeded.
    [[nodiscard]] virtual AudioFormat format() const noexcept = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;
    [[nodiscard]] virtual bool is_playing() const noexcept = 0;

protected:
    AudioDevice() = default;
};

// An AudioDevice with no sound card behind it.
//
// This is the deterministic sink the tests and CI run against. Because the
// interface is push-oriented, "playing" is simply recording: submitted frames
// are counted and their peak tracked, and the most recent block is retained so a
// test can inspect exactly what the mixer produced. It is not a placeholder for
// the real backend: it is a complete implementation of the same interface,
// documented as a test double, and it is what makes the mix path testable with
// no audio hardware.
class NullAudioDevice final : public AudioDevice {
public:
    NullAudioDevice() = default;

    [[nodiscard]] std::string_view backend() const noexcept override { return "null"; }

    [[nodiscard]] Result<AudioFormat> open(u32 sample_rate, u32 channels) override;
    [[nodiscard]] Error start() override;
    void stop() noexcept override;

    [[nodiscard]] u32 submit(const f32* frames, u32 frame_count) override;

    [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] bool is_playing() const noexcept override { return playing_; }

    // Total frames accepted since open(). A test's machine-checkable proof that
    // the mixer produced output.
    [[nodiscard]] u64 frames_submitted() const noexcept { return frames_submitted_; }

    // The peak absolute sample across every frame submitted so far, so a test
    // can assert that non-silent audio was produced without scanning every
    // sample.
    [[nodiscard]] f32 peak_sample() const noexcept { return peak_sample_; }

    // The interleaved samples of the most recent submit() call.
    [[nodiscard]] const std::vector<f32>& last_block() const noexcept { return last_block_; }

    void reset() noexcept;

private:
    AudioFormat format_{};
    bool open_ = false;
    bool playing_ = false;
    u64 frames_submitted_ = 0;
    f32 peak_sample_ = 0.0f;
    std::vector<f32> last_block_;
};

} // namespace fe::audio
