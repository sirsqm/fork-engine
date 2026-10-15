// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/rhi/Device.h"

namespace fe::renderer3d {

// The knobs the post-processing chain reads, one set per frame. The defaults are
// the identity: exposure 1, no vignette. Enabling the chain with default
// settings still runs the pass, which is what a test that asserts "the chain
// ran" needs; a caller that wants no pass at all leaves `enabled` false.
struct PostProcessSettings {
    // When false, `Renderer3D` skips the chain entirely and draws straight to the
    // caller's target, exactly as before the chain existed.
    bool enabled = false;

    // Linear scale applied to the sampled colour before tonemapping. 1.0 is
    // neutral.
    f32 exposure = 1.0f;

    // Darkens the corners of the frame: 0.0 is off, 1.0 is a strong vignette.
    f32 vignette = 0.0f;

    // Applies a Reinhard tonemap (c/(c+1)) so a colour above 1.0 is compressed
    // instead of clipped. Named rather than folded into exposure because it is a
    // separate, optional step.
    bool tonemap = true;
};

// A screen-space post-processing chain over a rendered colour image.
//
// It owns one shader and one full-screen quad and nothing else: the source is a
// texture the caller already rendered into, and the destination is whatever
// render target is bound when `apply` is called. `Renderer3D` renders the 3D
// pass into an off-screen colour target and then hands that target's colour
// texture here, which is what makes the chain possible on the *window* too: the
// default framebuffer cannot be sampled as a texture, so the scene must be an
// off-screen image before it can be graded.
//
// The first cut is deliberate: exposure, an optional Reinhard tonemap, and a
// vignette, applied in one pass. Bloom, FXAA, and a user-authored effect stack
// are later work behind this same "sample a colour image, write a graded one"
// shape.
class PostProcess {
public:
    PostProcess() = default;
    ~PostProcess() = default;

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // Compiles the shader and uploads the full-screen quad. Returns an Error if
    // the device rejects the program.
    [[nodiscard]] Error initialize(rhi::Device& device);

    [[nodiscard]] bool ready() const noexcept { return shader_ != nullptr; }

    // Draws the full-screen quad, sampling `source` and writing the graded result
    // into the currently bound render target. The caller has bound the
    // destination and set its viewport. `source` must not be the destination's
    // own attachment; the caller guarantees that by rendering the scene into a
    // distinct off-screen target first.
    [[nodiscard]] Error apply(rhi::Device& device, rhi::Texture& source, const PostProcessSettings& settings);

    // The number of times `apply` has drawn, so a test can prove the chain ran
    // without reading back pixels.
    [[nodiscard]] u32 pass_count() const noexcept { return pass_count_; }

private:
    rhi::ShaderRef shader_;
    rhi::BufferRef quad_vertex_buffer_;
    rhi::BufferRef quad_index_buffer_;
    u32 pass_count_ = 0;
};

} // namespace fe::renderer3d
