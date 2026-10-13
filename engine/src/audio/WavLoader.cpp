// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/audio/WavLoader.h"

#include <cstring>
#include <string>

namespace fe::audio {
namespace {

// WAV PCM (`fmt` tag 1), IEEE float (tag 3), and the extensible wrapper whose
// real format lives in a sub-format GUID (tag 0xFFFE).
constexpr u16 kWaveFormatPcm = 1;
constexpr u16 kWaveFormatIeeeFloat = 3;
constexpr u16 kWaveFormatExtensible = 0xFFFE;

[[nodiscard]] u16 read_u16(const u8* p) noexcept {
    return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
}

[[nodiscard]] u32 read_u32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

[[nodiscard]] bool tag_is(const u8* p, const char* tag) noexcept {
    return std::memcmp(p, tag, 4) == 0;
}

// Converts one sample of `bytes`-wide integer PCM to float in [-1, 1). The
// divisor is 2^(bits-1): the most negative value maps to exactly -1 and the
// largest positive value stays just below +1, which is the standard symmetric
// mapping.
[[nodiscard]] f32 decode_integer_sample(const u8* p, u32 bytes) noexcept {
    if (bytes == 2) {
        const auto value = static_cast<i32>(static_cast<i16>(read_u16(p)));
        return static_cast<f32>(value) / 32768.0f;
    }
    // 24-bit little-endian, sign-extended from bit 23.
    i32 value = static_cast<i32>(p[0]) | (static_cast<i32>(p[1]) << 8) | (static_cast<i32>(p[2]) << 16);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<i32>(0xFF000000);
    }
    return static_cast<f32>(value) / 8388608.0f;
}

struct FmtChunk {
    u16 format_tag = 0;
    u32 channels = 0;
    u32 sample_rate = 0;
    u32 bits_per_sample = 0;
    u32 block_align = 0;
};

// Reads a `fmt ` chunk body. Handles the extensible wrapper by resolving the
// sub-format GUID to its base tag.
[[nodiscard]] Result<FmtChunk> parse_fmt(const u8* body, u32 size) {
    if (size < 16) {
        return Error("wav: 'fmt ' chunk is too small");
    }

    FmtChunk fmt;
    fmt.format_tag = read_u16(body);
    fmt.channels = read_u16(body + 2);
    fmt.sample_rate = read_u32(body + 4);
    fmt.block_align = read_u16(body + 12);
    fmt.bits_per_sample = read_u16(body + 14);

    if (fmt.format_tag == kWaveFormatExtensible) {
        // The 16-byte GUID follows the standard 16-byte PCM fields, after a
        // 2-byte cbSize and a 2-byte valid-bits count.
        if (size < 40) {
            return Error("wav: extensible 'fmt ' chunk is truncated");
        }
        fmt.format_tag = read_u16(body + 24); // first two bytes of the sub-format GUID
    }

    if (fmt.channels == 0) {
        return Error("wav: 'fmt ' chunk declares zero channels");
    }
    if (fmt.sample_rate == 0) {
        return Error("wav: 'fmt ' chunk declares a zero sample rate");
    }
    return fmt;
}

// The bytes per interleaved sample implied by the declared format, or 0 when
// the combination is not supported.
[[nodiscard]] u32 bytes_per_sample(const FmtChunk& fmt) noexcept {
    if (fmt.format_tag == kWaveFormatPcm) {
        if (fmt.bits_per_sample == 16 || fmt.bits_per_sample == 24) {
            return fmt.bits_per_sample / 8;
        }
        return 0;
    }
    if (fmt.format_tag == kWaveFormatIeeeFloat && fmt.bits_per_sample == 32) {
        return 4;
    }
    return 0;
}

} // namespace

bool WavLoader::supports(std::string_view extension) const noexcept {
    return extension == ".wav";
}

