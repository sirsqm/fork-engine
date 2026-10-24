// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/rhi/opengl/OpenGLDevice.h"

#include "fe/core/Log.h"
#include "fe/platform/Window.h"

// The GL declarations come from a different place per platform. On desktop they
// come from glad, the vendored loader that resolves entry points against the
// driver through the window. On web there is no loader to vendor: the browser
// *is* the GL implementation, and Emscripten's own GLES3 header declares the
// same names with the same types, with the entry points supplied by its WebGL2
// binding. The device code below is identical either way; only the declarations
// and the "load the pointers" step differ. See
// docs/adr/0074-web-window-backend.md.
#if defined(FE_PLATFORM_WEB)
#include <GLES3/gl3.h>
#else
#include <glad/gl.h>
#endif

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fe::rhi::opengl {
namespace {

// ---------------------------------------------------------------------------
// Shader compilation helpers
// ---------------------------------------------------------------------------

[[nodiscard]] GLuint compile_stage(GLenum stage, std::string_view source, std::string& out_error) {
    const GLuint shader = glCreateShader(stage);
    const char* source_ptr = source.data();
    const GLint source_length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &source_ptr, &source_length);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint log_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        std::string log(static_cast<usize>(log_length > 0 ? log_length : 1), '\0');
        glGetShaderInfoLog(shader, log_length, nullptr, log.data());
        out_error = log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// ---------------------------------------------------------------------------
// Concrete resources
// ---------------------------------------------------------------------------

class OpenGLShader final : public Shader {
public:
    OpenGLShader(std::string name, GLuint program) : name_(std::move(name)), program_(program) {}

    ~OpenGLShader() override {
        if (program_ != 0) {
            glDeleteProgram(program_);
        }
    }

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

    void set_mat4(std::string_view name, const Mat4& value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniformMatrix4fv(location, 1, GL_FALSE, value.m);
        }
    }

    void set_mat3(std::string_view name, const f32* value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniformMatrix3fv(location, 1, GL_FALSE, value);
        }
    }

    void set_vec4(std::string_view name, const Vec4& value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniform4f(location, value.x, value.y, value.z, value.w);
        }
    }

    void set_vec3(std::string_view name, const Vec3& value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniform3f(location, value.x, value.y, value.z);
        }
    }

    void set_int(std::string_view name, i32 value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniform1i(location, value);
        }
    }

    void set_float(std::string_view name, f32 value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniform1f(location, value);
        }
    }

    void set_vec4_array(std::string_view name, const Vec4* values, u32 count) override {
        if (count == 0) {
            return;
        }
        // An array uniform resolves as either "name" or "name[0]" depending on
        // the driver and how the array is declared; try the plain name first,
        // then the indexed form.
        GLint location = location_of(name);
        if (location < 0) {
            location = location_of(std::string(name) + "[0]");
        }
        if (location >= 0) {
            glUniform4fv(location, static_cast<GLsizei>(count), &values[0].x);
        }
    }

    void bind() const { glUseProgram(program_); }

    [[nodiscard]] GLuint program() const noexcept { return program_; }

private:
    // Uniform locations are cached: glGetUniformLocation is not free, and the
    // renderer sets the same handful of uniforms for every draw.
    [[nodiscard]] GLint location_of(std::string_view name) {
        const std::string key(name);
        const auto it = locations_.find(key);
        if (it != locations_.end()) {
            return it->second;
        }
        const GLint location = glGetUniformLocation(program_, key.c_str());
        locations_.emplace(key, location);
        return location;
    }

    std::string name_;
    GLuint program_ = 0;
    std::unordered_map<std::string, GLint> locations_;
};

