// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/audio/AudioClip.h"
#include "fe/audio/AudioDevice.h"
#include "fe/audio/AudioMixer.h"
#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioStream.h"
#include "fe/audio/AudioSystem.h"
#include "fe/audio/AudioTypes.h"
#include "fe/audio/WavLoader.h"
#include "fe/core/FileSystem.h"
#include "fe/platform/InputState.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace fe;

namespace {

// --- WAV construction ------------------------------------------------------

// Builds a little-endian RIFF/WAVE document in memory from integer PCM values.
// The parser is fed the bytes directly, so no file is written for the decode
// tests.
std::vector<u8> make_wav(u16 format_tag, u16 channels, u32 sample_rate, u16 bits, const std::vector<i32>& values) {
    const u16 bytes_per_sample = static_cast<u16>(bits / 8);
    std::vector<u8> data;
    data.reserve(values.size() * bytes_per_sample);
    for (i32 value : values) {
        const u32 raw = static_cast<u32>(value);
        for (u16 b = 0; b < bytes_per_sample; ++b) {
            data.push_back(static_cast<u8>((raw >> (8 * b)) & 0xFF));
        }
    }

    std::vector<u8> out;
    const auto push_u16 = [&out](u16 x) {
        out.push_back(static_cast<u8>(x & 0xFF));
        out.push_back(static_cast<u8>((x >> 8) & 0xFF));
    };
    const auto push_u32 = [&out](u32 x) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>((x >> (8 * i)) & 0xFF));
        }
    };
    const auto push_tag = [&out](const char* tag) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>(tag[i]));
        }
    };

    const u32 data_size = static_cast<u32>(data.size());
    const u32 fmt_size = 16;
    const u32 riff_size = 4 + (8 + fmt_size) + (8 + data_size);

    push_tag("RIFF");
    push_u32(riff_size);
    push_tag("WAVE");
    push_tag("fmt ");
    push_u32(fmt_size);
    push_u16(format_tag);
    push_u16(channels);
    push_u32(sample_rate);
    push_u32(sample_rate * channels * bytes_per_sample);
    push_u16(static_cast<u16>(channels * bytes_per_sample));
    push_u16(bits);
    push_tag("data");
    push_u32(data_size);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// A mono clip for the mixer tests. Values are chosen to be exactly representable
// in f32 so the expected samples below can be compared for equality.
std::shared_ptr<audio::AudioClip> mono_clip(u32 sample_rate, std::vector<f32> samples) {
    return std::make_shared<audio::AudioClip>("test.wav", audio::AudioFormat{sample_rate, 1}, std::move(samples));
}

FrameContext step(f32 delta) {
    FrameContext context;
    context.delta_time = delta;
    context.elapsed_time = static_cast<f64>(delta);
    return context;
}

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_audio_test_" + name + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// WAV decoding
// ---------------------------------------------------------------------------

TEST_CASE("a 16-bit PCM wav decodes to the expected format and samples") {
    // 16-bit mono at 8 kHz: -1.0, -0.5, 0.0, 0.5 as signed 16-bit values.
    const auto bytes = make_wav(1, 1, 8000, 16, {-32768, -16384, 0, 16384});

    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    CHECK(decoded.value().format.sample_rate == 8000);
    CHECK(decoded.value().format.channels == 1);
    CHECK(decoded.value().frame_count() == 4);
    REQUIRE(decoded.value().samples.size() == 4);
    CHECK(decoded.value().samples[0] == doctest::Approx(-1.0f));
    CHECK(decoded.value().samples[1] == doctest::Approx(-0.5f));
    CHECK(decoded.value().samples[2] == doctest::Approx(0.0f));
    CHECK(decoded.value().samples[3] == doctest::Approx(0.5f));
}

TEST_CASE("a 24-bit PCM wav decodes with sign extension") {
    // 24-bit mono: -1.0, -0.5, 0.0, 0.5 as signed 24-bit values.
    const auto bytes = make_wav(1, 1, 44100, 24, {-8388608, -4194304, 0, 4194304});

    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    CHECK(decoded.value().format.sample_rate == 44100);
    CHECK(decoded.value().format.channels == 1);
    CHECK(decoded.value().frame_count() == 4);
    REQUIRE(decoded.value().samples.size() == 4);
    CHECK(decoded.value().samples[0] == doctest::Approx(-1.0f));
    CHECK(decoded.value().samples[1] == doctest::Approx(-0.5f));
    CHECK(decoded.value().samples[2] == doctest::Approx(0.0f));
    CHECK(decoded.value().samples[3] == doctest::Approx(0.5f));
}

TEST_CASE("a stereo wav keeps interleaved samples and reports the frame count") {
    // 16-bit stereo at 8 kHz: two frames, (left,right) = (0.5, -0.5) then (0.25, -0.25).
    const auto bytes = make_wav(1, 2, 8000, 16, {16384, -16384, 8192, -8192});

    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    CHECK(decoded.value().format.channels == 2);
    CHECK(decoded.value().frame_count() == 2);
    REQUIRE(decoded.value().samples.size() == 4);
    CHECK(decoded.value().samples[0] == doctest::Approx(0.5f));
    CHECK(decoded.value().samples[1] == doctest::Approx(-0.5f));
    CHECK(decoded.value().samples[2] == doctest::Approx(0.25f));
    CHECK(decoded.value().samples[3] == doctest::Approx(-0.25f));
}

TEST_CASE("a malformed or truncated wav returns an error, never garbage") {
    const auto good = make_wav(1, 1, 8000, 16, {-32768, -16384, 0, 16384});

    SUBCASE("empty input") {
        CHECK_FALSE(audio::WavLoader::parse(nullptr, 0));
    }

    SUBCASE("too small to be RIFF") {
        CHECK_FALSE(audio::WavLoader::parse(good.data(), 8));
    }

    SUBCASE("missing RIFF tag") {
        auto bad = good;
        bad[0] = 'X';
        CHECK_FALSE(audio::WavLoader::parse(bad.data(), bad.size()));
    }

    SUBCASE("RIFF size claims more than is present") {
        auto bad = good;
        // Enlarge the declared RIFF size without adding bytes.
        bad[4] = static_cast<u8>(bad[4] + 64);
        CHECK_FALSE(audio::WavLoader::parse(bad.data(), bad.size()));
    }

    SUBCASE("a chunk extends past the declared end") {
        auto bad = good;
        // The data chunk size field sits at the end of the header; inflate it so
        // the body would run past the file.
        const usize data_size_offset = bad.size() - 4 - 4 - 8;
        bad[data_size_offset] = 0xFF;
        bad[data_size_offset + 1] = 0xFF;
        CHECK_FALSE(audio::WavLoader::parse(bad.data(), bad.size()));
    }

    SUBCASE("no data chunk") {
        const auto header_only = make_wav(1, 1, 8000, 16, {});
        auto bad = header_only;
        bad.resize(bad.size() - 8); // drop the empty data chunk header
        CHECK_FALSE(audio::WavLoader::parse(bad.data(), bad.size()));
    }

    SUBCASE("unsupported bit depth") {
        const auto bad = make_wav(1, 1, 8000, 8, {0, 1, 2, 3});
        CHECK_FALSE(audio::WavLoader::parse(bad.data(), bad.size()));
    }
}

