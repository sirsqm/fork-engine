// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

// The real output backend, built on miniaudio.
//
// This header is private to engine/src/audio: no public header includes
// miniaudio.h, so nothing above the audio module depends on the library. The
// device pulls from a lock-free single-producer/single-consumer ring buffer that
// AudioSystem::submit fills and the audio thread drains.

#include "fe/audio/AudioDevice.h"
#include "fe/core/Types.h"

#include <atomic>
#include <memory>
#include <vector>

struct ma_device;

namespace fe::audio {

// An interleaved-float ring buffer shared between the submitter (one thread) and
// the audio callback (another thread).
//
// It is intentionally lock-free and allocation-free after construction: the
// audio thread must never block or allocate. A single producer writes and a
// single consumer reads, which the atomic head/tail indices uphold. Frames are
// simply dropped when the buffer is full, which is the correct behaviour for
// audio - a brief underrun is better than a stall or a growing delay.
class FrameRingBuffer {
public:
    explicit FrameRingBuffer(usize capacity_frames, u32 channels);

    // Copies up to `frame_count` frames in. Returns the number actually written.
    u32 write(const f32* frames, u32 frame_count) noexcept;

    // Copies up to `frame_count` frames out. Missing frames are not filled here;
    // the caller zeros the remainder. Returns the number read.
    u32 read(f32* frames, u32 frame_count) noexcept;

    [[nodiscard]] u32 available_to_read() const noexcept;
    void clear() noexcept;

private:
    usize capacity_frames_;
    u32 channels_;
    std::vector<f32> buffer_;
    std::atomic<u64> write_index_{0}; // total frames ever written
    std::atomic<u64> read_index_{0};  // total frames ever read
};

class MiniAudioDevice final : public AudioDevice {
public:
    MiniAudioDevice();
    ~MiniAudioDevice() override;

    MiniAudioDevice(const MiniAudioDevice&) = delete;
    MiniAudioDevice& operator=(const MiniAudioDevice&) = delete;

    [[nodiscard]] std::string_view backend() const noexcept override { return "miniaudio"; }

    [[nodiscard]] Result<AudioFormat> open(u32 sample_rate, u32 channels) override;
    [[nodiscard]] Error start() override;
    void stop() noexcept override;

    // The browser will not let a page make sound until the user interacts. On
    // the web this device reports that state so the caller can prompt for the
    // gesture; on desktop the flag is never set and all three are inert.
    [[nodiscard]] bool needs_user_gesture() const noexcept override;
    [[nodiscard]] bool is_suspended() const noexcept override;
    [[nodiscard]] bool resume_from_gesture() override;

    [[nodiscard]] u32 submit(const f32* frames, u32 frame_count) override;

    [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] bool is_playing() const noexcept override { return playing_; }

    // The audio-thread entry point. public so the free callback can reach it.
    void on_audio(f32* output, u32 frame_count) noexcept;

    // Called from miniaudio's notification callback once the platform has
    // actually resumed the audio context. public for the same reason as
    // on_audio(): the free callback is not a member.
    void on_platform_unlocked() noexcept;

private:
    std::unique_ptr<ma_device> device_;
    std::unique_ptr<FrameRingBuffer> ring_;
    AudioFormat format_{};
    bool open_ = false;
    std::atomic<bool> playing_{false};
    // True while the platform is waiting for a gesture before it will play. Set
    // when the web context opens suspended, cleared by the resume path or by the
    // unlocked notification. Never set on desktop.
    std::atomic<bool> awaiting_gesture_{false};
};

} // namespace fe::audio