class OpenGLBuffer final : public Buffer {
public:
    OpenGLBuffer(Kind kind, GLenum target, u32 element_count, u32 element_size)
        : kind_(kind), target_(target), element_count_(element_count), element_size_(element_size) {
        glGenBuffers(1, &handle_);
        glBindBuffer(target_, handle_);
        glBufferData(target_, static_cast<GLsizeiptr>(element_count_ * element_size_), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(target_, 0);
    }

    ~OpenGLBuffer() override {
        if (handle_ != 0) {
            glDeleteBuffers(1, &handle_);
        }
    }

    [[nodiscard]] Kind kind() const noexcept override { return kind_; }
    [[nodiscard]] u32 element_count() const noexcept override { return element_count_; }
    [[nodiscard]] GLenum target() const noexcept { return target_; }
    [[nodiscard]] GLuint handle() const noexcept { return handle_; }

    void update(std::span<const u8> data) override {
        glBindBuffer(target_, handle_);
        // Orphan-and-refill: discard the previous storage so the driver never
        // stalls waiting for in-flight draws to finish reading it.
        glBufferData(target_, static_cast<GLsizeiptr>(element_count_ * element_size_), nullptr, GL_DYNAMIC_DRAW);
        if (!data.empty()) {
            glBufferSubData(target_, 0, static_cast<GLsizeiptr>(data.size()), data.data());
        }
        glBindBuffer(target_, 0);
    }

private:
    Kind kind_;
    GLenum target_;
    u32 element_count_;
    u32 element_size_;
    GLuint handle_ = 0;
};

class OpenGLTexture final : public Texture {
public:
    OpenGLTexture(std::string name, u32 width, u32 height) : name_(std::move(name)), width_(width), height_(height) {
        glGenTextures(1, &handle_);
    }

    ~OpenGLTexture() override {
        if (handle_ != 0) {
            glDeleteTextures(1, &handle_);
        }
    }

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }
    [[nodiscard]] u32 width() const noexcept override { return width_; }
    [[nodiscard]] u32 height() const noexcept override { return height_; }
    [[nodiscard]] GLuint handle() const noexcept { return handle_; }
    [[nodiscard]] u64 native_handle() const noexcept override { return static_cast<u64>(handle_); }

    // Allocates color storage without uploading pixels. Used for render-target
    // attachments, whose contents are produced by drawing.
    void allocate(u32 width, u32 height, TextureFilter filter) {
        glBindTexture(GL_TEXTURE_2D, handle_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        set_sampling(filter, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }

    // Allocates a depth-only texture. The shadow pass renders into it and then
    // samples it as a plain float depth, so compare mode stays NONE: the shader
    // does the comparison itself, which keeps it backend-agnostic.
    void allocate_depth(u32 width, u32 height) {
        glBindTexture(GL_TEXTURE_2D, handle_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                     0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        // Nearest: depth must not be interpolated across the shadow map, and a
        // shadow-map texel is a single depth value, not a gradient.
        set_sampling(TextureFilter::Nearest, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }

    void upload(const TextureDesc& desc) {
        glBindTexture(GL_TEXTURE_2D, handle_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, desc.pixels);
        set_sampling(desc.min_filter, desc.wrap_u, desc.wrap_v, desc.mag_filter);
        if (desc.generate_mipmaps) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

private:
    void set_sampling(TextureFilter min_filter, TextureWrap wrap_u, TextureWrap wrap_v,
                      TextureFilter mag_filter = TextureFilter::Linear) {
        const GLint gl_min = min_filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
        const GLint gl_mag = mag_filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_min);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_mag);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_u == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_v == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    std::string name_;
    u32 width_;
    u32 height_;
    GLuint handle_ = 0;
};

// ---------------------------------------------------------------------------
// Render target
// ---------------------------------------------------------------------------

// A framebuffer object with a color texture and, optionally, a depth
// attachment. The color texture is the attachment the editor viewport blits.
// The depth attachment is either a renderbuffer (cheap, not sampleable) or a
// texture (sampleable, for the shadow pass), chosen by `sample_depth`.
class OpenGLRenderTarget final : public RenderTarget {
public:
    OpenGLRenderTarget(std::string name, u32 width, u32 height, bool depth, bool sample_depth)
        : name_(std::move(name)), width_(width), height_(height), depth_(depth), sample_depth_(sample_depth) {}

    ~OpenGLRenderTarget() override {
        if (depth_buffer_ != 0) {
            glDeleteRenderbuffers(1, &depth_buffer_);
        }
        if (framebuffer_ != 0) {
            glDeleteFramebuffers(1, &framebuffer_);
        }
    }

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }
    [[nodiscard]] u32 width() const noexcept override { return width_; }
    [[nodiscard]] u32 height() const noexcept override { return height_; }
    [[nodiscard]] TextureRef color_texture() const noexcept override { return color_; }
    [[nodiscard]] TextureRef depth_texture() const noexcept override { return depth_texture_; }

    [[nodiscard]] GLuint framebuffer() const noexcept { return framebuffer_; }

    void bind() override {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_));
    }

    void unbind() override { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    void clear(const ClearColor& color) override {
        glClearColor(color.r, color.g, color.b, color.a);
        glClear(GL_COLOR_BUFFER_BIT | (depth_ ? GL_DEPTH_BUFFER_BIT : 0));
    }

    // Allocates the attachments. Separate from the constructor so creation can
    // fail with an Error instead of asserting: an incomplete framebuffer is a
    // driver or size problem the caller should see.
    [[nodiscard]] Error create_attachments() {
        color_ = std::make_shared<OpenGLTexture>(name_ + ".color", width_, height_);
        // Render targets are sampled at a different scale than they are drawn,
        // so linear filtering avoids a blocky downsample.
        color_->allocate(width_, height_, TextureFilter::Linear);

        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_->handle(), 0);

        if (depth_) {
            if (sample_depth_) {
                depth_texture_ = std::make_shared<OpenGLTexture>(name_ + ".depth", width_, height_);
                depth_texture_->allocate_depth(width_, height_);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_texture_->handle(), 0);
            } else {
                glGenRenderbuffers(1, &depth_buffer_);
                glBindRenderbuffer(GL_RENDERBUFFER, depth_buffer_);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(width_),
                                      static_cast<GLsizei>(height_));
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_buffer_);
            }
        }

        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            return Error("render target '" + name_ + "' is incomplete (GL status " + std::to_string(status) + ")");
        }
        return Error::none();
    }

