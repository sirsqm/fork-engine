// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/Assert.h"

#include "fe/core/Log.h"

#include <cstdlib>

namespace fe {
namespace {

[[noreturn]] void default_handler(const char* expression, const char* file, int line, const char* message) noexcept {
    if (message != nullptr) {
        FE_LOG_CRITICAL("assertion failed: %s (%s) at %s:%d", expression, message, file, line);
    } else {
        FE_LOG_CRITICAL("assertion failed: %s at %s:%d", expression, file, line);
    }
    std::abort();
}

AssertHandler g_handler = &default_handler;

} // namespace

void set_assert_handler(AssertHandler handler) noexcept {
    g_handler = handler != nullptr ? handler : &default_handler;
}

AssertHandler assert_handler() noexcept {
    return g_handler;
}

void assert_fail(const char* expression, const char* file, int line, const char* message) noexcept {
    g_handler(expression, file, line, message);
    // A custom handler is permitted to return for test purposes; guarantee that
    // a failed assertion never continues in a release build.
    std::abort();
}

} // namespace fe
