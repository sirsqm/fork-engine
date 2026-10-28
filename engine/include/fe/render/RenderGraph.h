// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fe::render {

// The render-graph seam (ADR-0100).
//
// A frame's passes used to be ordered by hand in code: `Renderer2D` draws the
// world, then the UI; `Renderer3D` draws the shadow pass, then the main pass,
// then the post-processing chain. The order was correct but implicit -- it lived
// in the sequence of calls in a `render()` body, so nothing could reorder a
// pass, drop one whose output nobody reads, or time one as a unit.
//
// This type makes the order data. A caller declares passes and the dependencies
// between them; `order()` returns a run order that respects every dependency, and
// `run()` invokes each pass in that order, bracketing each in a profiler scope so
// a pass shows up in a `FrameReport` as `pass/<name>`.
//
// Scope of this first cut: it orders passes and runs them. It does *not* own
// render targets, manage resource lifetimes, or drop passes whose output is
// unread -- those need a resource description the passes do not yet carry, and
// are the follow-on that makes the graph an optimizer rather than a scheduler.
// What it establishes is the seam: the moment pass order is data, reordering,
// culling, and per-pass profiling become changes to that data rather than edits
// to the renderers' structure (ADR-0017).
//
// The graph is a pure value: it holds no GPU state and touches no device, so it
// is fully testable headlessly. That is the same split the extraction seam
// (ADR-0052) uses, and for the same reason -- the decision can be asserted
// without a context.
class RenderGraph {
public:
    // One pass: a name, and the names of passes it must run after.
    struct Pass {
        std::string name;
        std::vector<std::string> after;
    };

    RenderGraph() = default;

    // Declares a pass. Fails if the name is empty or already declared -- two
    // passes with one name is ambiguous, so it is refused rather than guessed at.
    [[nodiscard]] Result<void> add_pass(std::string_view name);

    // Declares that `pass` must run after `dependency`. Fails if either name is
    // unknown. A dependency may be declared before either pass is added only
    // through `add_pass`'s `after` list; this method requires both to exist.
    [[nodiscard]] Result<void> add_dependency(std::string_view pass, std::string_view dependency);

    // A pass's declared name, in declaration order. Fails if out of range.
    [[nodiscard]] Result<std::string_view> pass_name(usize index) const;

    // The number of declared passes.
    [[nodiscard]] usize pass_count() const noexcept { return passes_.size(); }

    // True when no passes are declared.
    [[nodiscard]] bool empty() const noexcept { return passes_.empty(); }

    // Drops every pass and dependency.
    void clear() noexcept;

    // The run order: every pass follows the passes it declared. Ties are broken
    // by declaration order, so the result is stable -- two graphs with the same
    // passes and the same dependencies order them identically, and a pass with no
    // constraints keeps the position it was added in. Fails if the dependencies
    // form a cycle, naming the passes left unresolved.
    [[nodiscard]] Result<std::vector<std::string>> order() const;

    // `order()`, then `run_pass` for each pass in turn. Stops at the first pass
    // that returns an Error and propagates it. Each pass runs inside a profiler
    // scope named `pass/<name>` (a no-op when no profiler frame is active), so
    // the frame report attributes time per pass without the caller instrumenting
    // each one.
    [[nodiscard]] Error run(const std::function<Error(std::string_view)>& run_pass) const;

private:
    // Finds a pass index by name, or kInvalid when absent.
    [[nodiscard]] u32 find(std::string_view name) const noexcept;

    static constexpr u32 kInvalid = 0xFFFFFFFFu;

    std::vector<Pass> passes_;
};

} // namespace fe::render