private:
    std::string name_;
    u32 width_;
    u32 height_;
    bool depth_;
    bool sample_depth_;
    GLuint framebuffer_ = 0;
    GLuint depth_buffer_ = 0;
    std::shared_ptr<OpenGLTexture> color_;
    std::shared_ptr<OpenGLTexture> depth_texture_;
};

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

class OpenGLDevice final : public Device {
public:
    explicit OpenGLDevice(Window& window) : window_(&window) {
        // An OpenGL 3.3 core profile context has no default vertex array object.
        // Without one bound, glVertexAttribPointer and glDrawElements are
        // invalid and nothing is drawn, so the device owns a single VAO for the
        // whole frame. The 2D renderer binds one vertex buffer per batch and
        // reconfigures attributes on each bind, which is exactly what a single
        // VAO can express; a future multi-buffer renderer would manage a VAO
        // per layout behind the same interface.
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
    }

    ~OpenGLDevice() override {
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
        }
    }

    [[nodiscard]] const char* backend_name() const noexcept override { return "OpenGL 3.3"; }

    [[nodiscard]] std::string info() const override {
        const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        return std::string(version != nullptr ? version : "unknown") + " | " +
               std::string(renderer != nullptr ? renderer : "unknown");
    }

    // This backend creates a desktop OpenGL 3.3 core context (see
    // create_opengl_device), so it compiles the desktop dialect. The choice is
    // made here, once per device, and the translation is the identity, so the
    // bytes reaching the driver are the ones the shader was written with.
    [[nodiscard]] ShaderDialect shader_dialect() const noexcept override { return ShaderDialect::DesktopCore; }

    Result<ShaderRef> create_shader(const ShaderDesc& desc) override {
        // Translate at the seam: the caller supplied desktop sources and never
        // names a dialect, and the compiler sees what this device's dialect
        // requires. For the desktop dialect these are byte-for-byte copies.
        const std::string vertex_source = compiled_shader_source(ShaderStage::Vertex, desc.vertex_source);
        const std::string fragment_source = compiled_shader_source(ShaderStage::Fragment, desc.fragment_source);

        std::string error;
        const GLuint vertex = compile_stage(GL_VERTEX_SHADER, vertex_source, error);
        if (vertex == 0) {
            return Error("vertex shader '" + desc.name + "' failed to compile:\n" + error);
        }
        const GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, fragment_source, error);
        if (fragment == 0) {
            glDeleteShader(vertex);
            return Error("fragment shader '" + desc.name + "' failed to compile:\n" + error);
        }

        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);

        // The stages can be deleted as soon as the program is linked.
        glDetachShader(program, vertex);
        glDetachShader(program, fragment);
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint status = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status != GL_TRUE) {
            GLint log_length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
            std::string log(static_cast<usize>(log_length > 0 ? log_length : 1), '\0');
            glGetProgramInfoLog(program, log_length, nullptr, log.data());
            glDeleteProgram(program);
            return Error("shader program '" + desc.name + "' failed to link:\n" + log);
        }

        return std::static_pointer_cast<Shader>(std::make_shared<OpenGLShader>(desc.name, program));
    }

    Result<BufferRef> create_vertex_buffer(const VertexLayout& layout, u32 capacity_bytes) override {
        if (capacity_bytes == 0 || layout.stride == 0) {
            return Error("create_vertex_buffer: capacity and stride must be non-zero");
        }
        const u32 vertex_count = capacity_bytes / layout.stride;
        auto buffer =
            std::make_shared<OpenGLBuffer>(Buffer::Kind::Vertex, GL_ARRAY_BUFFER, vertex_count, layout.stride);
        // Remember the layout so bind_vertex_buffer can configure attributes.
        layouts_.emplace(buffer->handle(), layout);
        return std::static_pointer_cast<Buffer>(buffer);
    }

    Result<BufferRef> create_index_buffer(u32 capacity_indices) override {
        if (capacity_indices == 0) {
            return Error("create_index_buffer: capacity must be non-zero");
        }
        auto buffer = std::make_shared<OpenGLBuffer>(Buffer::Kind::Index, GL_ELEMENT_ARRAY_BUFFER, capacity_indices,
                                                     static_cast<u32>(sizeof(u32)));
        return std::static_pointer_cast<Buffer>(buffer);
    }

    Result<TextureRef> create_texture(const TextureDesc& desc) override {
        if (desc.width == 0 || desc.height == 0) {
            return Error("create_texture: dimensions must be non-zero");
        }
        if (desc.pixels == nullptr) {
            return Error("create_texture: pixel data must not be null");
        }
        auto texture = std::make_shared<OpenGLTexture>(desc.name, desc.width, desc.height);
        texture->upload(desc);
        return std::static_pointer_cast<Texture>(texture);
    }

    Result<RenderTargetRef> create_render_target(const RenderTargetDesc& desc) override {
        if (desc.width == 0 || desc.height == 0) {
            return Error("create_render_target: dimensions must be non-zero");
        }
        auto target = std::make_shared<OpenGLRenderTarget>(desc.name, desc.width, desc.height, desc.depth,
                                                           desc.depth && desc.sample_depth);
        if (Error error = target->create_attachments(); error) {
            return error;
        }
        return std::static_pointer_cast<RenderTarget>(target);
    }

    void begin_frame(u32 framebuffer_width, u32 framebuffer_height, const ClearColor& clear_color) override {
        // A frame always starts on the window's default framebuffer, whatever an
        // editor viewport left bound in the previous frame.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        bound_target_ = nullptr;
        default_framebuffer_width_ = framebuffer_width;
        default_framebuffer_height_ = framebuffer_height;
        set_viewport(0, 0, framebuffer_width, framebuffer_height);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        set_blend_mode(BlendMode::Alpha);
        glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void end_frame() override { window_->swap_buffers(); }

    void bind_render_target(RenderTarget* target) override {
        if (target == nullptr) {
            // Restore the window. Resetting the viewport to the window's size
            // matters: a caller that renders into a small panel target and then
            // back to the window must not inherit the panel's viewport, or the
            // window draw lands in a corner.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            bound_target_ = nullptr;
            set_viewport(0, 0, default_framebuffer_width_, default_framebuffer_height_);
            return;
        }
        target->bind();
        bound_target_ = target;
    }

    [[nodiscard]] RenderTarget* bound_render_target() const noexcept override { return bound_target_; }

    void set_viewport(u32 x, u32 y, u32 width, u32 height) override {
        glViewport(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height));
    }

    void set_blend_mode(BlendMode mode) override {
        switch (mode) {
        case BlendMode::None:
            glDisable(GL_BLEND);
            break;
        case BlendMode::Alpha:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case BlendMode::Additive:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        }
    }

    void set_depth_test(bool enabled) override {
        if (enabled) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    void set_primitive_topology(PrimitiveTopology topology) override { topology_ = topology; }

    void bind_shader(Shader& shader) override {
        auto& gl_shader = static_cast<OpenGLShader&>(shader);
        gl_shader.bind();
        bound_shader_ = gl_shader.program();
    }

    void bind_vertex_buffer(Buffer& buffer) override {
        auto& gl_buffer = static_cast<OpenGLBuffer&>(buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gl_buffer.handle());

        // Reconfigure the vertex attributes every bind. This is cheap (a few
        // glVertexAttribPointer calls) and avoids the VAO lifetime bookkeeping
        // that a multi-buffer renderer would need. The 2D renderer binds one
        // vertex buffer and one index buffer per batch, so the cost is
        // negligible; a future 3D renderer can introduce VAOs behind the same
        // interface.
        const auto it = layouts_.find(gl_buffer.handle());
        if (it == layouts_.end()) {
            return;
        }
        const VertexLayout& layout = it->second;
        u32 keep_mask = 0;
        for (const VertexAttribute& attribute : layout.attributes) {
            glEnableVertexAttribArray(attribute.location);
            const GLint components = static_cast<GLint>(component_count(attribute.format));
            glVertexAttribPointer(attribute.location, components, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(layout.stride),
                                  reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.offset)));
            glVertexAttribDivisor(attribute.location, attribute.divisor);
            keep_mask |= 1u << attribute.location;
        }

        // Disable any attribute a *previous* pass left enabled that this layout
        // does not use. Without this, an instanced pass's per-instance attributes
        // would stay live into the next, non-instanced pass and read whatever
        // buffer happens to be bound. The instance buffer's bind deliberately
        // does not do this (see bind_instance_buffer), so a caller binds the
        // per-vertex buffer first and the per-instance buffer second.
        for (u32 location = 0; location < kMaxVertexAttributes; ++location) {
            const u32 bit = 1u << location;
            if ((enabled_attributes_ & bit) != 0 && (keep_mask & bit) == 0) {
                glDisableVertexAttribArray(static_cast<GLuint>(location));
            }
        }
        enabled_attributes_ = keep_mask;
    }

    void bind_instance_buffer(Buffer& buffer, u32 byte_offset = 0) override {
        auto& gl_buffer = static_cast<OpenGLBuffer&>(buffer);
        glBindBuffer(GL_ARRAY_BUFFER, gl_buffer.handle());

        const auto it = layouts_.find(gl_buffer.handle());
        if (it == layouts_.end()) {
            return;
        }
        // Only add: the per-vertex attributes configured by bind_vertex_buffer
        // belong to the other buffer and must stay live. Each attribute's divisor
        // comes from its layout, so this call is what turns a plain layout into an
        // instanced one. The offset is applied per attribute, which is what lets
        // one shared buffer hold every batch of the frame.
        const VertexLayout& layout = it->second;
        for (const VertexAttribute& attribute : layout.attributes) {
            glEnableVertexAttribArray(attribute.location);
            const GLint components = static_cast<GLint>(component_count(attribute.format));
            glVertexAttribPointer(
                attribute.location, components, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(layout.stride),
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(byte_offset) + attribute.offset));
            glVertexAttribDivisor(attribute.location, attribute.divisor);
            enabled_attributes_ |= 1u << attribute.location;
        }
    }

    void bind_index_buffer(Buffer& buffer) override {
        auto& gl_buffer = static_cast<OpenGLBuffer&>(buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_buffer.handle());
    }

    void bind_texture(u32 slot, Texture& texture) override {
        auto& gl_texture = static_cast<OpenGLTexture&>(texture);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, gl_texture.handle());
    }

    void draw_indexed(u32 index_count) override {
        if (index_count == 0) {
            return;
        }
        const GLenum mode = topology_ == PrimitiveTopology::Lines ? GL_LINES : GL_TRIANGLES;
        glDrawElements(mode, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, nullptr);
    }

    void draw_indexed_instanced(u32 index_count, u32 instance_count) override {
        if (index_count == 0 || instance_count == 0) {
            return;
        }
        const GLenum mode = topology_ == PrimitiveTopology::Lines ? GL_LINES : GL_TRIANGLES;
        glDrawElementsInstanced(mode, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, nullptr,
                                static_cast<GLsizei>(instance_count));
    }

    void read_pixels(u32 x, u32 y, u32 width, u32 height, std::span<u8> out) override {
        if (out.size() < static_cast<usize>(width) * height * 4U) {
            return;
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(static_cast<GLint>(x), static_cast<GLint>(y), static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    }

private:
    // The GL 3.3 core profile guarantees at least 16 vertex attribute slots.
    static constexpr u32 kMaxVertexAttributes = 16;

    Window* window_;
    GLuint vao_ = 0;
    GLuint bound_shader_ = 0;
    RenderTarget* bound_target_ = nullptr;
    u32 default_framebuffer_width_ = 0;
    u32 default_framebuffer_height_ = 0;
    PrimitiveTopology topology_ = PrimitiveTopology::Triangles;
    // Which attribute locations are currently enabled on the single VAO. Tracked
    // so bind_vertex_buffer can disable the ones a previous pass enabled that the
    // new layout does not use -- an instanced pass leaves its per-instance
    // attributes live otherwise, and the next pass reads a stale buffer.
    u32 enabled_attributes_ = 0;
    std::unordered_map<GLuint, VertexLayout> layouts_;
};

} // namespace

