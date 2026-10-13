// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/rhi/Device.h"

#include <memory>

namespace fe {

class Window;

namespace rhi::opengl {

// Creates an OpenGL 3.3 core graphics device bound to `window`.
//
// This is the backend factory. It is called only by rhi::create_device(); no
// other part of the engine includes this header, which is what keeps OpenGL
// (and glad) out of the rest of the codebase.
//
// The window must already own a current OpenGL context. The function resolves
// the entry points through the window's loader and returns an Error if they
// cannot be loaded.
[[nodiscard]] Result<std::unique_ptr<Device>> create_opengl_device(Window& window);

} // namespace rhi::opengl
} // namespace fe
