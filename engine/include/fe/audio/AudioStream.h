// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioFormat.h"
#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <memory>
#include <string_view>
#include <vector>

namespace fe::audio {

// Decodes a sound incrementally, a bounded block at a time, instead of
// materialising the whole thing in memory.
//
// The whole-file path (`WavLoader` -> `AudioClip`) is the right model for a
// short sound: decode once, keep the samples, play them many times. It is the
// wrong model for a long one - a ten-minute stereo stream is hundreds of
// megabytes resident - and it is the wrong model for a file so large it should
// never be fully resident at all. `AudioStream` is the second model: the caller
// asks for N frames, the stream decodes and returns just those, and the caller
// (the mixer, or a music system) advances a read cursor.
//
// The interface is pull-based and synchronous, the audio counterpart of
// `net::Transport`'s poll model: no callback runs on another thread, so the
// stream is deterministic and testable with no sound card, exactly as the rest
// of the module is. An implementation may read its file lazily; it never
// allocates for the whole sound.
//
// Both paths decode through the same codec (WavCodec.h), so a streamed sound and
// a loaded clip are bit-identical for the frames a stream returns. The tests
// assert that equality directly, which is what keeps the two from drifting.
class AudioStream {
public:
    virtual ~AudioStream();

    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    // The PCM layout of the decoded frames (sample rate and channel count).
    [[nodiscard]] virtual AudioFormat format() const noexcept = 0;

    // Total frames in the sound. Known from the header without decoding, so a
    // caller can size a seek bar or loop a whole file without reading it.
    [[nodiscard]] virtual u64 frame_count() const noexcept = 0;

    // The frame the next read() will return.
    [[nodiscard]] virtual u64 position() const noexcept = 0;

    // True once the cursor has reached frame_count().
    [[nodiscard]] virtual bool at_end() const noexcept = 0;

    // Decodes up to `frame_count` frames starting at the cursor into `out`
    // (cleared first), advancing the cursor by the frames actually produced.
    // Returns the number of frames written: fewer than requested at the end of
    // the sound, and 0 once the cursor is at the end. `out` is interleaved
    // float samples, `frames_returned * format().channels` of them.
    [[nodiscard]] virtual u32 read(u32 frame_count, std::vector<f32>& out) = 0;

    // Moves the cursor to `frame`, clamped to [0, frame_count()]. Seeking is
    // exact for every encoding: a fixed-width format indexes directly, and IMA
    // ADPCM decodes from the start of the block that contains the frame.
    [[nodiscard]] virtual Error seek(u64 frame) = 0;

protected:
    AudioStream() = default;
};

// An AudioStream over a WAV file read from disk.
//
// The file's bytes are read once (a WAV header has to be walked, and a compressed
// block codec needs its block intact), but the *decoded* samples are not
// materialised: read() decodes only the requested range, so the resident cost is
// the file bytes plus one block, not the decoded PCM. That is the difference that
// matters for a large file, and it is the property the tests check by asserting
// that a stream never holds more decoded frames than it was asked for.
//
// Decoding is per-block for ADPCM and direct-index for PCM, so a seek to a frame
// in the middle is exact without decoding everything before it.
class WavStream final : public AudioStream {
public:
    ~WavStream() override;

    // Opens `path` and parses its header. Returns an Error for a missing,
    // malformed, or unsupported file - never a half-open stream.
    [[nodiscard]] static Result<std::unique_ptr<WavStream>> open(std::string_view path);

    // Opens a stream over bytes already in memory, so a test (or a pack-backed
    // source) needs no file. The bytes are copied, so the caller may free them.
    [[nodiscard]] static Result<std::unique_ptr<WavStream>> from_memory(std::vector<u8> bytes);

    [[nodiscard]] AudioFormat format() const noexcept override;
    [[nodiscard]] u64 frame_count() const noexcept override;
    [[nodiscard]] u64 position() const noexcept override;
    [[nodiscard]] bool at_end() const noexcept override;

    [[nodiscard]] u32 read(u32 frame_count, std::vector<f32>& out) override;
    [[nodiscard]] Error seek(u64 frame) override;

    // The largest number of frames a single read() ever decodes at once, for a
    // test that wants to prove reads are bounded. Set by the caller; default 4096.
    void set_block_frames(u32 frames) noexcept { block_frames_ = frames == 0 ? 1 : frames; }

private:
    struct Impl;
    WavStream() = default;

    std::unique_ptr<Impl> impl_;
    u32 block_frames_ = 4096;
};

} // namespace fe::audio
