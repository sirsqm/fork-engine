// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

// Assertion and contract-checking macros.
//
// Two distinct tools, deliberately kept separate:
//
//   FE_ASSERT(expr)   - a *debug* contract. Compiled out entirely in release
//                       builds. Use it for invariants that are guaranteed by
//                       construction and whose check would be too costly to
//                       keep in shipped games.
//
//   FE_CHECK(expr)    - an *always-on* contract. Evaluated in every build
//                       configuration. Use it for cheap precondition checks on
//                       public API boundaries and for data that comes from
//                       files, scripts, or the network.
//
// Neither tool is an error-handling mechanism. Recoverable failures must be
// reported through return values (see fe::Error / fe::Result), not assertions.

#if defined(NDEBUG) && !defined(FE_FORCE_ASSERTS)
#define FE_ASSERT_ENABLED 0
#else
#define FE_ASSERT_ENABLED 1
#endif

namespace fe {

// Reports a failed assertion and terminates. The default implementation logs a
// critical message and calls std::abort. Tests replace it to record failures
// instead of killing the process.
using AssertHandler = void (*)(const char* expression, const char* file, int line, const char* message);

void set_assert_handler(AssertHandler handler) noexcept;
[[nodiscard]] AssertHandler assert_handler() noexcept;

// Called by the macros below. Always terminates (via the active handler).
[[noreturn]] void assert_fail(const char* expression, const char* file, int line, const char* message) noexcept;

} // namespace fe

#define FE_ASSERT_IMPL(expr, msg)                                                                                      \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            ::fe::assert_fail(#expr, __FILE__, __LINE__, msg);                                                         \
        }                                                                                                              \
    } while (false)

#if FE_ASSERT_ENABLED
#define FE_ASSERT(expr) FE_ASSERT_IMPL(expr, nullptr)
#define FE_ASSERT_MSG(expr, msg) FE_ASSERT_IMPL(expr, msg)
#else
#define FE_ASSERT(expr) ((void)0)
#define FE_ASSERT_MSG(expr, msg) ((void)0)
#endif

// Always-on contract check. Prefer this at API boundaries and when validating
// data that the engine does not control.
#define FE_CHECK(expr) FE_ASSERT_IMPL(expr, nullptr)
#define FE_CHECK_MSG(expr, msg) FE_ASSERT_IMPL(expr, msg)
