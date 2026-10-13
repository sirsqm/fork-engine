// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/audio/AudioClip.h"
#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/Resource.h"

#include <string>
#include <string_view>

namespace fe::audio {

// The decoded contents of a WAV file, before it becomes a Resource.
//
// This is what the parser returns. Keeping it a plain value (not an AudioClip,
// which needs a path and a Resource base) is what lets the parser be tested
// without touching the filesystem or the resource manager.
struct DecodedAudio {
    AudioFormat format{};
    std::vector<f32> samples{};

    [[nodiscard]] u64 frame_count() const noexcept {
        return format.channels == 0 ? 0 : static_cast<u64>(samples.size()) / format.channels;
    }
};

// Decodes 16-bit and 24-bit PCM WAV files.
//
// The parser is exposed separately from load() so it can be called on an
// in-memory buffer: a test generates a tiny RIFF chunk and checks the decode
// without writing a file, and a future importer can decode an embedded asset.
// `load` is the thin ResourceLoader wrapper the ResourceManager uses under the
// "wav" scheme.
//
// Scope: PCM (`fmt` tag 1) and IEEE float (`fmt` tag 3) with 16 or 24 bit
// integer samples, or 32-bit float samples. More bit depths and compressed
// codecs are later work; a file outside this set returns an Error rather than
// guessing.
class WavLoader final : public ResourceLoader {
public:
    // Handles the "wav" scheme, keeping decoded clips distinct from the raw
    // file, exactly as "tex" distinguishes a GPU texture from a CPU image.
    [[nodiscard]] std::string_view scheme() const noexcept override { return "wav"; }

    [[nodiscard]] bool supports(std::string_view extension) const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<Resource>> load(std::string_view path) override;

    // Parses a complete WAV file held in memory. Returns an Error for a
    // truncated, malformed, or unsupported file: never a partial clip and never
    // a crash. This is the function the unit tests drive directly.
    [[nodiscard]] static Result<DecodedAudio> parse(const u8* data, usize size);

    // Returns the cache key a reference should use to resolve to a decoded
    // clip, e.g. "assets/beep.wav" -> "wav:assets/beep.wav". Mirrors
    // Texture2DLoader::reference_key, so the serialized reference is a stable
    // manager key that survives a scene reload.
    [[nodiscard]] static std::string reference_key(std::string_view file_path);
};

} // namespace fe::audio
