// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "MiniAudioDevice.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

// miniaudio's implementation is compiled once, as C, by the miniaudio target
// fetched in FEDependencies.cmake; this translation unit only includes the
// declarations. That keeps the third-party code on its own warning settings and
// avoids the duplicate-symbol trap of defining MINIAUDIO_IMPLEMENTATION while
// also linking the library.
#include <miniaudio.h>

#if defined(FE_PLATFORM_WEB)
// EM_ASM, used below to resume the browser's audio context from the page.
#include <emscripten.h>
#endif

namespace fe::audio {
namespace {

// About half a second of stereo audio at 48 kHz. Large enough that a normal
// frame's submit fits comfortably and the audio thread rarely underruns, small
// enough that a stall cannot build up a long, audible delay.
constexpr usize kRingCapacityFrames = 24000;

// One "period" is the callback block size miniaudio asks for; 1024 frames is
// roughly 21 ms at 48 kHz, a common, safe default that is not so large as to add
// noticeable latency.
constexpr u32 kPeriodFrames = 1024;

void audio_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    (void)input;
    auto* self = static_cast<MiniAudioDevice*>(device->pUserData);
    if (self == nullptr) {
        std::memset(output, 0,
                    static_cast<usize>(frame_count) * static_cast<usize>(device->playback.channels) * sizeof(f32));
        return;
    }
    self->on_audio(static_cast<f32*>(output), frame_count);
}

// miniaudio's device-notification entry point. Only `unlocked` matters here: the
// web backend raises it once the browser has actually resumed the audio context,
// which is the authoritative end of the suspended period. The notification
// carries the device, so the engine object comes from its user-data pointer, the
// same way the audio callback finds it.
void audio_notification(const ma_device_notification* notification) {
    if (notification == nullptr || notification->pDevice == nullptr) {
        return;
    }
    auto* self = static_cast<MiniAudioDevice*>(notification->pDevice->pUserData);
    if (self == nullptr) {
        return;
    }
    if (notification->type == ma_device_notification_type_unlocked) {
        self->on_platform_unlocked();
    }
}

// Resumes the browser's audio context from the page. The context is created
// suspended (autoplay policy) and only a gesture may resume it; miniaudio keeps
// its WebAudio objects in a JS-side table keyed by the device index. The body
// avoids `===` on purpose: clang-format parses an EM_ASM body as C++ and would
// rewrite it into a syntax error (see AGENTS.md).
#if defined(FE_PLATFORM_WEB)
void resume_web_audio_context(int device_index) {
    EM_ASM(
        {
            var device = window.miniaudio.get_device_by_index($0);
            if (device != null && device.webaudio != null) {
                device.webaudio.resume();
            }
        },
        device_index);
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// FrameRingBuffer
// ---------------------------------------------------------------------------

FrameRingBuffer::FrameRingBuffer(usize capacity_frames, u32 channels)
    : capacity_frames_(capacity_frames == 0 ? 1 : capacity_frames), channels_(channels == 0 ? 1 : channels),
      buffer_(capacity_frames_ * channels_, 0.0f) {}

u32 FrameRingBuffer::write(const f32* frames, u32 frame_count) noexcept {
    if (frames == nullptr || frame_count == 0) {
        return 0;
    }
    const u64 write = write_index_.load(std::memory_order_relaxed);
    const u64 read = read_index_.load(std::memory_order_acquire);
    const u64 used = write - read;
    const u64 free_frames = capacity_frames_ > used ? capacity_frames_ - used : 0;
    const u32 to_write = static_cast<u32>(std::min<u64>(free_frames, frame_count));

    for (u32 i = 0; i < to_write; ++i) {
        const u64 slot = (write + i) % capacity_frames_;
        const f32* source = frames + static_cast<usize>(i) * channels_;
        std::copy(source, source + channels_, buffer_.begin() + static_cast<std::ptrdiff_t>(slot * channels_));
    }
    write_index_.store(write + to_write, std::memory_order_release);
    return to_write;
}

u32 FrameRingBuffer::read(f32* frames, u32 frame_count) noexcept {
    if (frames == nullptr || frame_count == 0) {
        return 0;
    }
    const u64 write = write_index_.load(std::memory_order_acquire);
    const u64 read = read_index_.load(std::memory_order_relaxed);
    const u64 used = write - read;
    const u32 to_read = static_cast<u32>(std::min<u64>(used, frame_count));

    for (u32 i = 0; i < to_read; ++i) {
        const u64 slot = (read + i) % capacity_frames_;
        const f32* source = buffer_.data() + static_cast<std::ptrdiff_t>(slot * channels_);
        std::copy(source, source + channels_, frames + static_cast<usize>(i) * channels_);
    }
    read_index_.store(read + to_read, std::memory_order_release);
    return to_read;
}

u32 FrameRingBuffer::available_to_read() const noexcept {
    return static_cast<u32>(write_index_.load(std::memory_order_acquire) - read_index_.load(std::memory_order_acquire));
}

void FrameRingBuffer::clear() noexcept {
    read_index_.store(write_index_.load(std::memory_order_acquire), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// MiniAudioDevice
// ---------------------------------------------------------------------------

MiniAudioDevice::MiniAudioDevice() = default;

MiniAudioDevice::~MiniAudioDevice() {
    stop();
    if (device_ != nullptr) {
        ma_device_uninit(device_.get());
    }
}

Result<AudioFormat> MiniAudioDevice::open(u32 sample_rate, u32 channels) {
    if (open_) {
        return Error("miniaudio device is already open");
    }
    if (sample_rate == 0 || channels == 0) {
        return Error("miniaudio device requires a non-zero sample rate and channel count");
    }

    device_ = std::make_unique<ma_device>();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = channels;
    config.sampleRate = sample_rate;
    config.periodSizeInFrames = kPeriodFrames;
    config.dataCallback = audio_callback;
    config.pUserData = this;
    // The web backend creates its audio context suspended and reports the
    // resume through this callback; on desktop it never fires and the handler is
    // never reached.
    config.notificationCallback = audio_notification;
    // Silence the output before the first submit rather than playing whatever
    // the buffer happened to contain.
    config.noPreSilencedOutputBuffer = MA_FALSE;

    const ma_result result = ma_device_init(nullptr, &config, device_.get());
    if (result != MA_SUCCESS) {
        device_.reset();
        return Error("miniaudio: failed to initialise the playback device");
    }

    // A backend may not honour the requested format exactly; report what it
    // actually opened with, and build the ring for that layout.
    format_ = AudioFormat{device_->sampleRate, device_->playback.channels};
    ring_ = std::make_unique<FrameRingBuffer>(kRingCapacityFrames, format_.channels);
    open_ = true;

    // On the web the context is created suspended, so a started device is silent
    // until a gesture. Detecting it here -- rather than assuming it from the
    // platform -- means a page that has already been interacted with starts
    // audible, and the flag is cleared by the resume notification.
#if defined(FE_PLATFORM_WEB)
    awaiting_gesture_.store(true, std::memory_order_release);
#endif
    return format_;
}

Error MiniAudioDevice::start() {
    if (!open_) {
        return Error("miniaudio device cannot start before open()");
    }
    if (ma_device_start(device_.get()) != MA_SUCCESS) {
        return Error("miniaudio: failed to start playback");
    }
    // `playing_` means "the engine has started the stream", not "sound is
    // audible". On the web the context may still be suspended; is_suspended()
    // reports that, and the two together are what a caller needs to tell a
    // gesture-blocked page from a broken device.
    playing_.store(true, std::memory_order_release);
    return Error::none();
}

void MiniAudioDevice::stop() noexcept {
    if (device_ != nullptr && open_) {
        playing_.store(false, std::memory_order_release);
        awaiting_gesture_.store(false, std::memory_order_release);
        ma_device_stop(device_.get());
    }
}

bool MiniAudioDevice::needs_user_gesture() const noexcept {
    return awaiting_gesture_.load(std::memory_order_acquire);
}

bool MiniAudioDevice::is_suspended() const noexcept {
    return awaiting_gesture_.load(std::memory_order_acquire) && playing_.load(std::memory_order_acquire);
}

bool MiniAudioDevice::resume_from_gesture() {
    if (!awaiting_gesture_.load(std::memory_order_acquire)) {
        return true; // already audible, or nothing to resume
    }
    if (!open_ || device_ == nullptr) {
        return false;
    }
#if defined(FE_PLATFORM_WEB)
    resume_web_audio_context(device_->webaudio.deviceIndex);
#endif
    // The context's resume() is asynchronous, so clear the flag now for the
    // caller's benefit and let the unlocked notification confirm it. Clearing it
    // here is what makes a second gesture a no-op rather than a second resume.
    awaiting_gesture_.store(false, std::memory_order_release);
    return true;
}

void MiniAudioDevice::on_platform_unlocked() noexcept {
    // The browser has resumed the context: sound is now audible. This is the
    // authoritative signal, so it clears the flag even if resume_from_gesture()
    // was never called (the page may have been interacted with before the device
    // was opened).
    awaiting_gesture_.store(false, std::memory_order_release);
}

u32 MiniAudioDevice::submit(const f32* frames, u32 frame_count) {
    if (!open_ || ring_ == nullptr) {
        return 0;
    }
    return ring_->write(frames, frame_count);
}

void MiniAudioDevice::on_audio(f32* output, u32 frame_count) noexcept {
    const usize sample_count = static_cast<usize>(frame_count) * format_.channels;
    const u32 read = ring_ != nullptr ? ring_->read(output, frame_count) : 0;

    // Zero whatever the ring could not supply. The playback buffer is not
    // guaranteed to be pre-silenced (a device may be configured otherwise), and
    // an underrun must be silence, not noise.
    if (read < frame_count) {
        std::fill(output + static_cast<usize>(read) * format_.channels, output + sample_count, 0.0f);
    }
}

} // namespace fe::audio