TEST_CASE("WavLoader::reference_key builds a stable manager key without double-prefixing") {
    CHECK(audio::WavLoader::reference_key("assets/beep.wav") == "wav:assets/beep.wav");
    CHECK(audio::WavLoader::reference_key("wav:assets/beep.wav") == "wav:assets/beep.wav");
    // A Windows drive letter is a path, not a scheme, so it still gets prefixed.
    CHECK(audio::WavLoader::reference_key("C:/assets/beep.wav") == "wav:C:/assets/beep.wav");
}

// ---------------------------------------------------------------------------
// The mixer (pure logic, no device)
// ---------------------------------------------------------------------------

TEST_CASE("one looping voice and one one-shot mix to the exact expected samples") {
    auto looping = mono_clip(8000, {0.5f, 0.25f, 0.125f, 0.0625f});
    auto one_shot = mono_clip(8000, {1.0f, 0.5f});

    audio::Voice voices[2];
    voices[0].clip = looping.get();
    voices[0].loop = true;
    voices[1].clip = one_shot.get();
    voices[1].loop = false;

    f32 output[6] = {};
    const f32 peak = audio::mix_voices(voices, 2, output, 6, 1, 8000);

    // Hand-computed frame by frame at a 1:1 rate:
    //   f0: 0.5  + 1.0   = 1.5
    //   f1: 0.25 + 0.5   = 0.75
    //   f2: 0.125 (one-shot finished) = 0.125
    //   f3: 0.0625 (looping wraps after this frame's read) = 0.0625
    //   f4: 0.5   (looping wrapped to the start)
    //   f5: 0.25
    CHECK(output[0] == doctest::Approx(1.5f));
    CHECK(output[1] == doctest::Approx(0.75f));
    CHECK(output[2] == doctest::Approx(0.125f));
    CHECK(output[3] == doctest::Approx(0.0625f));
    CHECK(output[4] == doctest::Approx(0.5f));
    CHECK(output[5] == doctest::Approx(0.25f));
    CHECK(peak == doctest::Approx(1.5f));

    CHECK_FALSE(voices[0].finished);
    CHECK(voices[0].frames_played == 6);
    CHECK(voices[1].finished);
    CHECK(voices[1].frames_played == 2);
}

TEST_CASE("gain scales a voice's contribution") {
    auto clip = mono_clip(8000, {0.5f, 0.25f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.gain = 0.5f;

    f32 output[2] = {};
    const f32 peak = audio::mix_voices(&voice, 1, output, 2, 1, 8000);

    CHECK(output[0] == doctest::Approx(0.25f));
    CHECK(output[1] == doctest::Approx(0.125f));
    CHECK(peak == doctest::Approx(0.25f));
}

TEST_CASE("pitch is applied as a sample-rate ratio") {
    auto clip = mono_clip(8000, {1.0f, 0.5f, 0.25f, 0.125f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.loop = true;
    voice.pitch = 2.0f; // an octave up: read every other frame

    f32 output[6] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 6, 1, 8000));

    CHECK(output[0] == doctest::Approx(1.0f));
    CHECK(output[1] == doctest::Approx(0.25f));
    CHECK(output[2] == doctest::Approx(1.0f));
    CHECK(output[3] == doctest::Approx(0.25f));
    CHECK(output[4] == doctest::Approx(1.0f));
    CHECK(output[5] == doctest::Approx(0.25f));
}

TEST_CASE("a clip is resampled when the output rate differs from the clip rate") {
    // A 4 kHz clip played at 8 kHz: each output frame consumes half a clip frame,
    // so every clip sample is heard twice.
    auto clip = mono_clip(4000, {1.0f, 0.5f, 0.25f, 0.125f});
    audio::Voice voice;
    voice.clip = clip.get();

    f32 output[8] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 8, 1, 8000));

    const f32 expected[8] = {1.0f, 1.0f, 0.5f, 0.5f, 0.25f, 0.25f, 0.125f, 0.125f};
    for (usize i = 0; i < 8; ++i) {
        CHECK(output[i] == doctest::Approx(expected[i]));
    }
}

TEST_CASE("a non-looping voice is marked finished and stops contributing") {
    auto clip = mono_clip(8000, {0.5f, 0.25f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.loop = false;

    f32 output[4] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 4, 1, 8000));

    CHECK(output[0] == doctest::Approx(0.5f));
    CHECK(output[1] == doctest::Approx(0.25f));
    CHECK(output[2] == doctest::Approx(0.0f));
    CHECK(output[3] == doctest::Approx(0.0f));
    CHECK(voice.finished);
    CHECK(voice.frames_played == 2);

    // A finished voice contributes nothing to a later mix.
    f32 later[2] = {1.0f, 1.0f};
    const f32 peak = audio::mix_voices(&voice, 1, later, 2, 1, 8000);
    CHECK(later[0] == doctest::Approx(0.0f));
    CHECK(later[1] == doctest::Approx(0.0f));
    CHECK(peak == doctest::Approx(0.0f));
    CHECK(voice.frames_played == 2);
}

TEST_CASE("a mono clip is up-mixed to every output channel") {
    auto clip = mono_clip(8000, {0.5f, 0.25f});
    audio::Voice voice;
    voice.clip = clip.get();

    f32 output[4] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 2, 2, 8000));

    CHECK(output[0] == doctest::Approx(0.5f));  // frame 0, left
    CHECK(output[1] == doctest::Approx(0.5f));  // frame 0, right
    CHECK(output[2] == doctest::Approx(0.25f)); // frame 1, left
    CHECK(output[3] == doctest::Approx(0.25f)); // frame 1, right
}

// ---------------------------------------------------------------------------
// Distance attenuation (pure)
// ---------------------------------------------------------------------------

TEST_CASE("the attenuation curve is 1 at min_distance and 0 at max_distance") {
    // Endpoints: full gain at and inside min_distance, silence at and beyond
    // max_distance. The numbers are the documented curve, not re-derived.
    CHECK(audio::distance_attenuation(0.0f, 1.0f, 20.0f) == doctest::Approx(1.0f));
    CHECK(audio::distance_attenuation(1.0f, 1.0f, 20.0f) == doctest::Approx(1.0f));
    CHECK(audio::distance_attenuation(20.0f, 1.0f, 20.0f) == doctest::Approx(0.0f));
    CHECK(audio::distance_attenuation(100.0f, 1.0f, 20.0f) == doctest::Approx(0.0f));

    // The interior follows (1/d - 1/max) / (1/min - 1/max). At d = 2 with
    // min = 1, max = 20: (0.5 - 0.05) / (1 - 0.05) = 0.45 / 0.95.
    CHECK(audio::distance_attenuation(2.0f, 1.0f, 20.0f) == doctest::Approx(0.45f / 0.95f));
    // At d = 4: (0.25 - 0.05) / 0.95 = 0.20 / 0.95.
    CHECK(audio::distance_attenuation(4.0f, 1.0f, 20.0f) == doctest::Approx(0.20f / 0.95f));
}

TEST_CASE("the attenuation curve is monotonically non-increasing with distance") {
    const f32 min_distance = 1.0f;
    const f32 max_distance = 20.0f;
    f32 previous = audio::distance_attenuation(0.0f, min_distance, max_distance);
    for (f32 d = 1.0f; d <= 20.0f; d += 1.0f) {
        const f32 gain = audio::distance_attenuation(d, min_distance, max_distance);
        CHECK(gain <= previous + 1e-6f);
        previous = gain;
    }
    // It really does fall in the interior rather than being flat.
    CHECK(audio::distance_attenuation(10.0f, min_distance, max_distance) <
          audio::distance_attenuation(2.0f, min_distance, max_distance));
}

