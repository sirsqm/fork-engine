// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The `jobs` module: a fixed thread pool with `parallel_for` and a task graph
// (ADR-0017, ADR-0068). These tests pin the two things the ADR promises and a
// concurrency surface must prove:
//
//   * the parallel result is *identical to the serial path* -- every index is
//     visited exactly once and with the value a serial loop would give it, and a
//     graph runs every task exactly once in an order that honours dependencies;
//   * the module is correct in both execution modes. A single-threaded system
//     (worker_count 0, the default) runs everything inline, so the same
//     assertions hold with no threads at all.
//
// No mocks: the real pool runs real threads. Where order is unspecified (the
// order chunks run in), a test asserts only what the contract guarantees and
// never assumes a scheduling.
//
// A nested-call test is included deliberately: a job that calls parallel_for
// again is the case that deadlocks a naive pool, and it is the reason the
// submitting thread helps run the queue.

#include <doctest/doctest.h>

#include "fe/jobs/JobSystem.h"

#include <atomic>
#include <numeric>
#include <vector>

using namespace fe;

namespace {

// A worker count that is more than one on every machine but still small, so the
// chunking path (range split across workers) is exercised without oversubscribing
// a CI runner.
constexpr u32 kTestWorkers = 4;

} // namespace

TEST_CASE("a default JobSystem is single-threaded") {
    JobSystem jobs;
    CHECK(jobs.worker_count() == 0);
    CHECK_FALSE(jobs.is_threaded());
}

TEST_CASE("a JobSystem with workers reports them") {
    JobSystem jobs(kTestWorkers);
    CHECK(jobs.worker_count() == kTestWorkers);
    CHECK(jobs.is_threaded());
}

TEST_CASE("parallel_for visits every index exactly once, in both modes") {
    // Each index writes its own slot, so no two chunks touch the same memory and
    // the test needs no lock. That is the contract a caller parallelising a loop
    // must honour, and it is what makes this a valid serial-equivalence check.
    for (const u32 workers : {0u, kTestWorkers}) {
        CAPTURE(workers);
        JobSystem jobs(workers);

        constexpr usize kCount = 1000;
        std::vector<usize> visited(kCount, 0);
        jobs.parallel_for(kCount, [&visited](usize i) { visited[i] += 1; });

        for (usize i = 0; i < kCount; ++i) {
            CHECK(visited[i] == 1);
        }
    }
}

TEST_CASE("parallel_for produces the same result as a serial loop") {
    for (const u32 workers : {0u, kTestWorkers}) {
        CAPTURE(workers);
        JobSystem jobs(workers);

        constexpr usize kCount = 10007; // deliberately not a multiple of the chunk count
        std::vector<usize> parallel(kCount, 0);
        jobs.parallel_for(kCount, [&parallel](usize i) { parallel[i] = i * i + 3; });

        std::vector<usize> serial(kCount, 0);
        for (usize i = 0; i < kCount; ++i) {
            serial[i] = i * i + 3;
        }
        CHECK(parallel == serial);
    }
}

TEST_CASE("parallel_for over a sub-range covers exactly [begin, end)") {
    JobSystem jobs(kTestWorkers);

    constexpr usize kCount = 64;
    std::vector<int> touched(kCount, 0);
    jobs.parallel_for(10, 40, [&touched](usize i) { touched[i] += 1; });

    for (usize i = 0; i < kCount; ++i) {
        const bool in_range = i >= 10 && i < 40;
        CHECK(touched[i] == (in_range ? 1 : 0));
    }
}

TEST_CASE("parallel_for over an empty or single-index range is well defined") {
    for (const u32 workers : {0u, kTestWorkers}) {
        CAPTURE(workers);
        JobSystem jobs(workers);

        int calls = 0;
        jobs.parallel_for(5, 5, [&calls](usize) { ++calls; }); // empty
        CHECK(calls == 0);

        jobs.parallel_for(7, 6, [&calls](usize) { ++calls; }); // end < begin
        CHECK(calls == 0);

        jobs.parallel_for(3, 4, [&calls](usize i) { // exactly one index
            ++calls;
            CHECK(i == 3);
        });
        CHECK(calls == 1);
    }
}

TEST_CASE("parallel_for with fewer indices than workers uses one chunk per index") {
    JobSystem jobs(kTestWorkers);
    // Two indices, four workers: the split must not hand any worker an empty
    // chunk, and every index must still run exactly once.
    std::vector<usize> visited(2, 0);
    jobs.parallel_for(2, [&visited](usize i) { visited[i] += 1; });
    CHECK(visited[0] == 1);
    CHECK(visited[1] == 1);
}

TEST_CASE("a job may call parallel_for again without deadlocking") {
    // The reentrancy case: every worker could be busy with an outer chunk, so a
    // nested call must run its own work rather than wait for a free worker. With
    // enough outer chunks to occupy every worker, a pool that did not help would
    // hang here; the test passing is the proof it does not.
    JobSystem jobs(kTestWorkers);

    constexpr usize kOuter = 32;
    constexpr usize kInner = 16;
    std::vector<usize> outer_done(kOuter, 0);
    std::vector<usize> inner_done(kOuter * kInner, 0);

    jobs.parallel_for(kOuter, [&](usize outer) {
        jobs.parallel_for(kInner, [&, outer](usize inner) { inner_done[outer * kInner + inner] += 1; });
        outer_done[outer] += 1;
    });

    for (const usize count : outer_done) {
        CHECK(count == 1);
    }
    for (const usize count : inner_done) {
        CHECK(count == 1);
    }
}

