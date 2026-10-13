// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/rhi/Types.h"

#include <span>
#include <string>
#include <string_view>

namespace fe::rhi {

// A compiled shader program.
class Shader {
public:
    virtual ~Shader() = default;

    [[nodiscard]] virtual const std::string& name() const noexcept = 0;

    // Sets a 4x4 matrix uniform by name. Silently ignored if the uniform does
    // not exist, which keeps optional uniforms cheap; a missing *required*
    // uniform is a shader-authoring bug caught by the renderer's own tests.
    virtual void set_mat4(std::string_view name, const Mat4& value) = 0;
    virtual void set_vec4(std::string_view name, const Vec4& value) = 0;
    virtual void set_int(std::string_view name, i32 value) = 0;
    virtual void set_float(std::string_view name, f32 value) = 0;
};

// A GPU vertex or index buffer.
class Buffer {
public:
    enum class Kind : u8 {
        Vertex,
        Index,
    };

    virtual ~Buffer() = default;

    [[nodiscard]] virtual Kind kind() const noexcept = 0;
    [[nodiscard]] virtual u32 element_count() const noexcept = 0;

    // Replaces the buffer contents. `data` must contain element_count() *
    // (stride or sizeof(u32)) bytes.
    virtual void update(std::span<const u8> data) = 0;
};

// A GPU texture.
class Texture {
public:
    virtual ~Texture() = default;

    [[nodiscard]] virtual const std::string& name() const noexcept = 0;
    [[nodiscard]] virtual u32 width() const noexcept = 0;
    [[nodiscard]] virtual u32 height() const noexcept = 0;
};

} // namespace fe::rhi
