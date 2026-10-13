// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/rhi/Device.h"

namespace fe::rhi::opengl {

// The OpenGL 3.3 core backend. Defined in a private header: only the backend
// itself and the factory in Device.cpp include this.
[[nodiscard]] Result<std::unique_ptr<Device>> create_opengl_device(Window& window);

} // namespace fe::rhi::opengl
