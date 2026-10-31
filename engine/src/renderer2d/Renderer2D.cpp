// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/renderer2d/Renderer2D.h"

#include "fe/core/Assert.h"
#include "fe/core/FrameArena.h"
#include "fe/core/Log.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Scene.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fe::renderer2d {
namespace {

// GLSL 3.30 core. The vertex shader places one instanced quad. The per-vertex
// input is a unit-square corner; the per-instance inputs are the sprite's world
// transform (columns 0 and 1 already scaled by size, plus the translation), its
// tint, and its uv rectangle. Position is assembled on the GPU from those, which
// is the whole point of instancing: the CPU no longer transforms four corners
// per sprite.
//
// The world transform is rebuilt from the three columns rather than uploaded as
// a mat4: a 2D sprite never needs the third column, so this is one vec4 less per
// instance. `u_view_projection` is applied last, as before.
constexpr const char* kVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec2 a_corner;

layout(location = 1) in vec4 a_world_x;
layout(location = 2) in vec4 a_world_y;
layout(location = 3) in vec4 a_world_z;
layout(location = 4) in vec4 a_color;
layout(location = 5) in vec4 a_uv;

uniform mat4 u_view_projection;

out vec4 v_color;
out vec2 v_uv;

void main() {
    // Column-major: the sprite's world position is x*col0 + y*col1 + col3. The
    // corner is in [-0.5, 0.5] and the columns carry the size, so this reproduces
    // the per-vertex transform the CPU used to do.
    vec4 world = a_world_x * a_corner.x + a_world_y * a_corner.y + a_world_z;
    v_color = a_color;
    // a_uv is (u0, v0, u1, v1) with v0 the top row. The corner's unit position
    // selects within it, and v is flipped so the bottom-left corner maps to v1,
    // matching the per-vertex uv assignment the CPU used to make.
    vec2 unit = a_corner + vec2(0.5, 0.5);
    v_uv = vec2(mix(a_uv.x, a_uv.z, unit.x), mix(a_uv.w, a_uv.y, unit.y));
    gl_Position = u_view_projection * vec4(world.xy, 0.0, 1.0);
}
)glsl";

constexpr const char* kFragmentShaderSource = R"glsl(
#version 330 core

in vec4 v_color;
in vec2 v_uv;

uniform sampler2D u_texture;

out vec4 frag_color;

void main() {
    frag_color = texture(u_texture, v_uv) * v_color;
}
)glsl";

// Draw-order key. Sorting by (layer, texture) groups equal textures together,
// which is what turns many sprites into few draw calls. Layer is placed first
// so that artist intent (what is in front) always wins over batching: a sprite
// on a higher layer is never reordered behind a lower one to save a draw call.
[[nodiscard]] bool draw_order_less(const render::SpriteInstance& a, const render::SpriteInstance& b) noexcept {
    if (a.layer != b.layer) {
        return a.layer < b.layer;
    }
    return a.texture < b.texture;
}

// The shared quad: four unit-square corners, drawn once and instanced. The
// corners go counter-clockwise so the two triangles wind consistently with the
// old per-sprite quads. UVs are unit-square too; the shader maps them into the
// instance's uv rectangle, so one quad serves every sprite's sub-rectangle.
constexpr std::array<Vec2, 4> kQuadCorners = {
    Vec2{-0.5f, -0.5f},
    Vec2{0.5f, -0.5f},
    Vec2{0.5f, 0.5f},
    Vec2{-0.5f, 0.5f},
};
constexpr std::array<u32, 6> kQuadIndices = {0, 1, 2, 2, 3, 0};

} // namespace

Renderer2D::Renderer2D() = default;

Renderer2D::~Renderer2D() = default;

