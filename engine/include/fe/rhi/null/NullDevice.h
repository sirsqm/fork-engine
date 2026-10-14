// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/rhi/Device.h"

namespace fe::rhi::null {

// The null (headless) RHI backend: a Device whose resources are inert and whose
// commands are no-ops.
//
// It exists so the render path -- walking the scene, collecting instances,
// batching, and issuing draw calls -- runs with no display, no GPU, and no
// graphics context. Nothing is rasterised and read_pixels() leaves the caller's
// buffer untouched, so this backend proves *submission*, not pixels: a caller
// asserts the counts the renderers already report (sprites drawn, draw calls
// issued), never a framebuffer. See docs/adr/0042-null-rhi-device.md.
//
// Private to the rhi module: only the factory in Device.cpp and the runtime
// include this. A caller reaches it through rhi::create_null_device().
[[nodiscard]] Result<std::unique_ptr<Device>> create_null_device();

} // namespace fe::rhi::null
