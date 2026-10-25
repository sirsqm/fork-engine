// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// A headless performance benchmark for the runtime.
//
// It builds a synthetic 2D scene (sprites to render, static colliders, and
// dynamic bodies to simulate), runs a fixed number of frames through the real
// Application frame loop, and reports frame-time percentiles. The numbers are
// meant to be recorded by CI and compared against a stored baseline: the
// benchmark is a regression gate, not a micro-optimisation toy.
//
// Why percentiles and not an average: a frame-time average hides the tail that
// actually stutters. p50 says how fast a typical frame is, p95/p99 say how bad
// the worst frames are, and the gate is set on p50 so ordinary CI noise does not
// trip it while a real regression still does.
//
// The work is deliberately made of the engine's own systems rather than a
// synthetic loop, so the measurement reflects the code the engine ships:
//
//   * renderer2d   -- batched sprite draws over the whole scene
//   * physics2d    -- the sequential-impulse solver over the dynamic bodies
//   * the profiler -- already brackets frame/update/frame/render, which is where
//                     the reported timings come from
//
// It runs headless (a hidden window with a real GL context), so the render path
// is identical to an interactive run and it works under Xvfb in CI.
//
// Usage:
//   fork_bench [--scene NAME] [--sprites N] [--bricks N] [--bodies N]
//              [--frames N] [--warmup N] [--workers N] [--json] [--out PATH]
//              [--verbose]
//
// Output: a machine-readable block on stdout (CSV by default, JSON with
// --json) and a human summary in the log. `--out PATH` also writes the block to
// a file, which is what CI records as an artifact.
//
// `--workers N` sizes the job pool that extraction and culling use (M9,
// ADR-0069). The default is 0 -- the single-threaded system every plain run
// uses -- so the recorded baseline and the CI gate measure the serial path; pass
// `--workers 4` to reproduce the parallel win.

#include "fe/core/Log.h"
#include "fe/core/Profiler.h"
#include "fe/mesh/Mesh.h"
#include "fe/physics2d/Collider2D.h"
#include "fe/physics2d/Physics2DSystem.h"
#include "fe/physics2d/RigidBody2D.h"
#include "fe/physics3d/Collider3D.h"
#include "fe/physics3d/Physics3DSystem.h"
#include "fe/physics3d/RigidBody3D.h"
#include "fe/platform/Window.h"
#include "fe/renderer3d/Renderer3D.h"
#include "fe/resources/Texture2D.h"
#include "fe/rhi/Types.h"
#include "fe/runtime/Application.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Node.h"
#include "fe/scene/Sprite2D.h"
#include "fe/scene3d/Camera3D.h"
#include "fe/scene3d/DirectionalLight3D.h"
#include "fe/scene3d/MeshRenderer3D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string scene = "sprite_heavy";
    fe::u32 sprites = 2000;
    fe::u32 bricks = 500;
    fe::u32 bodies = 200;
    fe::u32 frames = 240;
    fe::u32 warmup = 20;
    // The job pool's worker count. Zero (the default) is the single-threaded
    // system every plain run uses, so the recorded baseline is unchanged and the
    // gate measures the serial path. Set it to reproduce the parallel
    // extract/cull win: `--workers 4`.
    fe::u32 workers = 0;
    // Give the sprites a real texture so extraction produces instances and the
    // render workload is real. Off by default: the recorded baseline was taken
    // with untextured sprites, so flipping this on would change what the gate
    // measures. It is for measuring the extract/cull path (`--workers 4`), not
    // for the gate.
    bool textured = false;
    bool json = false;
    std::string out;
    fe::LogLevel log_level = fe::LogLevel::Warn;
};

