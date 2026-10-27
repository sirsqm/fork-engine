// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The GLES3 dialect parity check (docs/PLAN.md,
// `webgl2-mobile-parity`).
//
// WebGL2 and the GLES3 contexts Android and iOS expose are close but not
// identical, and a construct that a desktop GL 3.3 core context accepts can be a
// hard compile error on either. The engine authors every shader once, in the
// desktop dialect, and translates it at the RHI seam (ADR-0072); the ES
// translation has only ever been compiled by a browser, so a shader that the
// seam translates wrongly is invisible until a phone runs it.
//
// This program closes that gap on the build machine: it reads the engine's own
// shader sources (the exact files the renderers ship), translates them through
// the real `translate_shader_source` seam, and compiles each translated source
// in a GLES3 context. A software GLES implementation is enough -- the point is
// the compiler, not the rasteriser -- so it runs headless with Mesa's surfaceless
// EGL platform and no display.
//
// It exits 77 (CTest's SKIP_RETURN_CODE) when no GLES3 context can be created, so
// a machine without the EGL/GLES driver stays green rather than failing on
// something it cannot test.

#include "fe/rhi/ShaderDialect.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// One authored shader literal: the identifier it is bound to, its stage, and the
// GLSL text between the `R"glsl(` / `)glsl"` delimiters.
struct ShaderLiteral {
    std::string name;
    fe::rhi::ShaderStage stage;
    std::string source;
};

[[nodiscard]] std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Pulls every `R"glsl( ... )glsl"` block out of `text`, taking the identifier it
// is assigned to as its name and calling a fragment stage from that name. This
// is a deliberately small scanner, not a parser: the engine's shader sources are
// all plain `constexpr const char* kXShaderSource = R"glsl(...)glsl";`
// definitions, and reading them from the shipped file (rather than a copy) is
// what makes the check prove the real sources compile.
[[nodiscard]] std::vector<ShaderLiteral> extract(std::string_view text) {
    std::vector<ShaderLiteral> out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t at = text.find("R\"glsl(", pos);
        if (at == std::string_view::npos) {
            break;
        }
        const std::size_t body = at + 7; // strlen("R\"glsl(")
        const std::size_t end = text.find(")glsl\"", body);
        if (end == std::string_view::npos) {
            break;
        }
        // The name is the last identifier before `= R"glsl(`.
        std::string name = "shader";
        const std::size_t eq = text.rfind('=', at);
        if (eq != std::string_view::npos) {
            std::size_t b = eq;
            while (b > 0 && (std::isalnum(static_cast<unsigned char>(text[b - 1])) || text[b - 1] == '_')) {
                --b;
            }
            if (b < eq) {
                name = std::string(text.substr(b, eq - b));
            }
        }
        ShaderLiteral lit;
        lit.name = name;
        lit.stage =
            name.find("Fragment") != std::string::npos ? fe::rhi::ShaderStage::Fragment : fe::rhi::ShaderStage::Vertex;
        lit.source = std::string(text.substr(body, end - body));
        out.push_back(std::move(lit));
        pos = end + 6;
    }
    return out;
}

[[nodiscard]] bool compile(GLenum stage, const std::string& source, std::string& error) {
    const GLuint shader = glCreateShader(stage);
    const char* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) {
        glDeleteShader(shader);
        return true;
    }
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<std::size_t>(len > 1 ? len : 2), '\0');
    glGetShaderInfoLog(shader, len, nullptr, log.data());
    error = log;
    glDeleteShader(shader);
    return false;
}

// Creates a surfaceless GLES3 context with no display. Returns EGL_NO_CONTEXT
// when the driver is absent, which the caller turns into a skip.
[[nodiscard]] EGLContext make_context(EGLDisplay& display) {
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display == nullptr) {
        return EGL_NO_CONTEXT;
    }
    display = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) != EGL_TRUE) {
        return EGL_NO_CONTEXT;
    }
    const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                   EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (eglChooseConfig(display, config_attrs, &config, 1, &count) != EGL_TRUE || count < 1) {
        return EGL_NO_CONTEXT;
    }
    const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT) {
        return EGL_NO_CONTEXT;
    }
    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
        return EGL_NO_CONTEXT;
    }
    return context;
}

} // namespace

int main(int argc, char** argv) {
    EGLDisplay display = EGL_NO_DISPLAY;
    const EGLContext context = make_context(display);
    if (context == EGL_NO_CONTEXT) {
        std::printf("gles_dialect_compile: no GLES3 context available, skipping\n");
        return 77;
    }
    std::printf("gles_dialect_compile: %s | %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));

    int failures = 0;
    int compiled = 0;
    for (int i = 1; i < argc; ++i) {
        const std::vector<ShaderLiteral> literals = extract(read_file(argv[i]));
        if (literals.empty()) {
            std::printf("FAIL %s: no shader literals found\n", argv[i]);
            ++failures;
            continue;
        }
        for (const ShaderLiteral& lit : literals) {
            const std::string es = fe::rhi::translate_shader_source(fe::rhi::ShaderDialect::Es, lit.stage, lit.source);
            std::string error;
            const GLenum stage = lit.stage == fe::rhi::ShaderStage::Fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER;
            if (compile(stage, es, error)) {
                ++compiled;
            } else {
                std::printf("FAIL %s (%s)\n%s\n", lit.name.c_str(), argv[i], error.c_str());
                ++failures;
            }
        }
    }

    std::printf("gles_dialect_compile: %d shader(s) compiled under GLES3, %d failure(s)\n", compiled, failures);
    if (failures != 0) {
        return 1;
    }
    eglDestroyContext(display, context);
    eglTerminate(display);
    return 0;
}