TEST_CASE("a task graph runs every task exactly once, honouring dependencies") {
    for (const u32 workers : {0u, kTestWorkers}) {
        CAPTURE(workers);
        JobSystem jobs(workers);

        // A diamond: a -> {b, c} -> d, plus an independent e. Each task records
        // the sequence number at which it ran; a dependency must always have a
        // lower number than the task that depends on it, in both modes.
        std::atomic<u32> sequence{0};
        std::vector<std::atomic<u32>> ran_at(5);
        for (auto& slot : ran_at) {
            slot.store(0);
        }
        std::vector<std::atomic<u32>> runs(5);
        for (auto& slot : runs) {
            slot.store(0);
        }

        TaskGraph graph;
        auto record = [&](usize id) {
            return [&, id] {
                ran_at[id].store(sequence.fetch_add(1) + 1);
                runs[id].fetch_add(1);
            };
        };
        const TaskId a = graph.add(record(0));
        const TaskId b = graph.add(record(1), {a});
        const TaskId c = graph.add(record(2), {a});
        const TaskId d = graph.add(record(3), {b, c});
        const TaskId e = graph.add(record(4));
        (void)e;

        const Error error = jobs.run(graph);
        CHECK_FALSE(error);

        for (usize id = 0; id < 5; ++id) {
            CHECK(runs[id].load() == 1);
        }
        CHECK(ran_at[a].load() < ran_at[b].load());
        CHECK(ran_at[a].load() < ran_at[c].load());
        CHECK(ran_at[b].load() < ran_at[d].load());
        CHECK(ran_at[c].load() < ran_at[d].load());
    }
}

TEST_CASE("a graph with no tasks runs nothing and succeeds") {
    JobSystem jobs(kTestWorkers);
    TaskGraph graph;
    CHECK(graph.task_count() == 0);
    CHECK_FALSE(jobs.run(graph));
}

TEST_CASE("a graph with independent tasks runs all of them") {
    JobSystem jobs(kTestWorkers);
    constexpr usize kTasks = 64;
    std::vector<std::atomic<u32>> runs(kTasks);
    for (auto& slot : runs) {
        slot.store(0);
    }

    TaskGraph graph;
    for (usize i = 0; i < kTasks; ++i) {
        graph.add([&runs, i] { runs[i].fetch_add(1); });
    }
    CHECK(graph.task_count() == kTasks);
    CHECK_FALSE(jobs.run(graph));
    for (const auto& slot : runs) {
        CHECK(slot.load() == 1);
    }
}

TEST_CASE("a graph that depends on an unknown task is refused before anything runs") {
    for (const u32 workers : {0u, kTestWorkers}) {
        CAPTURE(workers);
        JobSystem jobs(workers);

        bool ran = false;
        TaskGraph graph;
        graph.add([&ran] { ran = true; });
        graph.add([&ran] { ran = true; }, {7}); // id 7 is never added

        const Error error = jobs.run(graph);
        CHECK(error);
        CHECK_FALSE(ran); // a malformed graph runs nothing
    }
}

TEST_CASE("a graph whose dependencies form a cycle is refused before anything runs") {
    for (const u32 workers : {0u, kTestWorkers}) {
        CAPTURE(workers);
        JobSystem jobs(workers);

        // A three-task cycle: 0 depends on 2, 1 depends on 0, 2 depends on 1. add()
        // accepts a dependency on an id added later, which is what lets a cycle be
        // expressed at all; run() must reject it rather than hang or run part of
        // it. No task body may run, so the flag stays false.
        bool ran = false;
        TaskGraph loop;
        const TaskId t0 = loop.add([&ran] { ran = true; }, {2});
        const TaskId t1 = loop.add([&ran] { ran = true; }, {t0});
        (void)t1;
        const TaskId t2 = loop.add([&ran] { ran = true; }, {1});
        (void)t2;

        const Error error = jobs.run(loop);
        CHECK(error);
        CHECK_FALSE(ran);
    }
}

TEST_CASE("a graph task may read a dependency's result") {
    // The ordering is not just a scheduling hint: a task that reads what its
    // dependency wrote must see the write. This runs enough times to catch a
    // pool that released the dependent before the dependency's writes were
    // visible.
    JobSystem jobs(kTestWorkers);

    for (int repeat = 0; repeat < 200; ++repeat) {
        std::vector<int> data(256, 0);
        TaskGraph graph;
        const TaskId fill = graph.add([&data] {
            for (usize i = 0; i < data.size(); ++i) {
                data[i] = static_cast<int>(i + 1);
            }
        });
        const TaskId sum = graph.add(
            [&data, &repeat] {
                const int total = std::accumulate(data.begin(), data.end(), 0);
                CHECK(total == 256 * 257 / 2);
                (void)repeat;
            },
            {fill});
        (void)sum;

        CHECK_FALSE(jobs.run(graph));
    }
}