// The presets a caller can select by name. Explicit flags override any preset,
// so `--scene physics_heavy --bodies 4000` works as expected.
void apply_preset(Options& options) {
    if (options.scene == "sprite_heavy") {
        options.sprites = 2000;
        options.bricks = 500;
        options.bodies = 200;
    } else if (options.scene == "physics_heavy") {
        options.sprites = 200;
        options.bricks = 1000;
        options.bodies = 1500;
    } else if (options.scene == "mixed") {
        options.sprites = 1000;
        options.bricks = 750;
        options.bodies = 750;
    } else if (options.scene == "mesh_heavy") {
        // The 3D path's counterpart to sprite_heavy: the workload is meshes to
        // extract, cull, shadow and draw, plus 3D bodies for the 3D solver. The
        // counts are lower than the 2D scenes on purpose -- each mesh is a box of
        // 24 vertices rather than a quad, and the shadow pass draws every caster
        // a second time, so a comparable node count is a much heavier frame.
        options.sprites = 800;
        options.bricks = 100;
        options.bodies = 100;
    }
    // An unknown name is treated as a label with the current counts, which is
    // useful for ad-hoc runs; it is not an error.
}

// Whether a scene name builds the 3D workload. The benchmark has two scene
// builders (2D sprites/physics and 3D meshes/physics) because the two renderers
// share the frame loop but nothing else; a scene is one or the other, and the
// name decides which. This is what gives the 3D path the same measured,
// regression-gated coverage the 2D path has had since ADR-0033.
[[nodiscard]] bool is_3d_scene(const std::string& scene) {
    return scene == "mesh_heavy";
}

// --- Statistics ------------------------------------------------------------

struct Stats {
    fe::f64 p50 = 0.0;
    fe::f64 p95 = 0.0;
    fe::f64 p99 = 0.0;
    fe::f64 mean = 0.0;
    fe::f64 min = 0.0;
    fe::f64 max = 0.0;
    fe::usize samples = 0;
};

// Interpolated percentile on a sorted, non-empty series.
[[nodiscard]] fe::f64 percentile_of(const std::vector<fe::f64>& sorted, fe::f64 fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (fraction <= 0.0) {
        return sorted.front();
    }
    if (fraction >= 1.0) {
        return sorted.back();
    }
    const fe::f64 rank = fraction * static_cast<fe::f64>(sorted.size() - 1);
    const auto lower = static_cast<fe::usize>(std::floor(rank));
    const auto upper = static_cast<fe::usize>(std::ceil(rank));
    const fe::f64 weight = rank - static_cast<fe::f64>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] Stats summarize(std::vector<fe::f64> values) {
    Stats stats;
    if (values.empty()) {
        return stats;
    }
    std::sort(values.begin(), values.end());
    fe::f64 sum = 0.0;
    for (const fe::f64 value : values) {
        sum += value;
    }
    stats.samples = values.size();
    stats.min = values.front();
    stats.max = values.back();
    stats.mean = sum / static_cast<fe::f64>(values.size());
    stats.p50 = percentile_of(values, 0.50);
    stats.p95 = percentile_of(values, 0.95);
    stats.p99 = percentile_of(values, 0.99);
    return stats;
}

// --- The benchmark application ---------------------------------------------

class BenchmarkApp final : public fe::Application {
public:
    BenchmarkApp(fe::Engine::Config config, Options options) : fe::Application(config), options_(std::move(options)) {}

protected:
    fe::Error on_start() override {
        scene().set_name("Benchmark/" + options_.scene);
        return is_3d_scene(options_.scene) ? build_3d_scene() : build_2d_scene();
    }

