// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/jobs/TaskGraph.h"

#include <functional>
#include <memory>

namespace fe {

// A fixed pool of worker threads, with `parallel_for` and a task graph (M9,
// ADR-0017, ADR-0068).
//
// This is where the engine's CPU parallelism lives. ADR-0017 allows threads to
// extract, cull, and compute, and keeps the RHI device single-threaded: a job
// must not touch the device, and only the thread that owns the frame submits.
// Nothing here reaches for the OS directly - the pool is `std::thread`,
// `std::mutex`, and `std::condition_variable`, the smallest surface the first
// cut needs (ADR-0017 rejected a third-party scheduler for exactly this reason).
//
// Two execution modes, chosen at construction:
//
//   * A pool of `worker_count` threads. parallel_for() and run() fan work out to
//     them and block the calling thread until it is done.
//   * A *single-threaded* system (`worker_count == 0`), where every job runs
//     inline on the calling thread, in order. This is not a degenerate case to
//     avoid: it is what makes the whole module testable against a serial
//     reference, what a platform without threads (a browser main thread, a
//     constrained target) would select, and what a caller gets by default so
//     that adding a JobSystem to existing code changes no behaviour until a
//     thread count is asked for. The API is identical in both modes; a caller
//     never branches on which one it has.
//
// Reentrancy: the calling thread participates in the work, so a job that calls
// parallel_for() (or run()) again runs the nested work inline on that worker.
// This is what keeps a job from deadlocking against a pool whose workers are all
// blocked waiting for it. The cost is that a nested call does not add
// parallelism; a job that wants the pool should not nest.
//
// Lifetime: the pool is created in the constructor and joined in the destructor,
// so no job outlives the JobSystem. A job body must not throw (see TaskGraph);
// an exception escaping a worker would terminate the process, which is the
// standard library's rule, not a choice made here.
class JobSystem {
public:
    // A default-constructed system is single-threaded (worker_count == 0): every
    // job runs inline on the caller. This is the safe default - a caller that
    // wants threads asks for them explicitly.
    JobSystem();
    explicit JobSystem(u32 worker_count);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // The number of worker threads. Zero means every job runs inline on the
    // calling thread.
    [[nodiscard]] u32 worker_count() const noexcept { return worker_count_; }

    // True when jobs run on a pool rather than inline. A caller may branch on
    // this for diagnostics, but must not need to for correctness: the API's
    // result is the same either way.
    [[nodiscard]] bool is_threaded() const noexcept { return worker_count_ > 0; }

    // Runs `body(index)` for every index in [begin, end), splitting the range
    // into chunks and running the chunks in parallel. Blocks until every chunk
    // has finished, so on return the whole range has been processed.
    //
    // The result is identical to a serial loop over the same range: the body is
    // invoked once per index, exactly once, and each index sees the same value it
    // would in `for (usize i = begin; i < end; ++i) body(i)`. Only the *order* of
    // calls is unspecified, so the body must not assume one chunk ran before
    // another - two indices that write the same memory are a data race the caller
    // must avoid (chunk-local scratch, or an index-derived output slot).
    //
    // An empty range (end <= begin) invokes nothing. A range with fewer indices
    // than workers uses one chunk per index at most, so no worker is handed an
    // empty chunk unless the range is empty.
    void parallel_for(usize begin, usize end, const std::function<void(usize)>& body);

    // The common case of a range starting at zero.
    void parallel_for(usize count, const std::function<void(usize)>& body) { parallel_for(0, count, body); }

    // Runs a task graph, honouring every dependency, and blocks until all tasks
    // have finished. Tasks with no unmet dependency may run in parallel.
    //
    // Returns an Error when the graph is malformed - a dependency on an id that
    // was never added, or a cycle (a graph whose tasks can never all be ready).
    // A malformed graph runs nothing: the check happens before any task body is
    // invoked, so a caller never observes a partial run of a bad graph.
    [[nodiscard]] Error run(const TaskGraph& graph);

private:
    class Impl;

    // Shared by both modes. A single-threaded system holds no Impl at all (the
    // methods run their loop inline), which is why this is null when
    // worker_count_ == 0.
    std::unique_ptr<Impl> impl_;
    u32 worker_count_ = 0;
};

} // namespace fe
