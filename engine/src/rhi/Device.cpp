// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/rhi/Device.h"

#include "fe/core/Log.h"
#include "fe/platform/Window.h"
#include "fe/rhi/opengl/OpenGLDevice.h"

namespace fe::rhi {

Result<std::unique_ptr<Device>> create_device(Window& window) {
    // Backend selection lives here and nowhere else. Adding a second backend
    // means adding a factory and a branch, not touching the rest of the engine.
    return opengl::create_opengl_device(window);
}

} // namespace fe::rhi