    // The 3D workload: a camera and a directional light, `sprites` boxes to draw
    // (the 3D renderer's extraction, culling, shadow pass and draw), `bricks`
    // static boxes as the 3D broad phase's obstacles, and `bodies` dynamic boxes
    // for the 3D solver. It mirrors the 2D builder so the two are read side by
    // side.
    fe::Error build_3d_scene() {
        fe::Node* camera_node = scene().create_node("Camera");
        camera_node->transform().position = fe::Vec3{0.0f, 6.0f, 18.0f};
        auto* camera = camera_node->add_component<fe::Camera3D>();
        camera->background_enabled = true;

        fe::Node* light_node = scene().create_node("Sun");
        auto* light = light_node->add_component<fe::DirectionalLight3D>();
        light->direction = fe::Vec3{-0.4f, -1.0f, -0.3f};
        light->intensity = 1.0f;
        light->ambient = 0.15f;

        // One shared mesh for every box, so the renderer's buffer cache holds a
        // single upload and the measurement is the per-instance path rather than
        // a per-mesh one. A real scene would reuse meshes the same way.
        const std::shared_ptr<fe::Mesh> box = fe::Mesh::make_cube();
        const std::shared_ptr<fe::Mesh> floor = fe::Mesh::make_plane(40.0f, 40.0f);

        // Drawable boxes: the renderer's workload. Spread in a grid and lifted
        // slightly so they sit above the floor and cast a shadow.
        for (fe::u32 i = 0; i < options_.sprites; ++i) {
            fe::Node* node = scene().create_node("Box");
            auto* renderer = node->add_component<fe::MeshRenderer3D>();
            renderer->mesh = box;
            const fe::f32 x = static_cast<fe::f32>(i % 32) * 1.6f - 25.0f;
            const fe::f32 z = static_cast<fe::f32>(i / 32) * 1.6f - 12.0f;
            node->transform().position = fe::Vec3{x, 1.0f, z};
            node->transform().scale = fe::Vec3{0.8f, 0.8f, 0.8f};
        }

        // Static obstacles for the 3D broad phase: no rigid body, so they never
        // move but the solver still prefilters against them.
        for (fe::u32 i = 0; i < options_.bricks; ++i) {
            fe::Node* node = scene().create_node("Brick");
            auto* collider = node->add_component<fe::physics3d::Collider3D>();
            collider->shape = fe::physics3d::ShapeType3D::Box;
            collider->half_extent = fe::Vec3{0.5f, 0.5f, 0.5f};
            const fe::f32 x = static_cast<fe::f32>(i % 10) * 2.0f - 10.0f;
            const fe::f32 z = static_cast<fe::f32>(i / 10) * 2.0f - 10.0f;
            node->transform().position = fe::Vec3{x, 0.5f, z};
        }

        // Dynamic bodies for the 3D solver: spheres with restitution 1 and no
        // sleeping, so they keep bouncing and the solver does real work every
        // frame rather than settling.
        for (fe::u32 i = 0; i < options_.bodies; ++i) {
            fe::Node* node = scene().create_node("Body");
            auto* collider = node->add_component<fe::physics3d::Collider3D>();
            collider->shape = fe::physics3d::ShapeType3D::Sphere;
            collider->radius = 0.5f;
            collider->restitution = 1.0f;
            collider->friction = 0.0f;
            auto* body = node->add_component<fe::physics3d::RigidBody3D>();
            body->mass = 1.0f;
            const fe::f32 angle = static_cast<fe::f32>(i) * 0.7f;
            body->velocity = fe::Vec3{std::cos(angle), 1.5f, std::sin(angle)} * 6.0f;
            const fe::f32 x = static_cast<fe::f32>(i % 20) * 2.0f - 20.0f;
            const fe::f32 z = static_cast<fe::f32>(i / 20) * 2.0f - 10.0f;
            node->transform().position = fe::Vec3{x, 4.0f, z};
        }

        // A floor, so the bodies land on something and the boxes have a surface
        // to cast a shadow onto.
        fe::Node* floor_node = scene().create_node("Floor");
        auto* floor_renderer = floor_node->add_component<fe::MeshRenderer3D>();
        floor_renderer->mesh = floor;
        auto* floor_collider = floor_node->add_component<fe::physics3d::Collider3D>();
        floor_collider->shape = fe::physics3d::ShapeType3D::Box;
        floor_collider->half_extent = fe::Vec3{40.0f, 0.5f, 40.0f};
        floor_node->transform().position = fe::Vec3{0.0f, -0.5f, 0.0f};

        auto* physics = schedule().add_system<fe::physics3d::Physics3DSystem>();
        physics->gravity = fe::Vec3{0.0f, -9.81f, 0.0f};

        FE_LOG_INFO("benchmark scene '%s': %zu nodes (%u meshes, %u static colliders, %u bodies)",
                    options_.scene.c_str(), scene().node_count(), options_.sprites, options_.bricks, options_.bodies);
        return fe::Error::none();
    }

