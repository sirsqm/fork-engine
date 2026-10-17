// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/renderer2d/Renderer2D.h"

#include "fe/core/Assert.h"
#include "fe/core/Log.h"
#include "fe/scene/Camera2D.h"
#include "fe/scene/Scene.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fe::renderer2d {
namespace {

// GLSL 3.30 core. The vertex shader transforms a sprite's local quad into clip
// space; the fragment shader samples the texture and multiplies by the tint.
//
// Positions arrive in world space (the CPU already applied the sprite's world
// matrix), so the vertex shader only applies the view-projection.
constexpr const char* kVertexShaderSource = R"glsl(
#version 330 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_view_projection;

out vec4 v_color;
out vec2 v_uv;

void main() {
    v_color = a_color;
    v_uv = a_uv;
    gl_Position = u_view_projection * vec4(a_position, 0.0, 1.0);
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

    // Start with capacity for 1024 sprites; grow on demand.
    constexpr u32 kInitialVertices = 1024 * 4;
    constexpr u32 kInitialIndices = 1024 * 6;
    const u32 vertex_bytes = kInitialVertices * static_cast<u32>(sizeof(Vertex));

    auto vertex_buffer = device.create_vertex_buffer(rhi::VertexLayout::position_color_uv(), vertex_bytes);
    if (!vertex_buffer) {
        return vertex_buffer.error();
    }
    vertex_buffer_ = vertex_buffer.value();

    auto index_buffer = device.create_index_buffer(kInitialIndices);
    if (!index_buffer) {
        return index_buffer.error();
    }
    index_buffer_ = index_buffer.value();

    vertex_capacity_ = kInitialVertices;
    index_capacity_ = kInitialIndices;

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

void Renderer2D::ensure_capacity(rhi::Device& device, u32 vertex_count, u32 index_count) {
    if (vertex_count <= vertex_capacity_ && index_count <= index_capacity_) {
        return;
    }

    const u32 new_vertex_capacity = std::max(vertex_count, vertex_capacity_ * 2);
    const u32 new_index_capacity = std::max(index_count, index_capacity_ * 2);

    if (vertex_count > vertex_capacity_) {
        auto buffer = device.create_vertex_buffer(rhi::VertexLayout::position_color_uv(),
                                                  new_vertex_capacity * static_cast<u32>(sizeof(Vertex)));
        if (buffer) {
            vertex_buffer_ = buffer.value();
            vertex_capacity_ = new_vertex_capacity;
        } else {
            FE_LOG_ERROR("renderer2d: could not grow vertex buffer: %s", buffer.error().message().c_str());
        }
    }

    if (index_count > index_capacity_) {
        auto buffer = device.create_index_buffer(new_index_capacity);
        if (buffer) {
            index_buffer_ = buffer.value();
            index_capacity_ = new_index_capacity;
        } else {
            FE_LOG_ERROR("renderer2d: could not grow index buffer: %s", buffer.error().message().c_str());
        }
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

void Renderer2D::append_quad(const render::SpriteInstance& instance) {
    const u32 base = static_cast<u32>(vertices_.size());

    // Local quad corners, centred on the origin. Corners go counter-clockwise.
    const Vec2 half = instance.size * 0.5f;
    const std::array<Vec2, 4> corners = {
        Vec2{-half.x, -half.y},
        Vec2{half.x, -half.y},
        Vec2{half.x, half.y},
        Vec2{-half.x, half.y},
    };
    const std::array<Vec2, 4> uvs = {
        Vec2{instance.uv.x, instance.uv.w},
        Vec2{instance.uv.z, instance.uv.w},
        Vec2{instance.uv.z, instance.uv.y},
        Vec2{instance.uv.x, instance.uv.y},
    };

    for (usize i = 0; i < 4; ++i) {
        const Vec3 world_pos = transform_point(instance.world, Vec3{corners[i].x, corners[i].y, 0.0f});
        vertices_.push_back(Vertex{Vec2{world_pos.x, world_pos.y}, instance.color, uvs[i]});
    }

    indices_.push_back(base + 0);
    indices_.push_back(base + 1);
    indices_.push_back(base + 2);
    indices_.push_back(base + 2);
    indices_.push_back(base + 3);
    indices_.push_back(base + 0);
}

void Renderer2D::extract(const Scene& scene, std::vector<render::SpriteInstance>& out) const {
    // Sprites and tiles share one array and one draw path: a tile is a textured
    // quad, so the only difference is where its world matrix and UV come from.
    // Filling one array is also what lets the layer sort order a map and the
    // sprites over it together.
    render::extract_sprites(scene, out);
    render::extract_tiles(scene, out);
}

usize Renderer2D::cull(std::vector<render::SpriteInstance>& out, const Frustum* frustum) {
    return render::cull_sprites(out, frustum);
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

Result<usize> Renderer2D::render(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    // --- 1. Extract --------------------------------------------------------
    std::vector<render::SpriteInstance> instances;
    extract(scene, instances);

    // --- 2. Cull -----------------------------------------------------------
    // A sprite outside the view cannot contribute a pixel, so it never needs a
    // quad, an index, or a draw. The test is conservative, so this can only save
    // work. The frustum is built from the same matrix the draw uses, so the two
    // steps can never disagree about what is on screen.
    const Frustum frustum =
        Frustum::from_view_projection(sprite_view_projection(scene, viewport_width, viewport_height));
    last_culled_ = static_cast<u32>(cull(instances, &frustum));

    // --- 3. Draw -----------------------------------------------------------
    return draw(device, scene, viewport_width, viewport_height, instances);
}

Result<usize> Renderer2D::draw(rhi::Device& device, const Scene& scene, u32 viewport_width, u32 viewport_height,
                               std::vector<render::SpriteInstance>& instances) {
    FE_CHECK(shader_ != nullptr);
    FE_CHECK(vertex_buffer_ != nullptr);
    FE_CHECK(index_buffer_ != nullptr);
    FE_CHECK(viewport_width > 0 && viewport_height > 0);

    // Sort into batches. The instance array is the sort unit; nothing here
    // touches the scene.
    std::sort(instances.begin(), instances.end(), draw_order_less);

    vertices_.clear();
    indices_.clear();
    batches_.clear();

    rhi::Texture* current_texture = nullptr;
    for (const render::SpriteInstance& instance : instances) {
        if (batches_.empty() || current_texture != instance.texture) {
            batches_.push_back(Batch{instance.texture, static_cast<u32>(indices_.size()), 0});
            current_texture = instance.texture;
        }
        append_quad(instance);
        batches_.back().index_count += 6;
    }

    last_draw_calls_ = 0;
    const usize sprite_count = instances.size();
    if (vertices_.empty()) {
        return sprite_count;
    }

    ensure_capacity(device, static_cast<u32>(vertices_.size()), static_cast<u32>(indices_.size()));

    // --- Upload and draw ---------------------------------------------------
    vertex_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(vertices_.data()), vertices_.size() * sizeof(Vertex)));
    index_buffer_->update(
        std::span<const u8>(reinterpret_cast<const u8*>(indices_.data()), indices_.size() * sizeof(u32)));

    device.set_blend_mode(rhi::BlendMode::Alpha);
    device.set_depth_test(false);

    const Mat4 view_projection = sprite_view_projection(scene, viewport_width, viewport_height);

    device.bind_shader(*shader_);
    shader_->set_mat4("u_view_projection", view_projection);
    shader_->set_int("u_texture", 0);
    device.bind_vertex_buffer(*vertex_buffer_);
    device.bind_index_buffer(*index_buffer_);

    for (const Batch& batch : batches_) {
        device.bind_texture(0, *batch.texture);
        device.draw_indexed(batch.index_count);
        ++last_draw_calls_;
    }

    return sprite_count;
}

} // namespace fe::renderer2d
