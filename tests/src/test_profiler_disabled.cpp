// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// This translation unit is compiled with FE_PROFILING=0 (see tests/CMakeLists.txt),
// which is the only way to test the disabled build: the macro must expand to
// nothing, compile without warnings, and record no samples. It is a separate
// file because the define is per-translation-unit, and the rest of the test
// suite wants instrumentation on.

#define FE_PROFILING 0
#include "fe/core/Profiler.h"

namespace fe_test {

// Uses both macros the way a caller would. With FE_PROFILING=0 they expand to
// ((void)0): no ProfileScope is constructed, so a frame that is in progress on
// the thread's profiler records nothing. Returns the number of samples the
// disabled macros could have recorded, which is always zero.
fe::usize run_disabled_macros() {
    FE_PROFILE_FUNCTION();
    {
        FE_PROFILE_SCOPE("disabled/scope");
    }
    // The compile-time-off path must not leave an unused variable behind, so the
    // macros cannot declare one. This function compiles either way.
    return 0;
}

} // namespace fe_test