    fe::Error build_2d_scene() {

        fe::Node* camera_node = scene().create_node("Camera");
        camera_node->add_component<fe::Camera2D>();

        // Sprites: the renderer's workload. Left untextured on purpose, so the
        // benchmark does not depend on a decode step and every sprite batches
        // into the same (white-texture) draw group. Untextured sprites are
        // skipped by extraction (an authoring state, not content), so this is
        // the physics-and-systems workload; pass --textured to exercise the
        // extract/cull path too.
        const std::shared_ptr<fe::Texture2D> texture = options_.textured ? make_bench_texture() : nullptr;
        for (fe::u32 i = 0; i < options_.sprites; ++i) {
            fe::Node* node = scene().create_node("Sprite");
            auto* sprite = node->add_component<fe::Sprite2D>();
            sprite->size = fe::Vec2{16.0f, 16.0f};
            sprite->texture = texture;
            // Spread over a wide grid so the scene is not all one point.
            const fe::f32 x = static_cast<fe::f32>(i % 100) * 20.0f;
            const fe::f32 y = static_cast<fe::f32>(i / 100) * 20.0f;
            node->transform().position = fe::Vec3{x, y, 0.0f};
        }

        // Static colliders: the broad phase's workload. No rigid body, so they
        // are obstacles the solver must prefilter against but never move.
        for (fe::u32 i = 0; i < options_.bricks; ++i) {
            fe::Node* node = scene().create_node("Brick");
            auto* collider = node->add_component<fe::physics2d::Collider2D>();
            collider->shape = fe::physics2d::ShapeType::Box;
            collider->half_extent = fe::Vec2{8.0f, 4.0f};
            const fe::f32 x = static_cast<fe::f32>(i % 50) * 24.0f;
            const fe::f32 y = static_cast<fe::f32>(i / 50) * 16.0f + 400.0f;
            node->transform().position = fe::Vec3{x, y, 0.0f};
        }

        // Dynamic bodies: the solver's workload. Circles bouncing in a box, so
        // the solver does real impulse work every frame rather than settling
        // once and sleeping.
        for (fe::u32 i = 0; i < options_.bodies; ++i) {
            fe::Node* node = scene().create_node("Body");
            auto* collider = node->add_component<fe::physics2d::Collider2D>();
            collider->shape = fe::physics2d::ShapeType::Circle;
            collider->radius = 6.0f;
            collider->restitution = 1.0f;
            collider->friction = 0.0f;
            auto* body = node->add_component<fe::physics2d::RigidBody2D>();
            body->mass = 1.0f;
            body->fixed_rotation = true;
            // Never sleep: a sleeping body would be skipped by the solver and
            // the benchmark would measure less work than the scene implies.
            body->can_sleep = false;
            // A deterministic spread of directions, so two runs with the same
            // counts simulate the same scene.
            const fe::f32 angle = static_cast<fe::f32>(i) * 0.7f;
            body->velocity = fe::Vec2{std::cos(angle), std::sin(angle)} * 220.0f;
            const fe::f32 x = static_cast<fe::f32>(i % 40) * 24.0f - 480.0f;
            const fe::f32 y = static_cast<fe::f32>(i / 40) * 24.0f;
            node->transform().position = fe::Vec3{x, y, 0.0f};
        }

        // Four static walls so the bodies bounce instead of drifting away.
        add_wall("WallTop", fe::Vec2{0.0f, 720.0f}, fe::Vec2{600.0f, 8.0f});
        add_wall("WallBottom", fe::Vec2{0.0f, -720.0f}, fe::Vec2{600.0f, 8.0f});
        add_wall("WallLeft", fe::Vec2{-600.0f, 0.0f}, fe::Vec2{8.0f, 720.0f});
        add_wall("WallRight", fe::Vec2{600.0f, 0.0f}, fe::Vec2{8.0f, 720.0f});

        auto* physics = schedule().add_system<fe::physics2d::Physics2DSystem>();
        physics->gravity = fe::Vec2{0.0f, 0.0f};

        FE_LOG_INFO("benchmark scene '%s': %zu nodes (%u sprites, %u static colliders, %u bodies)",
                    options_.scene.c_str(), scene().node_count(), options_.sprites, options_.bricks, options_.bodies);
        return fe::Error::none();
    }

