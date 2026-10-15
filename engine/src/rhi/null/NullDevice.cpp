// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/rhi/null/NullDevice.h"

#include <memory>
#include <string>
#include <string_view>

namespace fe::rhi::null {
namespace {

// ---------------------------------------------------------------------------
// Inert resources
//
// Every resource below satisfies the RHI interface so the renderers above it
// run unmodified, but holds no GPU state. The values a caller can observe
// (names, sizes, element counts) are the real ones the caller asked for, so a
// test or a headless host can still reason about what *would* have been drawn.
// ---------------------------------------------------------------------------

class NullShader final : public Shader {
public:
    explicit NullShader(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

    void set_mat4(std::string_view, const Mat4&) override {}
    void set_mat3(std::string_view, const f32*) override {}
    void set_vec4(std::string_view, const Vec4&) override {}
    void set_vec3(std::string_view, const Vec3&) override {}
    void set_int(std::string_view, i32) override {}
    void set_float(std::string_view, f32) override {}
    void set_vec4_array(std::string_view, const Vec4*, u32) override {}

private:
    std::string name_;
};

class NullBuffer final : public Buffer {
public:
    NullBuffer(Kind kind, u32 element_count) : kind_(kind), element_count_(element_count) {}

    [[nodiscard]] Kind kind() const noexcept override { return kind_; }
    [[nodiscard]] u32 element_count() const noexcept override { return element_count_; }

    // Accepts the update and discards it: there is no storage to fill.
    void update(std::span<const u8>) override {}

private:
    Kind kind_;
    u32 element_count_;
};

class NullTexture final : public Texture {
public:
    NullTexture(std::string name, u32 width, u32 height) : name_(std::move(name)), width_(width), height_(height) {}

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }
    [[nodiscard]] u32 width() const noexcept override { return width_; }
    [[nodiscard]] u32 height() const noexcept override { return height_; }

    // No backend identifier exists for an inert texture. A consumer that needs
    // one (the editor's ImGui blit) cannot run headless anyway; returning 0 is
    // the honest "there is no GPU object here".
    [[nodiscard]] u64 native_handle() const noexcept override { return 0; }

private:
    std::string name_;
    u32 width_;
    u32 height_;
};

class NullRenderTarget final : public RenderTarget {
public:
    NullRenderTarget(std::string name, u32 width, u32 height, bool sample_depth)
        : name_(std::move(name)), width_(width), height_(height),
          color_(std::make_shared<NullTexture>(name_ + ".color", width, height)),
          depth_(sample_depth ? std::make_shared<NullTexture>(name_ + ".depth", width, height) : nullptr) {}

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }
    [[nodiscard]] u32 width() const noexcept override { return width_; }
    [[nodiscard]] u32 height() const noexcept override { return height_; }

    [[nodiscard]] TextureRef color_texture() const noexcept override { return color_; }
    [[nodiscard]] TextureRef depth_texture() const noexcept override { return depth_; }

    void bind() override {}
    void unbind() override {}
    void clear(const ClearColor&) override {}

private:
    std::string name_;
    u32 width_;
    u32 height_;
    std::shared_ptr<NullTexture> color_;
    std::shared_ptr<NullTexture> depth_;
};

class NullDevice final : public Device {
public:
    [[nodiscard]] const char* backend_name() const noexcept override { return "null"; }

    [[nodiscard]] std::string info() const override { return "null device (no rasterisation)"; }

    Result<ShaderRef> create_shader(const ShaderDesc& desc) override {
        return std::static_pointer_cast<Shader>(std::make_shared<NullShader>(desc.name));
    }

    Result<BufferRef> create_vertex_buffer(const VertexLayout& layout, u32 capacity_bytes) override {
        // Mirror the OpenGL backend's validation so a headless run rejects the
        // same malformed requests the interactive one does, rather than
        // silently accepting what a real device would refuse.
        if (capacity_bytes == 0 || layout.stride == 0) {
            return Error("create_vertex_buffer: capacity and stride must be non-zero");
        }
        const u32 vertex_count = capacity_bytes / layout.stride;
        return std::static_pointer_cast<Buffer>(std::make_shared<NullBuffer>(Buffer::Kind::Vertex, vertex_count));
    }

    Result<BufferRef> create_index_buffer(u32 capacity_indices) override {
        if (capacity_indices == 0) {
            return Error("create_index_buffer: capacity must be non-zero");
        }
        return std::static_pointer_cast<Buffer>(std::make_shared<NullBuffer>(Buffer::Kind::Index, capacity_indices));
    }

    Result<TextureRef> create_texture(const TextureDesc& desc) override {
        if (desc.width == 0 || desc.height == 0) {
            return Error("create_texture: dimensions must be non-zero");
        }
        if (desc.pixels == nullptr) {
            return Error("create_texture: pixel data must not be null");
        }
        return std::static_pointer_cast<Texture>(std::make_shared<NullTexture>(desc.name, desc.width, desc.height));
    }

    Result<RenderTargetRef> create_render_target(const RenderTargetDesc& desc) override {
        if (desc.width == 0 || desc.height == 0) {
            return Error("create_render_target: dimensions must be non-zero");
        }
        return std::static_pointer_cast<RenderTarget>(
            std::make_shared<NullRenderTarget>(desc.name, desc.width, desc.height, desc.depth && desc.sample_depth));
    }

    void begin_frame(u32, u32, const ClearColor&) override {
        // A frame always starts on the default target, whatever a viewport left
        // bound in the previous frame, so the bound-target query matches the
        // OpenGL backend's behaviour.
        bound_target_ = nullptr;
    }

    void end_frame() override {}

    void bind_render_target(RenderTarget* target) override { bound_target_ = target; }

    [[nodiscard]] RenderTarget* bound_render_target() const noexcept override { return bound_target_; }

    void set_viewport(u32, u32, u32, u32) override {}
    void set_blend_mode(BlendMode) override {}
    void set_depth_test(bool) override {}
    void set_primitive_topology(PrimitiveTopology) override {}
    void bind_shader(Shader&) override {}
    void bind_vertex_buffer(Buffer&) override {}
    void bind_index_buffer(Buffer&) override {}
    void bind_texture(u32, Texture&) override {}

    // The draw is counted nowhere: there is nothing to submit to. A caller that
    // needs to prove the render path ran reads the renderer's own counters
    // (Renderer2D::last_draw_calls(), Renderer3D::last_draw_calls()), which are
    // the same values an interactive run reports.
    void draw_indexed(u32) override {}

    // There is no framebuffer to read back. The output buffer is left untouched
    // on purpose: a caller must not mistake "no pixels" for "black pixels". A
    // test that needs pixel assertions belongs in the graphics suite, which
    // requires a real context.
    void read_pixels(u32, u32, u32, u32, std::span<u8>) override {}

private:
    RenderTarget* bound_target_ = nullptr;
};

} // namespace

Result<std::unique_ptr<Device>> create_null_device() {
    return std::unique_ptr<Device>(std::make_unique<NullDevice>());
}

} // namespace fe::rhi::null
