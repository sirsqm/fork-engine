// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/audio/AudioClip.h"
#include "fe/audio/AudioDevice.h"
#include "fe/audio/AudioMixer.h"
#include "fe/audio/AudioSource.h"
#include "fe/audio/AudioSystem.h"
#include "fe/audio/AudioTypes.h"
#include "fe/audio/WavLoader.h"
#include "fe/core/FileSystem.h"
#include "fe/reflect/Registry.h"
#include "fe/resources/ResourceManager.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/serialization/SceneSerializer.h"
#include "fe/systems/FrameContext.h"

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

    auto json = serialization::scene::to_json(scene);
    REQUIRE(json);
    CHECK(json.value().find("\"AudioSource\"") != std::string::npos);
    CHECK(json.value().find("\"wav:assets/beep.wav\"") != std::string::npos);

    Scene loaded;
    REQUIRE_FALSE(serialization::scene::from_json(json.value(), loaded));
    Node* loaded_node = loaded.find_node_by_name("Music");
    REQUIRE(loaded_node != nullptr);
    auto* loaded_source = loaded_node->get_component<audio::AudioSource>();
    REQUIRE(loaded_source != nullptr);
    CHECK(loaded_source->gain == doctest::Approx(0.25f));
    CHECK(loaded_source->pitch == doctest::Approx(1.5f));
    CHECK(loaded_source->loop);
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
