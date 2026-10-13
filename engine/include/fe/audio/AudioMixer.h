// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioClip.h"
#include "fe/core/Types.h"

#include <vector>

namespace fe::audio {

// One currently playing instance of a clip.
//
// A voice is the runtime counterpart of an AudioSource: the source says "this
// node wants this clip at this gain and pitch", and the system creates a voice
// to track the read position as it plays. Separating them means the authored
// component stays serializable while the fast-changing playback state lives
// only for as long as the sound is audible.
struct Voice {
    const AudioClip* clip = nullptr;

    // Read position in the clip, in frames, as a fraction so a pitch that is not
    // a whole-number ratio still advances smoothly. Advanced by the mixer.
    f64 position = 0.0;

    f32 gain = 1.0f;
    f32 pitch = 1.0f;
    bool loop = false;
    bool playing = true;

    // Set by the mixer when a non-looping voice passes the end of its clip. A
    // finished voice contributes nothing on later calls.
    bool finished = false;

    // Total output frames this voice has produced. Exposed for diagnostics and
    // used by the system to decide when a paused source should be removed.
    u64 frames_played = 0;

    [[nodiscard]] bool active() const noexcept { return clip != nullptr && playing && !finished; }
};

// The output-channel sample for `frame` of `clip`, up-mixing mono to every
// channel. Channels beyond a multi-channel clip wrap, which is a well-defined
// answer for a layout mismatch rather than an out-of-range read.
[[nodiscard]] f32 clip_sample(const AudioClip& clip, u64 frame, u32 out_channel) noexcept;

// Mixes `voice_count` voices into `output` and advances their read positions.
//
// This is the whole of the mixing maths, kept as a pure function: it touches no
// device, no scene, and no resource manager, so it can be called directly by a
// test with hand-built voices and asserted sample by sample. `AudioSystem` is a
// thin driver around it: it turns a delta time into a frame count, calls this,
// and submits the result to a device.
//
// `output` holds `frames * out_channels` interleaved samples and is *cleared*
// first, then each voice's contribution is added. Every voice reads its clip at
// a rate of
//
//     ratio = (clip.sample_rate / output_sample_rate) * pitch
//
// clip frames per output frame, which is what makes pitch a resampling ratio and
// not just a speed change. A looping voice wraps at the end of its clip; a
// one-shot voice is marked finished once past the end and contributes nothing
// afterwards.
//
// Returns the peak absolute sample written, so a caller can report or meter the
// output without rescanning it.
[[nodiscard]] f32 mix_voices(Voice* voices, usize voice_count, f32* output, u32 frames, u32 out_channels,
                             u32 output_sample_rate);

} // namespace fe::audio