Error Renderer2D::initialize(rhi::Device& device) {
    rhi::ShaderDesc desc;
    desc.name = "sprite2d";
    desc.vertex_source = kVertexShaderSource;
    desc.fragment_source = kFragmentShaderSource;

    auto shader = device.create_shader(desc);
    if (!shader) {
        return shader.error();
    }
    shader_ = shader.value();

    // The shared quad. Four vertices and six indices, uploaded once; instancing
    // means they never grow with the sprite count. The vertex buffer is created
    // with exactly the quad's size, so vertex_capacity() is a constant four.
    auto vertex_buffer =
        device.create_vertex_buffer(rhi::VertexLayout::sprite_quad(), static_cast<u32>(sizeof(Vertex) * 4));
    if (!vertex_buffer) {
        return vertex_buffer.error();
    }
    vertex_buffer_ = vertex_buffer.value();
    vertex_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(kQuadCorners.data()), kQuadCorners.size() * sizeof(Vec2)));
    vertex_capacity_ = 4;

    auto index_buffer = device.create_index_buffer(static_cast<u32>(kQuadIndices.size()));
    if (!index_buffer) {
        return index_buffer.error();
    }
    index_buffer_ = index_buffer.value();
    index_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(kQuadIndices.data()), kQuadIndices.size() * sizeof(u32)));
    index_capacity_ = static_cast<u32>(kQuadIndices.size());

    // Start with capacity for 1024 instances; grow on demand.
    constexpr u32 kInitialInstances = 1024;
    auto instance_buffer = device.create_vertex_buffer(rhi::VertexLayout::sprite_instance(),
                                                       kInitialInstances * static_cast<u32>(sizeof(Instance)));
    if (!instance_buffer) {
        return instance_buffer.error();
    }
    instance_buffer_ = instance_buffer.value();
    instance_capacity_ = kInitialInstances;

    // 1x1 opaque white, used for untextured sprites.
    const std::array<u8, 4> white = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.name = "white1x1";
    white_desc.width = 1;
    white_desc.height = 1;
    white_desc.pixels = white.data();
    white_desc.min_filter = rhi::TextureFilter::Nearest;
    white_desc.mag_filter = rhi::TextureFilter::Nearest;
    auto white_texture = device.create_texture(white_desc);
    if (!white_texture) {
        return white_texture.error();
    }
    white_texture_ = white_texture.value();

    return Error::none();
}

void Renderer2D::ensure_capacity(rhi::Device& device, u32 instance_count) {
    if (instance_count <= instance_capacity_) {
        return;
    }

    const u32 new_capacity = std::max(instance_count, instance_capacity_ * 2);
    auto buffer = device.create_vertex_buffer(rhi::VertexLayout::sprite_instance(),
                                              new_capacity * static_cast<u32>(sizeof(Instance)));
    if (buffer) {
        instance_buffer_ = buffer.value();
        instance_capacity_ = new_capacity;
    } else {
        FE_LOG_ERROR("renderer2d: could not grow instance buffer: %s", buffer.error().message().c_str());
    }
}

const Camera2D* Renderer2D::find_active_camera(const Scene& scene) {
    const Camera2D* fallback = nullptr;
    const Camera2D* found = nullptr;
    scene.for_each_node([&](Node& node) {
        if (const auto* camera = node.get_component<Camera2D>(); camera != nullptr) {
            if (fallback == nullptr) {
                fallback = camera;
            }
            if (camera->primary && found == nullptr) {
                found = camera;
            }
        }
    });
    return found != nullptr ? found : fallback;
}

Camera2D* Renderer2D::find_active_camera(Scene& scene) {
    // Same rule as the const overload, expressed once. `const_cast` on the
    // result is safe because the lookup only reads the scene to locate a node;
    // it never mutates anything through the const path.
    return const_cast<Camera2D*>(find_active_camera(static_cast<const Scene&>(scene)));
}

void Renderer2D::append_instance(const render::SpriteInstance& instance) {
    Instance record{};
    // Column 0 and column 1 carry the sprite's size: the corner is a unit square,
    // so folding size in here lets the shader scale without a separate uniform.
    const Vec4 col0{instance.world.at(0, 0), instance.world.at(1, 0), instance.world.at(2, 0), instance.world.at(3, 0)};
    const Vec4 col1{instance.world.at(0, 1), instance.world.at(1, 1), instance.world.at(2, 1), instance.world.at(3, 1)};
    const Vec4 col3{instance.world.at(0, 3), instance.world.at(1, 3), instance.world.at(2, 3), instance.world.at(3, 3)};
    record.world_x =
        Vec4{col0.x * instance.size.x, col0.y * instance.size.x, col0.z * instance.size.x, col0.w * instance.size.x};
    record.world_y =
        Vec4{col1.x * instance.size.y, col1.y * instance.size.y, col1.z * instance.size.y, col1.w * instance.size.y};
    record.world_z = col3;
    record.color = instance.color;
    record.uv = instance.uv;
    instances_.push_back(record);
}