    // The 3D pass is the app's own step, exactly as it is in the player: the
    // Application frame draws 2D, and a 3D scene is rendered here. Running the 3D
    // scene through the same frame loop as the 2D one is the point -- the
    // profiler's `frame/render` bracket then covers the 3D renderer's extraction,
    // culling, shadow pass and draw, so the gate protects it the same way.
    void on_render(fe::f32) override {
        if (!is_3d_scene(options_.scene)) {
            return;
        }
        if (!engine().has_graphics() && !engine().headless_render()) {
            return;
        }
        auto drawn = engine().renderer_3d().render(engine().device(), scene(), engine().window().framebuffer_width(),
                                                   engine().window().framebuffer_height());
        if (!drawn) {
            FE_LOG_ERROR("benchmark: 3D render failed: %s", drawn.error().message().c_str());
        }
    }

    void on_stop() override { report(); }

private:
    // A small solid texture for the sprite workload, created through the RHI so
    // it works against the null device too. The benchmark does not need a decode
    // step; a checkerboard is enough for the sprites to be textured and for
    // extraction to produce instances.
    [[nodiscard]] std::shared_ptr<fe::Texture2D> make_bench_texture() {
        constexpr fe::u32 kSize = 16;
        std::array<fe::u8, kSize * kSize * 4> pixels{};
        for (fe::u32 y = 0; y < kSize; ++y) {
            for (fe::u32 x = 0; x < kSize; ++x) {
                const bool light = ((x / 4) + (y / 4)) % 2 == 0;
                const fe::u8 value = light ? 255 : 64;
                const fe::usize index = (static_cast<fe::usize>(y) * kSize + x) * 4;
                pixels[index + 0] = value;
                pixels[index + 1] = value;
                pixels[index + 2] = value;
                pixels[index + 3] = 255;
            }
        }

        fe::rhi::TextureDesc desc;
        desc.name = "bench_sprite";
        desc.width = kSize;
        desc.height = kSize;
        desc.pixels = pixels.data();
        auto created = engine().device().create_texture(desc);
        if (!created) {
            FE_LOG_WARN("benchmark: could not create the sprite texture: %s", created.error().message().c_str());
            return nullptr;
        }
        return std::make_shared<fe::Texture2D>("bench:sprite.png", created.value());
    }

    void add_wall(const char* name, const fe::Vec2& center, const fe::Vec2& half_extent) {
        fe::Node* node = scene().create_node(name);
        auto* collider = node->add_component<fe::physics2d::Collider2D>();
        collider->shape = fe::physics2d::ShapeType::Box;
        collider->half_extent = half_extent;
        collider->restitution = 1.0f;
        node->transform().position = fe::Vec3{center.x, center.y, 0.0f};
    }

