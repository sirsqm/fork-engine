// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/rhi/opengl/OpenGLDevice.h"

#include "fe/core/Log.h"
#include "fe/platform/Window.h"

#include <glad/gl.h>

#include <cstring>
#include <string>
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

    void set_vec4(std::string_view name, const Vec4& value) override {
        const GLint location = location_of(name);
        if (location >= 0) {
            glUniform4f(location, value.x, value.y, value.z, value.w);
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

    void upload(const TextureDesc& desc) {
        glBindTexture(GL_TEXTURE_2D, handle_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, desc.pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, to_gl_filter(desc.min_filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, to_gl_filter(desc.mag_filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, to_gl_wrap(desc.wrap_u));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, to_gl_wrap(desc.wrap_v));
        if (desc.generate_mipmaps) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }

private:
    [[nodiscard]] static GLint to_gl_filter(TextureFilter filter) noexcept {
        return filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
    }
    [[nodiscard]] static GLint to_gl_wrap(TextureWrap wrap) noexcept {
        return wrap == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    }

    std::string name_;
    u32 width_;
    u32 height_;
    GLuint handle_ = 0;
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

    Result<ShaderRef> create_shader(const ShaderDesc& desc) override {
        std::string error;
        const GLuint vertex = compile_stage(GL_VERTEX_SHADER, desc.vertex_source, error);
        if (vertex == 0) {
            return Error("vertex shader '" + desc.name + "' failed to compile:\n" + error);
        }
        const GLuint fragment = compile_stage(GL_FRAGMENT_SHADER, desc.fragment_source, error);
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

    void begin_frame(u32 framebuffer_width, u32 framebuffer_height, const ClearColor& clear_color) override {
        set_viewport(0, 0, framebuffer_width, framebuffer_height);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        set_blend_mode(BlendMode::Alpha);
        glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void end_frame() override { window_->swap_buffers(); }

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
        for (const VertexAttribute& attribute : layout.attributes) {
            glEnableVertexAttribArray(attribute.location);
            const GLint components = static_cast<GLint>(component_count(attribute.format));
            glVertexAttribPointer(attribute.location, components, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(layout.stride),
                                  reinterpret_cast<const void*>(static_cast<std::uintptr_t>(attribute.offset)));
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
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, nullptr);
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
    Window* window_;
    GLuint vao_ = 0;
    GLuint bound_shader_ = 0;
    std::unordered_map<GLuint, VertexLayout> layouts_;
};

} // namespace

namespace {

// gladLoadGL wants a plain function pointer, but the entry-point resolver is a
// member function. This trampoline bridges the two: it is only ever called
// during create_opengl_device(), where the window is known.
Window* g_loading_window = nullptr;

void* load_gl_proc(const char* name) {
    return g_loading_window->get_proc_address(name);
}

} // namespace

Result<std::unique_ptr<Device>> create_opengl_device(Window& window) {
    // Resolve the OpenGL entry points through the window's loader. This must
    // happen after the context is current, which the window guarantees.
    g_loading_window = &window;
    const int loaded = gladLoadGL(reinterpret_cast<GLADloadfunc>(&load_gl_proc));
    g_loading_window = nullptr;
    if (loaded == 0) {
        return Error("failed to load OpenGL entry points; an OpenGL 3.3 core context is required");
    }

    auto device = std::make_unique<OpenGLDevice>(window);
    FE_LOG_INFO("graphics device: %s (%s)", device->backend_name(), device->info().c_str());
    return std::unique_ptr<Device>(std::move(device));
}

} // namespace fe::rhi::opengl
