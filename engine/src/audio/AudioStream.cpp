// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/AudioStream.h"

#include "WavCodec.h"

#include "fe/core/FileSystem.h"

#include <string>
#include <utility>

namespace fe::audio {

AudioStream::~AudioStream() = default;

// The stream's private state: the file bytes plus the parsed header, held behind
// a pimpl so the internal codec types never appear in the public header.
struct WavStream::Impl {
    std::vector<u8> bytes;
    wavcodec::WavHeader header{};
    u64 position = 0;
};

WavStream::~WavStream() = default;

Result<std::unique_ptr<WavStream>> WavStream::from_memory(std::vector<u8> bytes) {
    auto header = wavcodec::parse_wav_header(bytes.data(), bytes.size());
    if (!header) {
        return header.error();
    }

    std::unique_ptr<WavStream> stream(new WavStream());
    stream->impl_ = std::make_unique<Impl>();
    stream->impl_->header = header.value();
    stream->impl_->bytes = std::move(bytes);
    return stream;
}

Result<std::unique_ptr<WavStream>> WavStream::open(std::string_view path) {
    // The file's bytes are read once. A WAV header has to be walked and a
    // compressed block codec needs its block intact, so the compressed bytes are
    // the natural resident unit; what is *not* materialised is the decoded PCM,
    // which is what read() produces on demand.
    auto bytes = fs::read_binary_file(path);
    if (!bytes) {
        return Error(std::string(bytes.error().message()) + " ('" + std::string(path) + "')");
    }
    return from_memory(std::move(bytes.value()));
}

AudioFormat WavStream::format() const noexcept {
    return impl_ != nullptr ? impl_->header.format : AudioFormat{};
}

u64 WavStream::frame_count() const noexcept {
    return impl_ != nullptr ? impl_->header.frame_count : 0;
}

u64 WavStream::position() const noexcept {
    return impl_ != nullptr ? impl_->position : 0;
}

bool WavStream::at_end() const noexcept {
    return impl_ == nullptr || impl_->position >= impl_->header.frame_count;
}

u32 WavStream::read(u32 frame_count, std::vector<f32>& out) {
    out.clear();
    if (impl_ == nullptr || frame_count == 0) {
        return 0;
    }

    const u64 remaining = impl_->header.frame_count - impl_->position;
    const u64 wanted = std::min<u64>(frame_count, remaining);
    if (wanted == 0) {
        return 0;
    }

    // Decode in bounded slices, appending to `out`, so a large request never
    // decodes more than `block_frames_` at a time and the transient buffer stays
    // small. The result is identical to one big decode because the range decode
    // is exact and contiguous.
    u64 produced = 0;
    while (produced < wanted) {
        const u64 slice = std::min<u64>(block_frames_, wanted - produced);
        wavcodec::decode_data_range(impl_->bytes.data(), impl_->header, impl_->position + produced, slice, out);
        produced += slice;
    }

    impl_->position += wanted;
    return static_cast<u32>(wanted);
}

Error WavStream::seek(u64 frame) {
    if (impl_ == nullptr) {
        return Error("wav stream: not open");
    }
    impl_->position = std::min(frame, impl_->header.frame_count);
    return Error::none();
}

} // namespace fe::audio
