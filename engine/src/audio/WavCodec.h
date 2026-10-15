// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

// The WAV codec primitives, shared by the whole-file loader (WavLoader) and the
// incremental stream (AudioStream). Internal to the audio module: it is not an
// installed header and nothing above audio includes it.
//
// Keeping the codec here - rather than inside WavLoader.cpp - is what lets a
// streamed file and a fully loaded clip decode through exactly the same code, so
// the two paths cannot drift apart. The tests assert that equality directly.

#include "fe/audio/AudioFormat.h"
#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

namespace fe::audio::wavcodec {

// WAV `fmt` tags: PCM (1), IEEE float (3), mu-law (6), A-law (7), Microsoft IMA
// ADPCM (0x11), and the extensible wrapper whose real tag lives in a sub-format
// GUID (0xFFFE).
inline constexpr u16 kWaveFormatPcm = 1;
inline constexpr u16 kWaveFormatIeeeFloat = 3;
inline constexpr u16 kWaveFormatMuLaw = 6;
inline constexpr u16 kWaveFormatALaw = 7;
inline constexpr u16 kWaveFormatImaAdpcm = 0x11;
inline constexpr u16 kWaveFormatExtensible = 0xFFFE;

[[nodiscard]] inline u16 read_u16(const u8* p) noexcept {
    return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
}

[[nodiscard]] inline u32 read_u32(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

[[nodiscard]] inline bool tag_is(const u8* p, const char* tag) noexcept {
    return std::memcmp(p, tag, 4) == 0;
}

// Converts one sample of `bytes`-wide integer PCM to float in [-1, 1). The
// divisor is 2^(bits-1): the most negative value maps to exactly -1 and the
// largest positive value stays just below +1, which is the standard symmetric
// mapping.
[[nodiscard]] inline f32 decode_integer_sample(const u8* p, u32 bytes) noexcept {
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

// G.711 mu-law: 8-bit companded samples. The decode is the ITU-T reference:
// complement the byte, split it into sign / exponent / mantissa, rebuild the
// magnitude, and remove the 0x84 bias. The result is a 16-bit-range value scaled
// to [-1, 1). Companding exists because a logarithmic quantisation gives a
// telephone signal a far better perceived quality at 8 bits than linear PCM.
[[nodiscard]] inline f32 decode_mulaw_sample(u8 byte) noexcept {
    const u8 u = static_cast<u8>(~byte);
    i32 magnitude = ((static_cast<i32>(u) & 0x0F) << 3) + 0x84;
    magnitude <<= (u & 0x70) >> 4;
    magnitude -= 0x84;
    const i32 value = (u & 0x80) != 0 ? -magnitude : magnitude;
    return static_cast<f32>(value) / 32768.0f;
}

// G.711 A-law: the same idea as mu-law with a different companding curve, used
// where A-law is the telephony standard. Decode is again the ITU-T reference.
[[nodiscard]] inline f32 decode_alaw_sample(u8 byte) noexcept {
    const u8 a = static_cast<u8>(byte ^ 0x55);
    i32 magnitude = (static_cast<i32>(a) & 0x0F) << 4;
    const i32 exponent = (a & 0x70) >> 4;
    if (exponent == 0) {
        magnitude += 8;
    } else if (exponent == 1) {
        magnitude += 0x108;
    } else {
        magnitude += 0x108;
        magnitude <<= exponent - 1;
    }
    const i32 value = (a & 0x80) != 0 ? magnitude : -magnitude;
    return static_cast<f32>(value) / 32768.0f;
}

// The standard IMA ADPCM step-size table (89 entries) and the per-nibble index
// adjustment. Together they are the whole codec: the nibble selects a fraction
// of the current step as the delta and nudges the step index, so the quantiser
// adapts to the signal's local slope.
inline constexpr std::array<i32, 89> kImaStepTable = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

inline constexpr std::array<i32, 16> kImaIndexTable = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// The adaptive state of one ADPCM channel. Reset from the block header at the
// start of every block, which is what makes a block independently decodable and
// a decode resumable at a block boundary.
struct ImaState {
    i32 predictor = 0;
    i32 index = 0;
};

// Expands one 4-bit nibble to the next 16-bit sample, advancing the state.
[[nodiscard]] inline i32 ima_expand_nibble(ImaState& state, u8 nibble) noexcept {
    const i32 step = kImaStepTable[static_cast<usize>(state.index)];
    i32 diff = step >> 3;
    if ((nibble & 1U) != 0U) {
        diff += step >> 2;
    }
    if ((nibble & 2U) != 0U) {
        diff += step >> 1;
    }
    if ((nibble & 4U) != 0U) {
        diff += step;
    }
    if ((nibble & 8U) != 0U) {
        diff = -diff;
    }
    state.predictor = std::clamp(state.predictor + diff, -32768, 32767);
    state.index = std::clamp(state.index + kImaIndexTable[nibble & 0x0FU], 0, 88);
    return state.predictor;
}

// The sample encodings the codec decodes. A tag/bit-depth outside this set is
// rejected rather than guessed at.
enum class Encoding { Pcm16, Pcm24, Float32, MuLaw, ALaw, ImaAdpcm, Unsupported };

// Bytes per interleaved sample for a fixed-width encoding (every encoding except
// IMA ADPCM, whose block layout is handled separately).
[[nodiscard]] inline u32 bytes_per_sample(Encoding encoding) noexcept {
    switch (encoding) {
    case Encoding::Pcm16:
        return 2;
    case Encoding::Pcm24:
        return 3;
    case Encoding::Float32:
        return 4;
    case Encoding::MuLaw:
    case Encoding::ALaw:
        return 1;
    default:
        return 0;
    }
}

[[nodiscard]] inline bool is_fixed_width(Encoding encoding) noexcept {
    return encoding != Encoding::ImaAdpcm && encoding != Encoding::Unsupported;
}

// The parts of a WAV file's header the decoders need: the format, the encoding,
// and the located `data` chunk. Produced once by parse_wav_header and reused by
// both the whole-file decode and the incremental stream.
struct WavHeader {
    AudioFormat format{};
    Encoding encoding = Encoding::Unsupported;
    u32 block_align = 0;
    usize data_offset = 0;
    u32 data_size = 0;
    u64 frame_count = 0;
};

// The number of frames the `data` chunk holds for an encoding. For the
// fixed-width encodings it is size / frame_bytes; for IMA ADPCM it is the sum of
// each block's frame count, since only the last block may be short.
[[nodiscard]] inline u64 count_frames(Encoding encoding, u32 data_size, u32 channels, u32 block_align) noexcept {
    if (channels == 0) {
        return 0;
    }
    if (encoding == Encoding::ImaAdpcm) {
        const u32 header_bytes = 4u * channels;
        if (block_align <= header_bytes) {
            return 0;
        }
        u64 frames = 0;
        u32 offset = 0;
        while (offset < data_size) {
            const u32 block_size = std::min(block_align, data_size - offset);
            if (block_size <= header_bytes) {
                break;
            }
            frames += (block_size - header_bytes) * 2u / channels + 1u;
            offset += block_size;
        }
        return frames;
    }
    const u32 frame_bytes = bytes_per_sample(encoding) * channels;
    return frame_bytes == 0 ? 0 : data_size / frame_bytes;
}

// Parses the RIFF header and the `fmt ` and `data` chunks, returning everything
// the decoders need and the byte range of the `data` chunk. This does no sample
// decoding: a caller that wants the whole sound calls decode_data() next, and
// the incremental stream uses the byte range to decode on demand.
[[nodiscard]] inline Result<WavHeader> parse_wav_header(const u8* data, usize size) {
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
    bool have_data = false;
    u16 format_tag = 0;
    u32 channels = 0;
    u32 sample_rate = 0;
    u32 bits_per_sample = 0;
    u32 block_align = 0;
    usize data_offset = 0;
    u32 data_size = 0;

    usize offset = 12;
    while (offset + 8 <= end) {
        const u8* header = data + offset;
        const u32 chunk_size = read_u32(header + 4);
        const usize body_offset = offset + 8;
        if (body_offset + chunk_size > end) {
            return Error("wav: chunk extends past the end of the file");
        }

        if (tag_is(header, "fmt ")) {
            const u8* body = data + body_offset;
            if (chunk_size < 16) {
                return Error("wav: 'fmt ' chunk is too small");
            }
            format_tag = read_u16(body);
            channels = read_u16(body + 2);
            sample_rate = read_u32(body + 4);
            block_align = read_u16(body + 12);
            bits_per_sample = read_u16(body + 14);
            if (format_tag == kWaveFormatExtensible) {
                if (chunk_size < 40) {
                    return Error("wav: extensible 'fmt ' chunk is truncated");
                }
                format_tag = read_u16(body + 24); // first two bytes of the sub-format GUID
            }
            if (channels == 0) {
                return Error("wav: 'fmt ' chunk declares zero channels");
            }
            if (sample_rate == 0) {
                return Error("wav: 'fmt ' chunk declares a zero sample rate");
            }
            have_fmt = true;
        } else if (tag_is(header, "data")) {
            if (!have_fmt) {
                return Error("wav: 'data' chunk appears before 'fmt '");
            }
            data_offset = body_offset;
            data_size = chunk_size;
            have_data = true;
            // A WAV may carry several data chunks; the first is the sound.
            break;
        }

        // Chunks are word-aligned: an odd body is followed by one pad byte.
        offset = body_offset + chunk_size + (chunk_size & 1u);
    }

    if (!have_data) {
        return Error("wav: no 'data' chunk found");
    }

    Encoding encoding = Encoding::Unsupported;
    switch (format_tag) {
    case kWaveFormatPcm:
        if (bits_per_sample == 16) {
            encoding = Encoding::Pcm16;
        } else if (bits_per_sample == 24) {
            encoding = Encoding::Pcm24;
        }
        break;
    case kWaveFormatIeeeFloat:
        if (bits_per_sample == 32) {
            encoding = Encoding::Float32;
        }
        break;
    case kWaveFormatMuLaw:
        encoding = Encoding::MuLaw;
        break;
    case kWaveFormatALaw:
        encoding = Encoding::ALaw;
        break;
    case kWaveFormatImaAdpcm:
        encoding = Encoding::ImaAdpcm;
        break;
    default:
        break;
    }
    if (encoding == Encoding::Unsupported) {
        return Error("wav: unsupported format (16/24-bit PCM, 32-bit float, mu-law, A-law, and IMA ADPCM are "
                     "decoded)");
    }
    if (is_fixed_width(encoding)) {
        const u32 frame_bytes = bytes_per_sample(encoding) * channels;
        if (frame_bytes == 0 || data_size % frame_bytes != 0) {
            return Error("wav: 'data' chunk size is not a whole number of frames");
        }
    } else if (block_align <= 4u * channels) {
        return Error("wav: IMA ADPCM block is too small for its channel headers");
    }

    WavHeader out;
    out.format = AudioFormat{sample_rate, channels};
    out.encoding = encoding;
    out.block_align = block_align;
    out.data_offset = data_offset;
    out.data_size = data_size;
    out.frame_count = count_frames(encoding, data_size, channels, block_align);
    return out;
}

// Decodes one IMA ADPCM block (`block_size` bytes, at most `block_align`) into
// `out`, appending `channels`-interleaved frames. The block is decoded from its
// own header, so blocks are independent and any block can be decoded in
// isolation - which is what makes the stream's seek-to-block resume exact.
inline void decode_ima_block(const u8* block, u32 block_size, u32 channels, std::vector<f32>& out) {
    const u32 header_bytes = 4u * channels;
    if (block_size <= header_bytes || channels == 0) {
        return;
    }

    std::vector<ImaState> states(channels);
    for (u32 ch = 0; ch < channels; ++ch) {
        const u8* header = block + ch * 4u;
        states[ch].predictor = static_cast<i16>(read_u16(header));
        states[ch].index = std::clamp(static_cast<i32>(header[2]), 0, 88);
    }

    const u32 data_bytes = block_size - header_bytes;
    const u32 frames_in_block = (data_bytes * 2u) / channels + 1u;

    std::vector<std::vector<f32>> channel_samples(channels);
    for (u32 ch = 0; ch < channels; ++ch) {
        channel_samples[ch].reserve(frames_in_block);
        // The first frame of a block is the header's predictor, not a nibble.
        channel_samples[ch].push_back(static_cast<f32>(states[ch].predictor) / 32768.0f);
    }

    // The sample data is 4-bit nibbles in 4-byte groups per channel, low nibble
    // of each byte first. A short final block simply has fewer groups.
    for (u32 group = 0; group < data_bytes; group += 4u * channels) {
        for (u32 ch = 0; ch < channels; ++ch) {
            for (u32 b = 0; b < 4u; ++b) {
                const u32 byte_index = group + ch * 4u + b;
                if (byte_index >= data_bytes) {
                    break;
                }
                const u8 byte = block[header_bytes + byte_index];
                if (channel_samples[ch].size() < frames_in_block) {
                    channel_samples[ch].push_back(static_cast<f32>(ima_expand_nibble(states[ch], byte & 0x0FU)) /
                                                  32768.0f);
                }
                if (channel_samples[ch].size() < frames_in_block) {
                    channel_samples[ch].push_back(static_cast<f32>(ima_expand_nibble(states[ch], (byte >> 4) & 0x0FU)) /
                                                  32768.0f);
                }
            }
        }
    }

    for (u32 frame = 0; frame < frames_in_block; ++frame) {
        for (u32 ch = 0; ch < channels; ++ch) {
            // A short final block can leave a channel one frame shy; pad with
            // silence rather than dropping the frame entirely.
            out.push_back(frame < channel_samples[ch].size() ? channel_samples[ch][frame] : 0.0f);
        }
    }
}

// Decodes `[start_frame, start_frame + frame_count)` interleaved frames of the
// `data` chunk into `out`, appending to it. Works for every encoding: the
// fixed-width formats index directly, and IMA ADPCM decodes the blocks that
// cover the range (skipping whole blocks before the start and trimming the
// frames after the end), so a partial read is exact.
inline void decode_data_range(const u8* data, const WavHeader& header, u64 start_frame, u64 frame_count,
                              std::vector<f32>& out) {
    const u32 channels = header.format.channels;
    if (channels == 0 || frame_count == 0) {
        return;
    }
    const u8* chunk = data + header.data_offset;

    if (is_fixed_width(header.encoding)) {
        const u32 bytes = bytes_per_sample(header.encoding);
        const u8* cursor = chunk + start_frame * bytes * channels;
        for (u64 i = 0; i < frame_count; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                switch (header.encoding) {
                case Encoding::Float32: {
                    f32 value = 0.0f;
                    std::memcpy(&value, cursor, sizeof(f32));
                    out.push_back(value);
                    break;
                }
                case Encoding::MuLaw:
                    out.push_back(decode_mulaw_sample(*cursor));
                    break;
                case Encoding::ALaw:
                    out.push_back(decode_alaw_sample(*cursor));
                    break;
                default:
                    out.push_back(decode_integer_sample(cursor, bytes));
                    break;
                }
                cursor += bytes;
            }
        }
        return;
    }

    // IMA ADPCM: walk blocks, decoding only those that intersect the range.
    const u32 header_bytes = 4u * channels;
    u64 frame_base = 0;
    u32 offset = 0;
    std::vector<f32> block_samples;
    while (offset < header.data_size && frame_base < start_frame + frame_count) {
        const u32 block_size = std::min(header.block_align, header.data_size - offset);
        if (block_size <= header_bytes) {
            break;
        }
        const u64 frames_in_block = (block_size - header_bytes) * 2u / channels + 1u;
        const u64 block_end = frame_base + frames_in_block;
        if (block_end > start_frame) {
            block_samples.clear();
            block_samples.reserve(static_cast<usize>(frames_in_block) * channels);
            decode_ima_block(chunk + offset, block_size, channels, block_samples);
            const u64 from = start_frame > frame_base ? start_frame - frame_base : 0;
            const u64 to = std::min(frames_in_block, start_frame + frame_count - frame_base);
            for (u64 f = from; f < to; ++f) {
                for (u32 c = 0; c < channels; ++c) {
                    out.push_back(block_samples[static_cast<usize>(f) * channels + c]);
                }
            }
        }
        frame_base = block_end;
        offset += block_size;
    }
}

// Decodes the whole `data` chunk. This is what the whole-file loader uses; it is
// decode_data_range over the entire frame count, so the two share one code path.
[[nodiscard]] inline Result<std::vector<f32>> decode_data(const u8* data, const WavHeader& header) {
    std::vector<f32> samples;
    samples.reserve(static_cast<usize>(header.frame_count) * header.format.channels);
    decode_data_range(data, header, 0, header.frame_count, samples);
    if (samples.empty() && header.frame_count != 0) {
        return Error("wav: 'data' chunk decoded to no frames");
    }
    return samples;
}

} // namespace fe::audio::wavcodec