void Renderer2D::extract(const Scene& scene, std::vector<render::SpriteInstance>& out, JobSystem* jobs) const {
    // Sprites and tiles share one array and one draw path: a tile is a textured
    // quad, so the only difference is where its world matrix and UV come from.
    // Filling one array is also what lets the layer sort order a map and the
    // sprites over it together.
    render::extract_sprites(scene, out, jobs);
    render::extract_tiles(scene, out, jobs);
    render::extract_text(scene, out, jobs);
}

usize Renderer2D::cull(std::vector<render::SpriteInstance>& out, const Frustum* frustum, JobSystem* jobs) {
    return render::cull_sprites(out, frustum, jobs);
}

// The view-projection the pass draws with: the active camera's, or a pixel-space
// fallback with the origin at the top-left when the scene has no camera. Also the
// matrix a caller derives the culling frustum from, so culling and drawing agree.
[[nodiscard]] static Mat4 sprite_view_projection(const Scene& scene, u32 viewport_width, u32 viewport_height) {
    const Camera2D* camera = Renderer2D::find_active_camera(scene);
    if (camera != nullptr) {
        return camera->view_projection(viewport_width, viewport_height);
    }
    return Mat4::orthographic(0.0f, static_cast<f32>(viewport_width), static_cast<f32>(viewport_height), 0.0f, -1.0f,
                              1.0f);
}

// The view-projection the UI pass draws with: a fixed pixel-space box with the
// origin at the *top-left* and +Y down, which is the space UI layout produces
// (ADR-0084). It deliberately ignores the scene's camera: a HUD pinned to the
// screen must not move when the camera pans or zoom, and that is the entire point
// of a separate pass.
//
// The `bottom`/`top` arguments are swapped relative to a world camera so that +Y
// in the instance's position points down the screen. With the box (0, width) x
// (height, 0), `Mat4::orthographic` maps y = 0 to the top of the clip space and
// y = height to the bottom, which is exactly the convention `ui::Rect` uses.
[[nodiscard]] static Mat4 ui_view_projection(u32 viewport_width, u32 viewport_height) {
    return Mat4::orthographic(0.0f, static_cast<f32>(viewport_width), static_cast<f32>(viewport_height), 0.0f, -1.0f,
                              1.0f);
}

Result<usize> Renderer2D::render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                 JobSystem* jobs) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    // --- 1. Extract --------------------------------------------------------
    std::vector<render::SpriteInstance> instances;
    extract(scene, instances, jobs);

    // --- 2. Cull -----------------------------------------------------------
    // A sprite outside the view cannot contribute a pixel, so it never needs a
    // quad, an index, or a draw. The test is conservative, so this can only save
    // work. The frustum is built from the same matrix the draw uses, so the two
    // steps can never disagree about what is on screen.
    const Frustum frustum =
        Frustum::from_view_projection(sprite_view_projection(scene, viewport_width, viewport_height));
    last_culled_ = static_cast<u32>(cull(instances, &frustum, jobs));

    // --- 3. Draw -----------------------------------------------------------
    return draw(device, scene, viewport_width, viewport_height, instances);
}

Result<usize> Renderer2D::render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                 FrameArena& arena, JobSystem* jobs) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    // Size the destination from a bound that can never be short, then let the
    // extraction fill it. The bound is a walk of the scene and allocates nothing.
    const usize bound = render::sprite_instance_bound(scene);
    if (bound == 0) {
        last_culled_ = 0;
        return draw(device, scene, viewport_width, viewport_height, {});
    }

    auto* storage = static_cast<render::SpriteInstance*>(
        arena.allocate(sizeof(render::SpriteInstance) * bound, alignof(render::SpriteInstance)));
    if (storage == nullptr) {
        // The arena cannot hold this frame's extraction. Fall back to the heap
        // path: identical output, only the allocation differs. This is the
        // documented overflow behaviour (ADR-0051/0054), not an error.
        return render(device, scene, viewport_width, viewport_height, jobs);
    }

    std::span<render::SpriteInstance> instances(storage, bound);
    usize used = render::extract_sprites_into(arena, scene, instances, jobs);
    used = render::extract_tiles_into(arena, scene, instances, used, jobs);
    used = render::extract_text_into(arena, scene, instances, used, jobs);

    const Frustum frustum =
        Frustum::from_view_projection(sprite_view_projection(scene, viewport_width, viewport_height));
    const usize before = used;
    used = render::cull_sprites_in_place(arena, instances, used, &frustum, jobs);
    last_culled_ = static_cast<u32>(before - used);

    return draw(device, scene, viewport_width, viewport_height, instances.first(used));
}

