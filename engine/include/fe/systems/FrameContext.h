// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Math.h"
#include "fe/core/Types.h"

namespace fe {

class ActionMap;
class InputState;
class ResourceManager;
class FrameArena;
class JobSystem;
class UserData;

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

    // This frame's gameplay actions, already evaluated from `input`. Nullable for
    // the same reasons as `input`: a tool or a test that runs a schedule with no
    // window has no actions, and a system must null-check it. This is the seam
    // that lets one piece of gameplay code serve a keyboard and a touchscreen:
    // the system asks whether "MoveLeft" is held and never learns which device
    // produced it (docs/PLAN.md, `touch-input-actions`).
    ActionMap* actions = nullptr;

    // The frame's transient arena. Nullable: a tool or a test that builds no
    // per-frame scratch runs the schedule with no arena, and a system must
    // null-check it rather than assume one exists. When present it is reset once
    // per frame by the runtime, before the systems run, so anything a system
    // allocates from it is valid for that frame only: a pointer into the arena
    // must not outlive the frame (ADR-0017, ADR-0051, ADR-0053).
    FrameArena* arena = nullptr;

    // The render target's size in pixels, which is the space a pointer position
    // is in and therefore the space screen-space UI is laid out against
    // (ADR-0084). The runtime fills it from the window's framebuffer size before
    // the systems run, so a UI system and the renderer that draws its output agree
    // on the viewport without either reaching for the window.
    //
    // Zero when the frame has no surface -- a headless run, a tool, a test -- so a
    // UI system must treat a zero extent as "nothing to lay out" rather than
    // dividing by it. It is a size rather than a pointer because it is a value a
    // system reads and never owns.
    Vec2 viewport{};

    // The job system a system fans CPU work out to (ADR-0017, ADR-0068).
    // Nullable: a tool or a test that runs a schedule with no pool runs it with
    // no jobs, and a system must null-check it rather than assume one exists. A
    // system that wants to parallelise a loop calls
    // `context.jobs->parallel_for(...)`; when the pointer is null it runs the
    // same loop serially. The API is identical either way, so the system does not
    // branch on whether threads exist.
    //
    // A job must not touch the RHI device: the device is single-threaded
    // (ADR-0017), so a job extracts, culls, or computes, and only the thread that
    // owns the frame submits.
    JobSystem* jobs = nullptr;

    // The store a game saves its own data to (best score, settings). Nullable: a
    // tool or a test that runs a schedule with no engine has no store, and a
    // system that saves must null-check it rather than assume one exists. When
    // present it is the engine's store, so a save through the context is the same
    // save the engine reports, and a system never reaches for the engine to find
    // it. It may be an in-memory store that persists nothing -- a run with no
    // application id -- so a system treats a successful write as "saved for this
    // run" and never assumes a reload will see it (ADR-0096).
    UserData* user_data = nullptr;
};

} // namespace fe
