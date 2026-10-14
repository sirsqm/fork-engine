// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/rhi/Device.h"

#include "fe/core/Log.h"
#include "fe/platform/Window.h"
#include "fe/rhi/null/NullDevice.h"
#include "fe/rhi/opengl/OpenGLDevice.h"

namespace fe::rhi {

Result<std::unique_ptr<Device>> create_device(Window& window) {
    // Backend selection lives here and nowhere else. Adding a second backend
    // means adding a factory and a branch, not touching the rest of the engine.
    return opengl::create_opengl_device(window);
}

Result<std::unique_ptr<Device>> create_null_device() {
    // A second branch on the same seam. The null device is chosen explicitly by
    // the runtime for a contextless window, never inferred from the window: a
    // caller that wants no rendering at all simply skips device creation.
    return null::create_null_device();
}

} // namespace fe::rhi