TEST_CASE("a degenerate attenuation span is total, never NaN") {
    // min_distance 0 has no 1/d term; the curve must still be defined.
    CHECK(audio::distance_attenuation(0.0f, 0.0f, 20.0f) == doctest::Approx(1.0f));
    CHECK(audio::distance_attenuation(5.0f, 0.0f, 20.0f) == doctest::Approx(0.0f));

    // max_distance <= min_distance: no interior region, so the curve is a step
    // at min_distance - full gain inside it, silence at or beyond it.
    CHECK(audio::distance_attenuation(10.0f, 20.0f, 20.0f) == doctest::Approx(1.0f));
    CHECK(audio::distance_attenuation(25.0f, 20.0f, 20.0f) == doctest::Approx(0.0f));
    CHECK(audio::distance_attenuation(5.0f, 20.0f, 10.0f) == doctest::Approx(1.0f));
    CHECK(audio::distance_attenuation(15.0f, 20.0f, 10.0f) == doctest::Approx(1.0f)); // inside min
    CHECK(audio::distance_attenuation(21.0f, 20.0f, 10.0f) == doctest::Approx(0.0f)); // past min
}

TEST_CASE("a positional voice is scaled by the distance to the listener") {
    auto clip = mono_clip(8000, {1.0f, 1.0f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.spatial = true;
    voice.world_position = Vec3{2.0f, 0.0f, 0.0f};
    voice.min_distance = 1.0f;
    voice.max_distance = 20.0f;

    audio::MixParams params;
    params.listener_position = Vec3{0.0f, 0.0f, 0.0f};

    f32 output[2] = {};
    const f32 peak = audio::mix_voices(&voice, 1, output, 2, 1, 8000, params);

    // d = 2 -> 0.45 / 0.95. Both frames are the clip's 1.0 scaled by that.
    CHECK(output[0] == doctest::Approx(0.45f / 0.95f));
    CHECK(output[1] == doctest::Approx(0.45f / 0.95f));
    CHECK(peak == doctest::Approx(0.45f / 0.95f));
    CHECK(voice.attenuation == doctest::Approx(0.45f / 0.95f));
}

TEST_CASE("a positional voice at the listener is at full gain") {
    auto clip = mono_clip(8000, {1.0f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.spatial = true;
    voice.world_position = Vec3{5.0f, 5.0f, 5.0f};

    audio::MixParams params;
    params.listener_position = Vec3{5.0f, 5.0f, 5.0f}; // distance 0

    f32 output[1] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 1, 1, 8000, params));
    CHECK(output[0] == doctest::Approx(1.0f));
}

TEST_CASE("a non-positional voice ignores the listener and keeps full gain") {
    // Regression: the pre-refinement behaviour. A source with spatial = false
    // sounds exactly as it did before attenuation existed, even with a listener
    // set and a distant node position.
    auto clip = mono_clip(8000, {1.0f, 0.5f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.gain = 0.5f;
    voice.spatial = false;
    voice.world_position = Vec3{1000.0f, 0.0f, 0.0f};

    audio::MixParams params;
    params.listener_position = Vec3{0.0f, 0.0f, 0.0f};

    f32 output[2] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 2, 1, 8000, params));

    CHECK(output[0] == doctest::Approx(0.5f)); // 1.0 * 0.5, no attenuation
    CHECK(output[1] == doctest::Approx(0.25f));
    CHECK(voice.attenuation == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// Buses and the master (pure)
// ---------------------------------------------------------------------------

TEST_CASE("a bus gain scales every voice routed to it") {
    auto clip = mono_clip(8000, {1.0f});
    audio::Voice voices[2];
    voices[0].clip = clip.get();
    voices[0].bus = 0; // "sfx", gain 0.25
    voices[1].clip = clip.get();
    voices[1].bus = 1; // "music", gain 0.5

    const audio::AudioBus buses[2] = {
        audio::AudioBus{"sfx", 0.25f},
        audio::AudioBus{"music", 0.5f},
    };
    audio::MixParams params;
    params.buses = buses;
    params.bus_count = 2;

    f32 output[1] = {};
    static_cast<void>(audio::mix_voices(voices, 2, output, 1, 1, 8000, params));

    // Each voice is 1.0 scaled by its own bus: 0.25 + 0.5 = 0.75.
    CHECK(output[0] == doctest::Approx(0.75f));
}

TEST_CASE("the master bus scales the sum of the group buses") {
    auto clip = mono_clip(8000, {1.0f});
    audio::Voice voices[2];
    voices[0].clip = clip.get();
    voices[0].bus = 0; // gain 0.25
    voices[1].clip = clip.get();
    voices[1].bus = 1; // gain 0.5

    const audio::AudioBus buses[2] = {
        audio::AudioBus{"sfx", 0.25f},
        audio::AudioBus{"music", 0.5f},
    };
    audio::MixParams params;
    params.buses = buses;
    params.bus_count = 2;
    params.master_gain = 0.5f;

    f32 output[1] = {};
    static_cast<void>(audio::mix_voices(voices, 2, output, 1, 1, 8000, params));

    // (0.25 + 0.5) * 0.5 = 0.375.
    CHECK(output[0] == doctest::Approx(0.375f));
}

TEST_CASE("a voice on no bus is scaled by the master gain alone") {
    auto clip = mono_clip(8000, {1.0f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.bus = audio::kNoBus;

    // A non-empty bus table that does not contain this voice must not touch it.
    const audio::AudioBus buses[1] = {audio::AudioBus{"music", 0.5f}};
    audio::MixParams params;
    params.buses = buses;
    params.bus_count = 1;
    params.master_gain = 0.25f;

    f32 output[1] = {};
    static_cast<void>(audio::mix_voices(&voice, 1, output, 1, 1, 8000, params));
    CHECK(output[0] == doctest::Approx(0.25f)); // 1.0 * master 0.25
}

TEST_CASE("a muted bus is silent but still advances the voice") {
    auto clip = mono_clip(8000, {1.0f, 1.0f, 1.0f, 1.0f});
    audio::Voice voice;
    voice.clip = clip.get();
    voice.bus = 0;

    const audio::AudioBus buses[1] = {audio::AudioBus{"music", 0.0f}};
    audio::MixParams params;
    params.buses = buses;
    params.bus_count = 1;

    f32 output[2] = {};
    const f32 peak = audio::mix_voices(&voice, 1, output, 2, 1, 8000, params);
    CHECK(output[0] == doctest::Approx(0.0f));
    CHECK(output[1] == doctest::Approx(0.0f));
    CHECK(peak == doctest::Approx(0.0f));
    // The read position moved, so unmuting resumes where it was, not from zero.
    CHECK(voice.position == doctest::Approx(2.0));
    CHECK(voice.frames_played == 2);
}

// ---------------------------------------------------------------------------
// The null device (the contract under test; audio output is not in CI)
// ---------------------------------------------------------------------------

TEST_CASE("the null device records submitted frames and their peak") {
    audio::NullAudioDevice device;
    auto opened = device.open(8000, 1);
    REQUIRE(opened);
    CHECK(device.is_open());
    CHECK_FALSE(device.is_playing());

    // Nothing is accepted before start().
    const f32 block[2] = {0.25f, -0.5f};
    CHECK(device.submit(block, 2) == 0);

    REQUIRE_FALSE(device.start());
    CHECK(device.is_playing());
    CHECK(device.submit(block, 2) == 2);
    CHECK(device.frames_submitted() == 2);
    CHECK(device.peak_sample() == doctest::Approx(0.5f));
    REQUIRE(device.last_block().size() == 2);
    CHECK(device.last_block()[0] == doctest::Approx(0.25f));
    CHECK(device.last_block()[1] == doctest::Approx(-0.5f));

    device.stop();
    CHECK_FALSE(device.is_playing());
    CHECK(device.submit(block, 2) == 0);
}

TEST_CASE("opening the null device twice is an error") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(44100, 2));
    CHECK_FALSE(device.open(44100, 2));
    CHECK_FALSE(device.open(0, 2));
}

// ---------------------------------------------------------------------------
// The system: advancing voice time by delta and stopping finished voices
// ---------------------------------------------------------------------------

TEST_CASE("AudioSystem advances voices by delta time and stops a finished one-shot") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    CHECK(system.sample_rate() == 8000); // follows the device's reported rate

    Scene scene;
    Node* node = scene.create_node("Beep");
    auto* source = node->add_component<audio::AudioSource>();
    // 16 frames of full-scale DC: easy to assert a peak of exactly 1.0.
    source->clip = mono_clip(8000, std::vector<f32>(16, 1.0f));
    source->loop = false;
    source->play_on_start = true;

    // 1 ms at 8 kHz is exactly 8 frames.
    system.update(step(0.001f), scene);
    CHECK(system.mixed_frames() == 8);
    CHECK(system.voice_count() == 1);
    CHECK(system.playing_voice_count() == 1);
    CHECK(system.last_peak() == doctest::Approx(1.0f));
    CHECK(device.frames_submitted() == 8);
    REQUIRE(device.last_block().size() == 8);
    CHECK(device.last_block()[0] == doctest::Approx(1.0f));

    // A second 8 frames reaches frame 16 but does not pass the end yet.
    system.update(step(0.001f), scene);
    CHECK(system.mixed_frames() == 16);
    CHECK(system.playing_voice_count() == 1);
    CHECK(system.last_peak() == doctest::Approx(1.0f));
    CHECK(device.frames_submitted() == 16);

    // The third step crosses the end: the voice finishes and produces silence.
    system.update(step(0.001f), scene);
    CHECK(system.mixed_frames() == 24);
    CHECK(system.playing_voice_count() == 0);
    CHECK(system.last_peak() == doctest::Approx(0.0f));
    CHECK(device.frames_submitted() == 24);
}

TEST_CASE("a source with play_on_start false is silent until started") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Beep");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(16, 1.0f));
    source->play_on_start = false;

    system.update(step(0.001f), scene);
    CHECK(system.voice_count() == 1);
    CHECK(system.playing_voice_count() == 0);
    CHECK(system.last_peak() == doctest::Approx(0.0f));
}

TEST_CASE("a looping source keeps playing past the end of its clip") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Beep");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(4, 1.0f)); // 4 frames
    source->loop = true;

    // Three steps of 8 frames each wrap several times but never finish.
    for (int i = 0; i < 3; ++i) {
        system.update(step(0.001f), scene);
        CHECK(system.playing_voice_count() == 1);
        CHECK(system.last_peak() == doctest::Approx(1.0f));
    }
    CHECK(system.mixed_frames() == 24);
}

// ---------------------------------------------------------------------------
// The system: listener, bus routing, and the master gain (integration)
// ---------------------------------------------------------------------------

TEST_CASE("the AudioSystem attenuates a positional source by its distance to the listener") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Emitter");
    node->transform().position = Vec3{2.0f, 0.0f, 0.0f};
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(8, 1.0f));
    source->loop = true;
    source->spatial = true;
    source->min_distance = 1.0f;
    source->max_distance = 20.0f;

    system.set_listener_position(Vec3{0.0f, 0.0f, 0.0f});
    system.update(step(0.001f), scene);

    // 1 ms at 8 kHz is 8 frames, each a full-scale 1.0 scaled by 0.45/0.95.
    CHECK(system.last_peak() == doctest::Approx(0.45f / 0.95f));

    // Moving the listener onto the node restores full gain on the next update.
    system.set_listener_position(Vec3{2.0f, 0.0f, 0.0f});
    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(1.0f));
}