Result<DecodedAudio> WavLoader::parse(const u8* data, usize size) {
    if (data == nullptr || size == 0) {
        return Error("wav: empty input");
    }
    // RIFF header (4) + size (4) + WAVE (4) is the smallest valid prefix.
    if (size < 12) {
        return Error("wav: file is too small to be a RIFF document");
    }
    if (!tag_is(data, "RIFF")) {
        return Error("wav: missing RIFF header");
    }
    const u32 riff_size = read_u32(data + 4);
    // The RIFF size counts everything after the size field. A declared size that
    // runs past the buffer is a truncated file, not something to read optimistically.
    if (static_cast<u64>(riff_size) + 8u > size) {
        return Error("wav: RIFF size runs past the end of the file");
    }
    const usize end = static_cast<usize>(riff_size) + 8u;
    if (!tag_is(data + 8, "WAVE")) {
        return Error("wav: RIFF document is not WAVE");
    }

    bool have_fmt = false;
    FmtChunk fmt;

    // Walk the chunks between the WAVE tag and the declared end.
    usize offset = 12;
    while (offset + 8 <= end) {
        const u8* header = data + offset;
        const u32 chunk_size = read_u32(header + 4);
        const usize body_offset = offset + 8;
        if (body_offset + chunk_size > end) {
            return Error("wav: chunk extends past the end of the file");
        }

        if (tag_is(header, "fmt ")) {
            auto parsed = parse_fmt(data + body_offset, chunk_size);
            if (!parsed) {
                return parsed.error();
            }
            fmt = parsed.value();
            have_fmt = true;
        } else if (tag_is(header, "data")) {
            if (!have_fmt) {
                return Error("wav: 'data' chunk appears before 'fmt '");
            }
            const u32 bytes = bytes_per_sample(fmt);
            if (bytes == 0) {
                return Error("wav: unsupported format (only 16/24-bit PCM and 32-bit float are decoded)");
            }
            const u32 frame_bytes = bytes * fmt.channels;
            if (chunk_size % frame_bytes != 0) {
                return Error("wav: 'data' chunk size is not a whole number of frames");
            }

            const u64 frame_count = chunk_size / frame_bytes;
            const u64 sample_count = frame_count * fmt.channels;
            std::vector<f32> samples;
            samples.reserve(static_cast<usize>(sample_count));

            const u8* cursor = data + body_offset;
            for (u64 i = 0; i < sample_count; ++i) {
                if (bytes == 4) {
                    // 32-bit float: reinterpret the bits (the format is defined
                    // to be IEEE 754, so a memcpy into f32 is exact).
                    f32 value = 0.0f;
                    std::memcpy(&value, cursor, sizeof(f32));
                    samples.push_back(value);
                } else {
                    samples.push_back(decode_integer_sample(cursor, bytes));
                }
                cursor += bytes;
            }

            DecodedAudio decoded;
            decoded.format = AudioFormat{fmt.sample_rate, fmt.channels};
            decoded.samples = std::move(samples);
            return decoded;
        }

        // Chunks are word-aligned: an odd body is followed by one pad byte.
        offset = body_offset + chunk_size + (chunk_size & 1u);
    }

    return Error("wav: no 'data' chunk found");
}

Result<std::shared_ptr<Resource>> WavLoader::load(std::string_view path) {
    auto bytes = read_bytes(path);
    if (!bytes) {
        return bytes.error();
    }

    auto decoded = parse(bytes.value().data(), bytes.value().size());
    if (!decoded) {
        return Error(std::string(decoded.error().message()) + " ('" + std::string(path) + "')");
    }

    auto clip =
        std::make_shared<AudioClip>(std::string(path), decoded.value().format, std::move(decoded.value().samples));
    return std::static_pointer_cast<Resource>(clip);
}

std::string WavLoader::reference_key(std::string_view file_path) {
    // An already-schemed path is returned unchanged so the helper is safe to
    // apply to either a bare file path or an existing reference. A colon at
    // index <= 1 is a Windows drive letter ("C:/..."), not a scheme, matching
    // the split ResourceManager::load performs.
    const std::size_t colon = file_path.find(':');
    if (colon != std::string_view::npos && colon > 1) {
        return std::string(file_path);
    }
    return "wav:" + std::string(file_path);
}

} // namespace fe::audio