namespace {

// Desktop only: gladLoadGL wants a plain function pointer, but the entry-point
// resolver is a member function. This trampoline bridges the two: it is only
// ever called during create_opengl_device(), where the window is known.
//
// On web there is nothing to load: Emscripten's WebGL2 binding supplies the
// entry points directly, so the trampoline is compiled out rather than left as
// dead code that would need a glad header.
#if !defined(FE_PLATFORM_WEB)
Window* g_loading_window = nullptr;

void* load_gl_proc(const char* name) {
    return g_loading_window->get_proc_address(name);
}
#endif

} // namespace

Result<std::unique_ptr<Device>> create_opengl_device(Window& window) {
#if defined(FE_PLATFORM_WEB)
    // The browser resolves its own WebGL2 entry points; the context is already
    // current, so there is nothing left to load. The parameter stays because the
    // factory signature is the same on both platforms.
    (void)window;
#else
    // Resolve the OpenGL entry points through the window's loader. This must
    // happen after the context is current, which the window guarantees.
    g_loading_window = &window;
    const int loaded = gladLoadGL(reinterpret_cast<GLADloadfunc>(&load_gl_proc));
    g_loading_window = nullptr;
    if (loaded == 0) {
        return Error("failed to load OpenGL entry points; an OpenGL 3.3 core context is required");
    }
#endif

    auto device = std::make_unique<OpenGLDevice>(window);
    FE_LOG_INFO("graphics device: %s (%s)", device->backend_name(), device->info().c_str());
    return std::unique_ptr<Device>(std::move(device));
}

} // namespace fe::rhi::opengl