TEST_CASE("the AudioSystem routes a source to a named bus and its gain scales the mix") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Music");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(8, 1.0f));
    source->loop = true;
    source->bus_name = "music";

    // A bus that does not exist yet leaves the source at unity.
    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(1.0f));

    // Creating the bus with a lower gain scales the same source down.
    system.set_bus_gain("music", 0.25f);
    CHECK(system.bus_gain("music") == doctest::Approx(0.25f));
    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(0.25f));

    // An unrelated bus does not touch it.
    system.set_bus_gain("sfx", 0.5f);
    CHECK(system.bus_count() == 2);
    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(0.25f));

    // Muting the bus silences the source; clearing the table restores it.
    system.set_bus_gain("music", 0.0f);
    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(0.0f));
    system.clear_buses();
    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(1.0f));
}

TEST_CASE("the AudioSystem master gain scales the whole mix") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Music");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(8, 1.0f));
    source->loop = true;
    source->bus_name = "music";
    system.set_bus_gain("music", 0.5f);

    system.set_master_gain(0.5f);
    CHECK(system.master_gain() == doctest::Approx(0.5f));
    system.update(step(0.001f), scene);
    // bus 0.5 * master 0.5 = 0.25.
    CHECK(system.last_peak() == doctest::Approx(0.25f));

    // Setting the master through the bus API by the reserved empty name is a
    // no-op; the master keeps its value.
    system.set_bus_gain("", 0.1f);
    CHECK(system.bus_count() == 1);
    CHECK(system.bus_gain("") == doctest::Approx(0.5f));
}

TEST_CASE("a non-positional source through the AudioSystem is unchanged") {
    // Regression: the exact shape of the pre-refinement system tests, with a
    // listener and buses configured, still produces full gain because the source
    // is non-positional and on no bus.
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    system.set_listener_position(Vec3{100.0f, 100.0f, 0.0f});
    system.set_bus_gain("music", 0.25f);

    Scene scene;
    Node* node = scene.create_node("Beep");
    node->transform().position = Vec3{500.0f, 0.0f, 0.0f};
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(16, 1.0f));
    source->loop = false;
    source->play_on_start = true;

    system.update(step(0.001f), scene);
    CHECK(system.mixed_frames() == 8);
    CHECK(system.playing_voice_count() == 1);
    CHECK(system.last_peak() == doctest::Approx(1.0f));
    CHECK(device.frames_submitted() == 8);
}

// ---------------------------------------------------------------------------
// Reflection and serialization
// ---------------------------------------------------------------------------

TEST_CASE("register_audio_types makes AudioSource available to the registry") {
    reflect::register_audio_types();

    const reflect::Registry& registry = reflect::Registry::instance();
    REQUIRE(registry.contains(audio::AudioSource::kTypeName));

    const reflect::TypeDesc* desc = registry.find(audio::AudioSource::kTypeName);
    REQUIRE(desc != nullptr);
    CHECK(desc->name == "AudioSource");
    CHECK_FALSE(desc->properties.empty());

    auto instance = registry.default_construct(audio::AudioSource::kTypeName);
    REQUIRE(instance != nullptr);
    CHECK(instance->type_name() == std::string("AudioSource"));
}

