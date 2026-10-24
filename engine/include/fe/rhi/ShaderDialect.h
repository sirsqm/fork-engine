// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <string>
#include <string_view>

namespace fe::rhi {

// The GLSL dialect a shader source is written for.
//
// Every shader in the engine is authored once, in the desktop dialect. A
// backend that speaks a different GLSL flavour translates at the seam instead of
// forking every shader string: the desktop backend hands the driver the bytes it
// was given, and a future GLSL ES backend (WebGL2, GLES3) asks for the ES
// translation. The choice belongs to the backend, never to a renderer or a
// shader author.
enum class ShaderDialect : u8 {
    DesktopCore, // `#version 330 core`, the OpenGL 3.3 core profile.
    Es,          // `#version 300 es`, the GLSL ES 3.00 profile (WebGL2, GLES3).
};

// Which stage a source belongs to. Only the fragment stage needs an injected
// default precision in the ES dialect, so the translation is stage-aware.
enum class ShaderStage : u8 {
    Vertex,
    Fragment,
};

// Translates `source` for `dialect`.
//
// This is a pure function of (dialect, stage, source): no device, no global
// state, no I/O. For `ShaderDialect::DesktopCore` it is the identity, so the
// bytes handed to the desktop driver are unchanged and existing rendering cannot
// regress. For `ShaderDialect::Es` it rewrites the version directive to
// `#version 300 es` and, for fragment shaders that do not already declare a
// precision, injects `precision highp float;` and `precision highp int;`.
//
// The translation is idempotent: a source that already carries an ES version
// directive and a precision declaration is returned unchanged, so translating
// twice never emits a second version directive or a duplicate precision.
[[nodiscard]] std::string translate_shader_source(ShaderDialect dialect, ShaderStage stage, std::string_view source);

} // namespace fe::rhi
