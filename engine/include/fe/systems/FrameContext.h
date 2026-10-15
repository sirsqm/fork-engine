// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

namespace fe {

class InputState;
class ResourceManager;
class FrameArena;

// What a system is told about the frame it is running in.
//
// A system reads everything it needs from this rather than reaching for global
// state. That is what makes a system testable headlessly (construct a scene,
// call update with a known delta) and what will let the editor step a single
// frame while paused.
struct FrameContext {
    // Seconds since the previous frame, already clamped by the runtime so a
    // debugger breakpoint or a window drag does not produce a huge step.
    f32 delta_time = 0.0f;

    // Seconds since the runtime started. Monotonic; not affected by pausing.
    f64 elapsed_time = 0.0;

    // The resource manager systems load assets through. Nullable: a tool or a
    // test may run a schedule without one, and a system that needs it must
    // check rather than assume.
    ResourceManager* resources = nullptr;

    // This frame's input snapshot. Nullable: a headless tool or a test with no
    // window runs the schedule with no input, and a gameplay system must null-
    // check it (or disable itself) rather than assume a keyboard exists. The
    // edge state has already been advanced for the frame when a system sees it.
    InputState* input = nullptr;

    // The frame's transient arena. Nullable: a tool or a test that builds no
    // per-frame scratch runs the schedule with no arena, and a system must
    // null-check it rather than assume one exists. When present it is reset once
    // per frame by the runtime, before the systems run, so anything a system
    // allocates from it is valid for that frame only: a pointer into the arena
    // must not outlive the frame (ADR-0017, ADR-0051, ADR-0053).
    FrameArena* arena = nullptr;
};

} // namespace fe