TEST_CASE("an AudioSource round-trips through the serializer") {
    reflect::register_audio_types();

    Scene scene;
    Node* node = scene.create_node("Music");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = std::make_shared<audio::AudioClip>("assets/beep.wav", audio::AudioFormat{8000, 1},
                                                      std::vector<f32>{1.0f, 0.5f});
    source->gain = 0.25f;
    source->pitch = 1.5f;
    source->loop = true;
    source->spatial = true;
    source->min_distance = 2.0f;
    source->max_distance = 40.0f;
    source->bus_name = "music";

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);
    CHECK(json.value().find("\"AudioSource\"") != std::string::npos);
    CHECK(json.value().find("\"wav:assets/beep.wav\"") != std::string::npos);
    CHECK(json.value().find("\"music\"") != std::string::npos);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));
    Node* loaded_node = loaded.find_node_by_name("Music");
    REQUIRE(loaded_node != nullptr);
    auto* loaded_source = loaded_node->get_component<audio::AudioSource>();
    REQUIRE(loaded_source != nullptr);
    CHECK(loaded_source->gain == doctest::Approx(0.25f));
    CHECK(loaded_source->pitch == doctest::Approx(1.5f));
    CHECK(loaded_source->loop);
    CHECK(loaded_source->spatial);
    CHECK(loaded_source->min_distance == doctest::Approx(2.0f));
    CHECK(loaded_source->max_distance == doctest::Approx(40.0f));
    CHECK(loaded_source->bus_name == "music");
}

TEST_CASE("an AudioSource's clip reference resolves through the resource manager on load") {
    reflect::register_audio_types();

    TempDir dir("resolve");
    const std::string path = dir.file("beep.wav");
    const auto bytes = make_wav(1, 1, 8000, 16, {16384, -16384});
    REQUIRE_FALSE(fs::write_binary_file(path, bytes.data(), bytes.size()));

    ResourceManager resources;
    resources.register_loader(std::make_unique<audio::WavLoader>());

    Scene scene;
    Node* node = scene.create_node("Beep");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = std::make_shared<audio::AudioClip>(path, audio::AudioFormat{8000, 1}, std::vector<f32>{0.5f, -0.5f});

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded, &resources));
    Node* loaded_node = loaded.find_node_by_name("Beep");
    REQUIRE(loaded_node != nullptr);
    auto* loaded_source = loaded_node->get_component<audio::AudioSource>();
    REQUIRE(loaded_source != nullptr);
    REQUIRE(loaded_source->clip != nullptr);
    CHECK(loaded_source->clip->type_name() == std::string("AudioClip"));

    auto* loaded_clip = dynamic_cast<audio::AudioClip*>(loaded_source->clip.get());
    REQUIRE(loaded_clip != nullptr);
    CHECK(loaded_clip->frame_count() == 2);
    CHECK(loaded_clip->channels() == 1);
}

TEST_CASE("WavLoader loads a decoded AudioClip through the resource manager") {
    TempDir dir("load");
    const std::string path = dir.file("tone.wav");
    const auto bytes = make_wav(1, 1, 8000, 16, {-32768, 0, 16384});
    REQUIRE_FALSE(fs::write_binary_file(path, bytes.data(), bytes.size()));

    ResourceManager resources;
    resources.register_loader(std::make_unique<audio::WavLoader>());

    const std::string key = audio::WavLoader::reference_key(path);
    auto loaded = resources.load_as<audio::AudioClip>(key);
    REQUIRE(loaded);
    CHECK(loaded.value()->frame_count() == 3);
    CHECK(loaded.value()->sample_rate() == 8000);
    CHECK(loaded.value()->samples()[0] == doctest::Approx(-1.0f));
    CHECK(loaded.value()->samples()[2] == doctest::Approx(0.5f));

    // A missing file is an error, not a crash.
    CHECK_FALSE(resources.load(audio::WavLoader::reference_key(dir.file("absent.wav"))));
}

// ---------------------------------------------------------------------------
// Compressed codecs (mu-law, A-law, IMA ADPCM)
// ---------------------------------------------------------------------------

namespace {

// Builds a WAV with an explicit block_align, so a compressed format (IMA ADPCM)
// whose block layout is not derivable from the bit depth can be constructed. The
// existing make_wav computes block_align from the bit depth, which is right for
// linear PCM and wrong for a block codec.
std::vector<u8> make_wav_raw(u16 format_tag, u16 channels, u32 sample_rate, u16 bits, u16 block_align,
                             const std::vector<u8>& data) {
    std::vector<u8> out;
    const auto push_u16 = [&out](u16 x) {
        out.push_back(static_cast<u8>(x & 0xFF));
        out.push_back(static_cast<u8>((x >> 8) & 0xFF));
    };
    const auto push_u32 = [&out](u32 x) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>((x >> (8 * i)) & 0xFF));
        }
    };
    const auto push_tag = [&out](const char* tag) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>(tag[i]));
        }
    };

    const u32 fmt_size = 16;
    const u32 riff_size = 4 + (8 + fmt_size) + (8 + static_cast<u32>(data.size()));
    push_tag("RIFF");
    push_u32(riff_size);
    push_tag("WAVE");
    push_tag("fmt ");
    push_u32(fmt_size);
    push_u16(format_tag);
    push_u16(channels);
    push_u32(sample_rate);
    push_u32(sample_rate * block_align);
    push_u16(block_align);
    push_u16(bits);
    push_tag("data");
    push_u32(static_cast<u32>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// One IMA ADPCM block: a 16-bit predictor, an 8-bit step index, a reserved byte,
// then the samples as nibbles packed low-first. A mono block is what the tests
// build; the multi-channel interleave is exercised by the frame count.
std::vector<u8> ima_block(i16 predictor, u8 index, const std::vector<u8>& nibbles) {
    std::vector<u8> block;
    block.push_back(static_cast<u8>(predictor & 0xFF));
    block.push_back(static_cast<u8>((predictor >> 8) & 0xFF));
    block.push_back(index);
    block.push_back(0);
    for (usize i = 0; i + 1 < nibbles.size(); i += 2) {
        block.push_back(static_cast<u8>((nibbles[i] & 0x0F) | ((nibbles[i + 1] & 0x0F) << 4)));
    }
    return block;
}

// Reads every frame of a stream into one interleaved buffer.
std::vector<f32> read_all(audio::AudioStream& stream, u32 chunk) {
    std::vector<f32> all;
    std::vector<f32> block;
    while (!stream.at_end()) {
        const u32 got = stream.read(chunk, block);
        if (got == 0) {
            break;
        }
        all.insert(all.end(), block.begin(), block.end());
    }
    return all;
}

} // namespace

TEST_CASE("a mu-law wav decodes through the G.711 reference") {
    // 0x00 and 0x80 are the two full-scale mu-law codes: one is +full, the other
    // -full, and both are exactly representable, so the decode is asserted
    // against the ITU-T reference rather than a round-trip.
    const auto bytes = make_wav(6, 1, 8000, 8, {0x00, 0x80});
    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    CHECK(decoded.value().format == audio::AudioFormat{8000, 1});
    REQUIRE(decoded.value().samples.size() == 2);
    CHECK(decoded.value().samples[0] == doctest::Approx(-32124.0f / 32768.0f));
    CHECK(decoded.value().samples[1] == doctest::Approx(32124.0f / 32768.0f));
}