    // Collects the frame series from the profiler ring and prints them. The
    // first `warmup` frames are dropped: the first frames pay one-off costs
    // (shader compile, buffer growth, allocator warm-up) that are not what a
    // steady-state regression gate should track.
    void report() const {
        const fe::Profiler& profiler = this->profiler();
        const fe::usize committed = profiler.frame_count();

        std::vector<fe::f64> total;
        std::vector<fe::f64> update;
        std::vector<fe::f64> render;

        // Offset 0 is the most recent frame; walk back over everything the ring
        // still holds. The ring keeps only the most recent frames
        // (Profiler::kMaxFrames), so for a run longer than the ring the warm-up
        // frames have already fallen out of it and nothing needs skipping. The
        // index used for the warm-up test is therefore the *true* frame index
        // (`frames - 1 - offset`), not the ring position: a run of 120 frames
        // keeps frames 57..120, all of which are steady state.
        for (fe::usize offset = 0; offset < committed; ++offset) {
            const auto frame = profiler.frame_report(offset);
            if (!frame) {
                continue;
            }
            const auto index_from_start = static_cast<fe::f64>(options_.frames) - 1.0 - static_cast<fe::f64>(offset);
            if (index_from_start < static_cast<fe::f64>(options_.warmup)) {
                continue; // warm-up frame
            }
            total.push_back(frame.value().total_ms);
            update.push_back(frame.value().update_ms);
            render.push_back(frame.value().render_ms);
        }

        const Stats total_stats = summarize(total);
        const Stats update_stats = summarize(update);
        const Stats render_stats = summarize(render);

        if (total_stats.samples == 0) {
            FE_LOG_WARN("benchmark produced no measurable frames (ran %u, ring holds %zu)", options_.frames, committed);
        } else {
            FE_LOG_INFO("frame total: p50=%.3fms p95=%.3fms p99=%.3fms mean=%.3fms (%zu samples)", total_stats.p50,
                        total_stats.p95, total_stats.p99, total_stats.mean, total_stats.samples);
        }

        const std::string block = options_.json ? to_json(total_stats, update_stats, render_stats)
                                                : to_csv(total_stats, update_stats, render_stats);
        std::fputs(block.c_str(), stdout);
        std::fflush(stdout);

        if (!options_.out.empty()) {
            if (std::FILE* file = std::fopen(options_.out.c_str(), "wb")) {
                std::fwrite(block.data(), 1, block.size(), file);
                std::fclose(file);
                FE_LOG_INFO("wrote benchmark result to '%s'", options_.out.c_str());
            } else {
                FE_LOG_WARN("could not write benchmark result to '%s'", options_.out.c_str());
            }
        }
    }

    [[nodiscard]] std::string to_csv(const Stats& total, const Stats& update, const Stats& render) const {
        std::string out;
        out += "# fork_engine benchmark\n";
        out += "# scene=" + options_.scene + " sprites=" + std::to_string(options_.sprites) +
               " bricks=" + std::to_string(options_.bricks) + " bodies=" + std::to_string(options_.bodies) +
               " frames=" + std::to_string(options_.frames) + " warmup=" + std::to_string(options_.warmup) + "\n";
        out += "metric,p50_ms,p95_ms,p99_ms,mean_ms,min_ms,max_ms,samples\n";
        append_row(out, "frame_total", total);
        append_row(out, "frame_update", update);
        append_row(out, "frame_render", render);
        return out;
    }
    static void append_row(std::string& out, const char* name, const Stats& stats) {
        char row[160];
        std::snprintf(row, sizeof(row), "%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu\n", name, stats.p50, stats.p95, stats.p99,
                      stats.mean, stats.min, stats.max, stats.samples);
        out += row;
    }

    [[nodiscard]] std::string to_json(const Stats& total, const Stats& update, const Stats& render) const {
        std::string out = "{\n";
        out += "  \"scene\": \"" + options_.scene + "\",\n";
        out += "  \"sprites\": " + std::to_string(options_.sprites) + ",\n";
        out += "  \"bricks\": " + std::to_string(options_.bricks) + ",\n";
        out += "  \"bodies\": " + std::to_string(options_.bodies) + ",\n";
        out += "  \"frames\": " + std::to_string(options_.frames) + ",\n";
        out += "  \"warmup\": " + std::to_string(options_.warmup) + ",\n";
        out += "  \"metrics\": {\n";
        out += json_metric("frame_total", total, ",\n");
        out += json_metric("frame_update", update, ",\n");
        out += json_metric("frame_render", render, "\n");
        out += "  }\n}\n";
        return out;
    }