// Builds the instance stream and issues the draws for one pass. Shared by the
// world pass and the UI pass so their batching, upload, and state setup cannot
// drift; the only difference between the two is the matrix they draw with and how
// the instances are ordered.
//
// `sort_by_texture` selects the ordering. The world pass sets it: sorting by
// (layer, texture) groups equal textures into one draw call, and layer is the
// primary key so an artist's intent always wins over batching. The UI pass clears
// it: UI instances arrive in draw order from `ui::layout`, and re-sorting them
// would move a child behind its parent. A stable sort by layer alone keeps the
// insertion order within a layer, so UI batching is by contiguous texture runs in
// draw order -- the same batching benefit, without the reordering.
//
// A null texture is drawn with the renderer's 1x1 white texture, so an untextured
// instance (a UI fill, an untextured sprite) is a solid coloured quad rather than
// a batch that binds no texture. This is what `white_texture_` exists for.
Result<usize> Renderer2D::submit(rhi::Device& device, std::span<render::SpriteInstance> instances,
                                 const Mat4& view_projection, bool sort_by_texture) {
    if (sort_by_texture) {
        std::sort(instances.begin(), instances.end(), draw_order_less);
    } else {
        std::stable_sort(instances.begin(), instances.end(),
                         [](const render::SpriteInstance& a, const render::SpriteInstance& b) noexcept {
                             return a.layer < b.layer;
                         });
    }

    instances_.clear();
    batches_.clear();

    rhi::Texture* current_texture = nullptr;
    for (const render::SpriteInstance& instance : instances) {
        rhi::Texture* texture = instance.texture != nullptr ? instance.texture : white_texture_.get();
        if (batches_.empty() || current_texture != texture) {
            batches_.push_back(Batch{texture, static_cast<u32>(instances_.size()), 0});
            current_texture = texture;
        }
        append_instance(instance);
        batches_.back().instance_count += 1;
    }

    last_draw_calls_ = 0;
    const usize quad_count = instances.size();
    if (instances_.empty()) {
        return quad_count;
    }

    ensure_capacity(device, static_cast<u32>(instances_.size()));

    // --- Upload and draw ---------------------------------------------------
    // Only the instance buffer is re-uploaded per frame; the quad is static.
    instance_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(instances_.data()), instances_.size() * sizeof(Instance)));

    device.set_blend_mode(rhi::BlendMode::Alpha);
    device.set_depth_test(false);

    device.bind_shader(*shader_);
    shader_->set_mat4("u_view_projection", view_projection);
    shader_->set_int("u_texture", 0);
    // The per-vertex buffer is bound first: bind_vertex_buffer disables attributes
    // the previous pass left enabled (the per-instance ones), so it must run before
    // the instance buffer re-enables them. The instance buffer is then bound per
    // batch, at that batch's offset into the one shared upload.
    device.bind_vertex_buffer(*vertex_buffer_);
    device.bind_index_buffer(*index_buffer_);

    for (const Batch& batch : batches_) {
        device.bind_instance_buffer(*instance_buffer_, batch.instance_offset * static_cast<u32>(sizeof(Instance)));
        device.bind_texture(0, *batch.texture);
        // Every batch shares the same six indices; only the instance range and
        // the texture differ.
        device.draw_indexed_instanced(static_cast<u32>(kQuadIndices.size()), batch.instance_count);
        ++last_draw_calls_;
    }

    return quad_count;
}

Result<usize> Renderer2D::draw(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                               std::span<render::SpriteInstance> instances) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    return submit(device, instances, sprite_view_projection(scene, viewport_width, viewport_height),
                  /*sort_by_texture=*/true);
}