TEST_CASE("an A-law wav decodes through the G.711 reference") {
    // 0x55 is A-law zero and 0xD5 is its negative; both decode to the small step
    // the reference produces for exponent zero. The sign bit is the top bit of
    // the decoded byte, so 0x55 (after the 0x55 xor, 0x00) is negative and 0xD5
    // (0x80) is positive.
    const auto bytes = make_wav(7, 1, 8000, 8, {0x55, 0xD5});
    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    CHECK(decoded.value().format == audio::AudioFormat{8000, 1});
    REQUIRE(decoded.value().samples.size() == 2);
    CHECK(decoded.value().samples[0] == doctest::Approx(-8.0f / 32768.0f));
    CHECK(decoded.value().samples[1] == doctest::Approx(8.0f / 32768.0f));
}

TEST_CASE("an IMA ADPCM wav decodes a block from its header and nibbles") {
    // Predictor 1000, step index 0, eight nibbles of value 4 (bit 2 set, so each
    // adds the full current step on top of step>>3, and the step index climbs by
    // 2 each time). The first frame is the header predictor; the rest are:
    //   1000, 1007, 1017, 1029, 1043, 1061, 1082, 1107, 1138
    const std::vector<u8> nibbles(8, 4);
    const auto block = ima_block(1000, 0, nibbles);
    const auto bytes = make_wav_raw(0x11, 1, 8000, 4, 8, block);

    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    CHECK(decoded.value().format == audio::AudioFormat{8000, 1});
    REQUIRE(decoded.value().samples.size() == 9);
    const std::array<i32, 9> expected = {1000, 1007, 1017, 1029, 1043, 1061, 1082, 1107, 1138};
    for (usize i = 0; i < expected.size(); ++i) {
        CHECK(decoded.value().samples[i] == doctest::Approx(static_cast<f32>(expected[i]) / 32768.0f));
    }
}

TEST_CASE("an IMA ADPCM block with zero nibbles holds the predictor steady") {
    // A zero nibble has diff = step>>3, which is zero for the smallest step, so
    // the predictor never moves: every frame equals the header predictor.
    const std::vector<u8> nibbles(8, 0);
    const auto block = ima_block(-512, 0, nibbles);
    const auto bytes = make_wav_raw(0x11, 1, 8000, 4, 8, block);

    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    REQUIRE(decoded.value().samples.size() == 9);
    for (f32 sample : decoded.value().samples) {
        CHECK(sample == doctest::Approx(-512.0f / 32768.0f));
    }
}

TEST_CASE("two IMA ADPCM blocks decode independently and concatenate") {
    // Each block resets from its own header, so two blocks with different
    // predictors decode to two runs rather than one continuation.
    auto data = ima_block(100, 0, std::vector<u8>(8, 0));
    const auto second = ima_block(200, 0, std::vector<u8>(8, 0));
    data.insert(data.end(), second.begin(), second.end());
    const auto bytes = make_wav_raw(0x11, 1, 8000, 4, 8, data);

    auto decoded = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(decoded);
    REQUIRE(decoded.value().samples.size() == 18);
    CHECK(decoded.value().samples[0] == doctest::Approx(100.0f / 32768.0f));
    CHECK(decoded.value().samples[8] == doctest::Approx(100.0f / 32768.0f));
    CHECK(decoded.value().samples[9] == doctest::Approx(200.0f / 32768.0f));
    CHECK(decoded.value().samples[17] == doctest::Approx(200.0f / 32768.0f));
}

TEST_CASE("an unsupported wav format is an error, not a guess") {
    // A tag the loader does not decode (0x02, Microsoft ADPCM) must be rejected
    // rather than read as if it were PCM.
    const auto bytes = make_wav(2, 1, 8000, 4, {0, 0, 0, 0});
    CHECK_FALSE(audio::WavLoader::parse(bytes.data(), bytes.size()));
}

// ---------------------------------------------------------------------------
// Streaming decode
// ---------------------------------------------------------------------------

TEST_CASE("a stream returns the same samples as a whole-file decode") {
    const auto bytes = make_wav(1, 1, 8000, 16, {-32768, -16384, 0, 16384, 32767});

    auto clip = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(clip);

    auto stream = audio::WavStream::from_memory(bytes);
    REQUIRE(stream);
    CHECK(stream.value()->format() == clip.value().format);
    CHECK(stream.value()->frame_count() == clip.value().samples.size());

    const auto streamed = read_all(*stream.value(), 2);
    REQUIRE(streamed.size() == clip.value().samples.size());
    for (usize i = 0; i < streamed.size(); ++i) {
        CHECK(streamed[i] == doctest::Approx(clip.value().samples[i]));
    }
}

TEST_CASE("a stream decodes compressed formats identically to the loader") {
    // The same block-codec path backs both, so a streamed ADPCM sound and a
    // loaded one must agree sample for sample - the property that keeps the two
    // decode paths from drifting.
    const std::vector<u8> nibbles = {4, 4, 0, 8, 2, 6, 1, 9};
    const auto block = ima_block(1000, 0, nibbles);
    const auto bytes = make_wav_raw(0x11, 1, 8000, 4, 8, block);

    auto clip = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(clip);
    auto stream = audio::WavStream::from_memory(bytes);
    REQUIRE(stream);

    const auto streamed = read_all(*stream.value(), 4);
    REQUIRE(streamed.size() == clip.value().samples.size());
    for (usize i = 0; i < streamed.size(); ++i) {
        CHECK(streamed[i] == doctest::Approx(clip.value().samples[i]));
    }
}

TEST_CASE("a stream read is bounded and never returns more than asked") {
    // A 1000-frame PCM wav read four frames at a time yields 250 reads, each of
    // exactly four frames, so a large request cannot silently materialise more
    // than the caller asked for.
    std::vector<i32> values;
    for (int i = 0; i < 1000; ++i) {
        values.push_back((i % 100) - 50);
    }
    const auto bytes = make_wav(1, 1, 8000, 16, values);
    auto stream = audio::WavStream::from_memory(bytes);
    REQUIRE(stream);
    CHECK(stream.value()->frame_count() == 1000);

    std::vector<f32> out;
    u32 reads = 0;
    while (!stream.value()->at_end()) {
        const u32 got = stream.value()->read(4, out);
        CHECK(got <= 4);
        CHECK(out.size() == got);
        ++reads;
    }
    CHECK(reads == 250);
    CHECK(stream.value()->position() == 1000);

    // A read past the end returns nothing and leaves the cursor at the end.
    CHECK(stream.value()->read(8, out) == 0);
    CHECK(out.empty());
}