    static std::string json_metric(const char* name, const Stats& stats, const char* tail) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "    \"%s\": {\"p50_ms\": %.4f, \"p95_ms\": %.4f, \"p99_ms\": %.4f, \"mean_ms\": %.4f, "
                      "\"min_ms\": %.4f, \"max_ms\": %.4f, \"samples\": %zu}%s",
                      name, stats.p50, stats.p95, stats.p99, stats.mean, stats.min, stats.max, stats.samples, tail);
        return buf;
    }

    Options options_;
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    // First pass: pick up --scene so its preset sets the baseline counts.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--scene" && i + 1 < argc) {
            options.scene = argv[i + 1];
        }
    }
    apply_preset(options);

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--scene" && i + 1 < argc) {
            ++i; // already consumed in the first pass
        } else if (arg == "--sprites" && i + 1 < argc) {
            options.sprites = static_cast<fe::u32>(std::stoul(argv[++i]));
        } else if (arg == "--bricks" && i + 1 < argc) {
            options.bricks = static_cast<fe::u32>(std::stoul(argv[++i]));
        } else if (arg == "--bodies" && i + 1 < argc) {
            options.bodies = static_cast<fe::u32>(std::stoul(argv[++i]));
        } else if (arg == "--frames" && i + 1 < argc) {
            options.frames = static_cast<fe::u32>(std::stoul(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            options.warmup = static_cast<fe::u32>(std::stoul(argv[++i]));
        } else if (arg == "--workers" && i + 1 < argc) {
            options.workers = static_cast<fe::u32>(std::stoul(argv[++i]));
        } else if (arg == "--textured") {
            options.textured = true;
        } else if (arg == "--json") {
            options.json = true;
        } else if (arg == "--out" && i + 1 < argc) {
            options.out = argv[++i];
        } else if (arg == "--verbose") {
            options.log_level = fe::LogLevel::Debug;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: fork_bench [--scene NAME] [--sprites N] [--bricks N] [--bodies N] "
                      "[--frames N] [--warmup N] [--workers N] [--textured] [--json] [--out PATH] [--verbose]\n"
                      "scenes: sprite_heavy (default), physics_heavy, mixed, mesh_heavy\n"
                      "        2D scenes: sprite_heavy, physics_heavy, mixed (--sprites are sprites)\n"
                      "        3D scene:  mesh_heavy (--sprites are meshes drawn by renderer3d)\n"
                      "--workers N  run extraction/culling on N threads (0 = serial, the default)\n"
                      "--textured   give sprites a real texture, so extraction has instances to build");
            std::exit(0);
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parse_options(argc, argv);

    fe::Engine::Config config;
    config.window.title = "Fork Engine Benchmark";
    config.window.width = 1280;
    config.window.height = 720;
    config.window.visible = false;
    config.window.vsync = false; // never let vsync clamp the measurement
    config.log_level = options.log_level;

    // Skip cleanly when there is no display or GL backend, exactly as the
    // graphics tests do, so `ctest` stays green on a machine without one. A
    // failure *after* the context exists is a real error and still propagates.
    {
        fe::WindowDesc probe_desc;
        probe_desc.title = "fork engine benchmark probe";
        probe_desc.width = 64;
        probe_desc.height = 64;
        probe_desc.visible = false;
        probe_desc.vsync = false;
        auto probe = fe::Window::create(probe_desc);
        if (!probe) {
            std::fputs("# fork_engine benchmark skipped: no graphics context\n", stdout);
            FE_LOG_WARN("benchmark skipped: %s", probe.error().message().c_str());
            return 0;
        }
    }

    BenchmarkApp app(config, options);
    app.set_worker_count(options.workers);
    app.set_headless(options.frames);
    return app.run();
}