Result<usize> Renderer2D::render_ui(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                    JobSystem* jobs) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    std::vector<render::SpriteInstance> instances;
    render::extract_ui(scene, viewport_width, viewport_height, instances, jobs);
    // No culling: the projection is the viewport itself, so every UI quad is on
    // screen by construction. A UI quad outside the viewport is a layout bug, and
    // dropping it would hide that bug rather than show it.
    return submit(device, instances, ui_view_projection(viewport_width, viewport_height), /*sort_by_texture=*/false);
}

Result<usize> Renderer2D::render_ui(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                                    FrameArena& arena, JobSystem* jobs) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    const usize bound = render::ui_instance_bound(scene);
    if (bound == 0) {
        return submit(device, {}, ui_view_projection(viewport_width, viewport_height), /*sort_by_texture=*/false);
    }

    auto* storage = static_cast<render::SpriteInstance*>(
        arena.allocate(sizeof(render::SpriteInstance) * bound, alignof(render::SpriteInstance)));
    if (storage == nullptr) {
        // The arena cannot hold this frame's UI extraction. Fall back to the heap
        // path: identical output, only the allocation differs (ADR-0051/0054).
        return render_ui(device, scene, viewport_width, viewport_height, jobs);
    }

    std::span<render::SpriteInstance> instances(storage, bound);
    const usize used = render::extract_ui_into(arena, scene, viewport_width, viewport_height, instances, 0, jobs);
    return submit(device, instances.first(used), ui_view_projection(viewport_width, viewport_height),
                  /*sort_by_texture=*/false);
}

Error Renderer2D::render_frame(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                               usize& world_drawn, usize& ui_quads_drawn, FrameArena* arena, JobSystem* jobs) {
    // The frame's two passes as a graph (ADR-0100): the UI is declared to run
    // after the world, so the ordering the class used to express by calling
    // render() and then render_ui() is now data. The graph is rebuilt per call
    // because it is cheap (two names) and a long-lived one would have to be
    // guarded against concurrent frames for no gain.
    render::RenderGraph graph;
    if (Result<void> added = graph.add_pass("world"); !added) {
        return added.error();
    }
    if (Result<void> added = graph.add_pass("ui"); !added) {
        return added.error();
    }
    if (Result<void> added = graph.add_dependency("ui", "world"); !added) {
        return added.error();
    }
    // Declare the flow so the graph can reason about it (ADR-0110): the world
    // draws into the backbuffer, the UI draws over it, and the swapchain is what
    // the frame is observed through. Both passes stay live -- the world's write
    // is read by the UI, and the UI writes the marked output -- which is exactly
    // the order the dependency already expressed. The declarations cost nothing
    // at run time and make the seam able to cull a pass whose output nobody
    // reads as the renderer grows a chain of intermediate targets.
    if (Result<void> added = graph.add_write("world", "backbuffer"); !added) {
        return added.error();
    }
    if (Result<void> added = graph.add_read("ui", "backbuffer"); !added) {
        return added.error();
    }
    if (Result<void> added = graph.add_write("ui", "swapchain"); !added) {
        return added.error();
    }
    if (Result<void> added = graph.mark_output("swapchain"); !added) {
        return added.error();
    }

    world_drawn = 0;
    ui_quads_drawn = 0;
    return graph.run([&](std::string_view pass) -> Error {
        if (pass == "world") {
            Result<usize> drawn = arena != nullptr
                                      ? render(device, scene, viewport_width, viewport_height, *arena, jobs)
                                      : render(device, scene, viewport_width, viewport_height, jobs);
            if (!drawn) {
                return drawn.error();
            }
            world_drawn = drawn.value();
            return Error::none();
        }
        // "ui": the graph guarantees this is the only other pass.
        Result<usize> drawn = arena != nullptr ? render_ui(device, scene, viewport_width, viewport_height, *arena, jobs)
                                               : render_ui(device, scene, viewport_width, viewport_height, jobs);
        if (!drawn) {
            return drawn.error();
        }
        ui_quads_drawn = drawn.value();
        return Error::none();
    });
}

} // namespace fe::renderer2d