TEST_CASE("a stream seek is exact and resumes decoding at the new frame") {
    const auto bytes = make_wav(1, 1, 8000, 16, {-32768, -16384, 0, 16384, 32767});
    auto clip = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(clip);
    auto stream = audio::WavStream::from_memory(bytes);
    REQUIRE(stream);

    // Skip the first two frames, then read one: it must be the third sample.
    REQUIRE_FALSE(stream.value()->seek(2));
    CHECK(stream.value()->position() == 2);
    std::vector<f32> out;
    CHECK(stream.value()->read(1, out) == 1);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == doctest::Approx(clip.value().samples[2]));

    // A seek past the end clamps, and reading there is empty.
    REQUIRE_FALSE(stream.value()->seek(999));
    CHECK(stream.value()->position() == 5);
    CHECK(stream.value()->at_end());
    CHECK(stream.value()->read(1, out) == 0);

    // A seek back to the start replays the first sample.
    REQUIRE_FALSE(stream.value()->seek(0));
    CHECK(stream.value()->read(1, out) == 1);
    CHECK(out[0] == doctest::Approx(clip.value().samples[0]));
}

TEST_CASE("a stream seek into an IMA ADPCM block is frame-exact") {
    // Seeking past the header frame of an ADPCM block must land on the right
    // nibble, which only works if the block is decoded from its start.
    const std::vector<u8> nibbles(8, 4);
    const auto block = ima_block(1000, 0, nibbles);
    const auto bytes = make_wav_raw(0x11, 1, 8000, 4, 8, block);

    auto clip = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(clip);
    auto stream = audio::WavStream::from_memory(bytes);
    REQUIRE(stream);

    for (u64 frame : {u64{0}, u64{3}, u64{8}}) {
        REQUIRE_FALSE(stream.value()->seek(frame));
        std::vector<f32> out;
        REQUIRE(stream.value()->read(1, out) == 1);
        CHECK(out[0] == doctest::Approx(clip.value().samples[static_cast<usize>(frame)]));
    }
}

TEST_CASE("opening a stream over malformed bytes is an error, not a half-open stream") {
    const std::vector<u8> garbage = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    CHECK_FALSE(audio::WavStream::from_memory(garbage));
    CHECK_FALSE(audio::WavStream::from_memory({}));
}

TEST_CASE("WavStream opens a file from disk and reads it") {
    TempDir dir("stream");
    const std::string path = dir.file("tone.wav");
    const auto bytes = make_wav(1, 1, 8000, 16, {-32768, 0, 16384});
    REQUIRE_FALSE(fs::write_binary_file(path, bytes.data(), bytes.size()));

    auto stream = audio::WavStream::open(path);
    REQUIRE(stream);
    CHECK(stream.value()->frame_count() == 3);
    CHECK(stream.value()->format() == audio::AudioFormat{8000, 1});

    std::vector<f32> out;
    CHECK(stream.value()->read(3, out) == 3);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == doctest::Approx(-1.0f));
    CHECK(out[2] == doctest::Approx(0.5f));

    CHECK_FALSE(audio::WavStream::open(dir.file("absent.wav")));
}

// ---------------------------------------------------------------------------
// Streaming voices in the mixer
// ---------------------------------------------------------------------------

