// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Test runner for the graphics tests. These create a real (but hidden) window
// and OpenGL context, so they need either a display or a software GL
// implementation (Mesa's llvmpipe works headless under Xvfb).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