TEST_CASE("block_sample up-mixes mono and wraps a channel-count mismatch") {
    const f32 mono[2] = {0.25f, -0.5f};
    CHECK(audio::block_sample(mono, 1, 0, 0) == doctest::Approx(0.25f));
    CHECK(audio::block_sample(mono, 1, 1, 3) == doctest::Approx(-0.5f));

    const f32 stereo[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(audio::block_sample(stereo, 2, 0, 0) == doctest::Approx(0.1f));
    CHECK(audio::block_sample(stereo, 2, 0, 1) == doctest::Approx(0.2f));
    CHECK(audio::block_sample(stereo, 2, 1, 0) == doctest::Approx(0.3f));
    // A third output channel wraps onto the first source channel.
    CHECK(audio::block_sample(stereo, 2, 1, 2) == doctest::Approx(0.3f));

    CHECK(audio::block_sample(nullptr, 1, 0, 0) == doctest::Approx(0.0f));
}

TEST_CASE("voice_level folds gain, attenuation and bus gain into one number") {
    audio::Voice voice;
    voice.gain = 0.5f;
    audio::MixParams params;
    CHECK(audio::voice_level(voice, params) == doctest::Approx(0.5f));

    // A positional voice at its min_distance is at full attenuation.
    voice.spatial = true;
    voice.min_distance = 1.0f;
    voice.max_distance = 10.0f;
    voice.world_position = fe::Vec3{1.0f, 0.0f, 0.0f};
    params.listener_position = fe::Vec3{0.0f, 0.0f, 0.0f};
    CHECK(audio::voice_level(voice, params) == doctest::Approx(0.5f));

    // A bus gain scales it.
    const audio::AudioBus bus{"music", 0.5f};
    params.buses = &bus;
    params.bus_count = 1;
    voice.bus = 0;
    CHECK(audio::voice_level(voice, params) == doctest::Approx(0.25f));
}

TEST_CASE("add_streamed_voice adds a mono block to every output channel") {
    audio::Voice voice;
    voice.gain = 1.0f;
    voice.stream_channels = 1;

    const f32 samples[3] = {0.5f, -0.25f, 0.125f};
    f32 output[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    audio::MixParams params;
    const f32 peak = audio::add_streamed_voice(voice, samples, 3, 3, output, 2, 1.0, params);

    CHECK(peak == doctest::Approx(0.5f));
    CHECK(output[0] == doctest::Approx(0.5f));
    CHECK(output[1] == doctest::Approx(0.5f));
    CHECK(output[2] == doctest::Approx(-0.25f));
    CHECK(output[4] == doctest::Approx(0.125f));
}

TEST_CASE("add_streamed_voice resamples a block by the rate ratio") {
    audio::Voice voice;
    voice.gain = 1.0f;
    voice.stream_channels = 1;

    // Ratio 2.0: output frame 0 reads source 0, output frame 1 reads source 2.
    const f32 samples[3] = {0.5f, 0.25f, 0.125f};
    f32 output[2] = {0.0f, 0.0f};
    audio::MixParams params;
    const f32 peak = audio::add_streamed_voice(voice, samples, 3, 2, output, 1, 2.0, params);

    CHECK(peak == doctest::Approx(0.5f));
    CHECK(output[0] == doctest::Approx(0.5f));
    CHECK(output[1] == doctest::Approx(0.125f));
}

TEST_CASE("add_streamed_voice adds to the existing sum rather than replacing it") {
    audio::Voice voice;
    voice.gain = 1.0f;
    voice.stream_channels = 1;

    const f32 samples[1] = {0.25f};
    f32 output[1] = {0.5f}; // a clip voice already contributed here
    audio::MixParams params;
    static_cast<void>(audio::add_streamed_voice(voice, samples, 1, 1, output, 1, 1.0, params));
    CHECK(output[0] == doctest::Approx(0.75f));
}

// ---------------------------------------------------------------------------
// Streaming through the AudioSystem
// ---------------------------------------------------------------------------

TEST_CASE("a streaming source plays from its file and stops at the end") {
    TempDir dir("stream_system");
    const std::string path = dir.file("music.wav");
    // 16 frames of half-scale DC, exactly representable in 16-bit PCM.
    const auto bytes = make_wav(1, 1, 8000, 16, std::vector<i32>(16, 16384));
    REQUIRE_FALSE(fs::write_binary_file(path, bytes.data(), bytes.size()));

    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Music");
    auto* source = node->add_component<audio::AudioSource>();
    // The clip's path is what the stream opens; the clip's own samples are
    // deliberately different so a test failure cannot be masked by the loaded
    // path quietly being used instead.
    source->clip = std::make_shared<audio::AudioClip>(path, audio::AudioFormat{8000, 1}, std::vector<f32>(16, -1.0f));
    source->stream = true;
    source->loop = false;
    source->play_on_start = true;

    // 1 ms at 8 kHz is 8 frames; the file holds 16, so two updates play it out.
    system.update(step(0.001f), scene);
    CHECK(system.playing_voice_count() == 1);
    CHECK(system.last_peak() == doctest::Approx(0.5f));
    REQUIRE(device.last_block().size() == 8);
    CHECK(device.last_block()[0] == doctest::Approx(0.5f));

    system.update(step(0.001f), scene);
    CHECK(system.playing_voice_count() == 1);
    CHECK(system.last_peak() == doctest::Approx(0.5f));

    // The third update finds the stream exhausted: the voice finishes silently.
    system.update(step(0.001f), scene);
    CHECK(system.playing_voice_count() == 0);
    CHECK(system.last_peak() == doctest::Approx(0.0f));
}

TEST_CASE("a streaming source and a loaded source produce the same mix") {
    TempDir dir("stream_equal");
    const std::string path = dir.file("tone.wav");
    const std::vector<i32> values = {-32768, -16384, 0, 16384, 32767, 8192, -8192, 0};
    const auto bytes = make_wav(1, 1, 8000, 16, values);
    REQUIRE_FALSE(fs::write_binary_file(path, bytes.data(), bytes.size()));

    auto loaded_clip = audio::WavLoader::parse(bytes.data(), bytes.size());
    REQUIRE(loaded_clip);

    audio::NullAudioDevice loaded_device;
    REQUIRE(loaded_device.open(8000, 1));
    REQUIRE_FALSE(loaded_device.start());
    audio::AudioSystem loaded_system(&loaded_device);
    Scene loaded_scene;
    Node* loaded_node = loaded_scene.create_node("Loaded");
    auto* loaded_source = loaded_node->add_component<audio::AudioSource>();
    loaded_source->clip =
        std::make_shared<audio::AudioClip>(path, loaded_clip.value().format, loaded_clip.value().samples);
    loaded_source->play_on_start = true;
    loaded_system.update(step(0.001f), loaded_scene);

    audio::NullAudioDevice stream_device;
    REQUIRE(stream_device.open(8000, 1));
    REQUIRE_FALSE(stream_device.start());
    audio::AudioSystem stream_system(&stream_device);
    Scene stream_scene;
    Node* stream_node = stream_scene.create_node("Streamed");
    auto* stream_source = stream_node->add_component<audio::AudioSource>();
    stream_source->clip = std::make_shared<audio::AudioClip>(path, loaded_clip.value().format, std::vector<f32>{});
    stream_source->stream = true;
    stream_source->play_on_start = true;
    stream_system.update(step(0.001f), stream_scene);

    REQUIRE(loaded_device.last_block().size() == stream_device.last_block().size());
    for (usize i = 0; i < loaded_device.last_block().size(); ++i) {
        CHECK(stream_device.last_block()[i] == doctest::Approx(loaded_device.last_block()[i]));
    }
}

TEST_CASE("a streaming source whose file is missing falls back to its loaded clip") {
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Ghost");
    auto* source = node->add_component<audio::AudioSource>();
    // The file does not exist, so the stream cannot be opened. The source must
    // degrade to playing the clip it already holds rather than crashing or
    // falling silent, so a stream is never a hard requirement on a file.
    source->clip = std::make_shared<audio::AudioClip>("does/not/exist.wav", audio::AudioFormat{8000, 1},
                                                      std::vector<f32>(8, 0.25f));
    source->stream = true;
    source->play_on_start = true;

    system.update(step(0.001f), scene);
    CHECK(system.last_peak() == doctest::Approx(0.25f));
    CHECK(system.playing_voice_count() == 1);
}

// ---------------------------------------------------------------------------
// The browser autoplay policy: a suspended audio context, resumed by a gesture.
// ---------------------------------------------------------------------------

TEST_CASE("a suspended device is started but silent, and a gesture resumes it") {
    // Models the web contract: the context opens suspended, so start() succeeds
    // and the device reports playing, yet nothing is accepted until a gesture.
    audio::NullAudioDevice device;
    device.set_starts_suspended(true);
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    CHECK(device.is_playing());
    CHECK(device.needs_user_gesture());
    CHECK(device.is_suspended());

    const f32 block[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    CHECK(device.submit(block, 4) == 0);
    CHECK(device.frames_submitted() == 0);

    CHECK(device.resume_from_gesture());
    CHECK_FALSE(device.is_suspended());
    CHECK_FALSE(device.needs_user_gesture());
    CHECK(device.submit(block, 4) == 4);
    CHECK(device.frames_submitted() == 4);
}

TEST_CASE("a device that needs no gesture plays as soon as it starts") {
    // The desktop path, pinned: no suspension, so start() is immediately
    // audible and resume_from_gesture() is a harmless no-op.
    audio::NullAudioDevice device;
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    CHECK_FALSE(device.needs_user_gesture());
    CHECK_FALSE(device.is_suspended());

    const f32 block[2] = {0.25f, 0.25f};
    CHECK(device.submit(block, 2) == 2);
    CHECK(device.resume_from_gesture());
    CHECK(device.submit(block, 2) == 2);
    CHECK(device.frames_submitted() == 4);
}

TEST_CASE("AudioSystem resumes a suspended device on the first user gesture") {
    // The end-to-end contract of `web-audio-context`: a started device stays
    // silent while the context is suspended, and the first input gesture resumes
    // it so the same frame's mix is audible.
    audio::NullAudioDevice device;
    device.set_starts_suspended(true);
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Tone");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(800, 1.0f));
    source->loop = true;
    source->play_on_start = true;

    InputState input;

    // No gesture yet: the system mixes, but the suspended device accepts
    // nothing, so the page is silent.
    FrameContext quiet = step(0.001f);
    quiet.input = &input;
    system.update(quiet, scene);
    CHECK(system.mixed_frames() == 8);
    CHECK(device.frames_submitted() == 0);
    CHECK(device.is_suspended());

    // A key press is a gesture. The same frame's output reaches the device.
    input.on_key(Key::Space, true);
    FrameContext gesture = step(0.001f);
    gesture.input = &input;
    system.update(gesture, scene);
    CHECK_FALSE(device.is_suspended());
    CHECK(device.frames_submitted() == 8);

    // Later frames need no further gesture: the device stays resumed.
    input.begin_frame();
    FrameContext later = step(0.001f);
    later.input = &input;
    system.update(later, scene);
    CHECK(device.frames_submitted() == 16);
}

TEST_CASE("a headless frame with no input does not crash a suspended device") {
    // A tool or test may run the schedule with no input at all. The resume check
    // must tolerate a null input rather than dereference it.
    audio::NullAudioDevice device;
    device.set_starts_suspended(true);
    REQUIRE(device.open(8000, 1));
    REQUIRE_FALSE(device.start());

    audio::AudioSystem system(&device);
    Scene scene;
    Node* node = scene.create_node("Tone");
    auto* source = node->add_component<audio::AudioSource>();
    source->clip = mono_clip(8000, std::vector<f32>(800, 1.0f));
    source->loop = true;
    source->play_on_start = true;

    FrameContext context = step(0.001f); // input left null
    system.update(context, scene);
    CHECK(system.mixed_frames() == 8);
    CHECK(device.frames_submitted() == 0);
}
