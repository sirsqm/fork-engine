// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/jobs/JobSystem.h"

#include "fe/core/Error.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace fe {

TaskId TaskGraph::add(std::function<void()> task, std::vector<TaskId> deps) {
    const TaskId id = static_cast<TaskId>(tasks_.size());
    tasks_.push_back(Task{std::move(task), std::move(deps)});
    return id;
}

namespace {

// A validated graph, laid out for execution: each task's dependency count and
// successors, plus a topological order. The order is what the single-threaded
// run executes; for the threaded run it is only the proof, produced before any
// body runs, that the graph has no cycle and no unknown dependency.
struct GraphLayout {
    std::vector<u32> indegree;
    std::vector<std::vector<u32>> successors;
    std::vector<u32> order;
};

// Validates the graph and lays it out. Returns an Error - before any task body
// is invoked - when a task depends on an id that was never added or when the
// dependencies contain a cycle.
[[nodiscard]] Result<GraphLayout> build_layout(const TaskGraph& graph) {
    const usize n = graph.task_count();
    GraphLayout layout;
    layout.indegree.assign(n, 0);
    layout.successors.resize(n);

    for (u32 i = 0; i < n; ++i) {
        for (const TaskId dep : graph.dependencies(i)) {
            if (dep >= n) {
                return Error("task graph: task " + std::to_string(i) + " depends on unknown task " +
                             std::to_string(dep));
            }
            ++layout.indegree[i];
            layout.successors[dep].push_back(i);
        }
    }

    // Kahn's algorithm: the order it produces is a valid execution order, and a
    // shortfall means the graph has a cycle, so no execution order exists.
    std::vector<u32> ready;
    for (u32 i = 0; i < n; ++i) {
        if (layout.indegree[i] == 0) {
            ready.push_back(i);
        }
    }
    std::vector<u32> remaining = layout.indegree;
    layout.order.reserve(n);
    for (usize head = 0; head < ready.size(); ++head) {
        const u32 task = ready[head];
        layout.order.push_back(task);
        for (const u32 next : layout.successors[task]) {
            if (--remaining[next] == 0) {
                ready.push_back(next);
            }
        }
    }
    if (layout.order.size() != n) {
        return Error("task graph: dependencies contain a cycle");
    }
    return layout;
}

} // namespace

// The thread pool. One mutex, one condition variable, one task queue; a task is
// a callable that reports completion by decrementing a batch's remaining count.
// The submitting thread waits on its batch and *helps* run queued tasks while it
// waits, which is what keeps a job that calls parallel_for (or run) from
// deadlocking against a pool whose workers are all busy: the waiter works too.
class JobSystem::Impl {
public:
    explicit Impl(u32 worker_count) {
        workers_.reserve(worker_count);
        for (u32 i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void parallel_for(usize begin, usize end, const std::function<void(usize)>& body) {
        const usize count = end - begin;
        const u32 chunks = chunk_count(count);

        // The batch is a local: this call blocks until every chunk has run, so it
        // outlives every task that references it.
        Batch batch{chunks};
        {
            std::lock_guard lock(mutex_);
            for (u32 c = 0; c < chunks; ++c) {
                // Contiguous subranges, split so a chunk's indices are adjacent
                // and the split is deterministic: the same range splits the same
                // way every run, which is what makes the parallel result
                // comparable to the serial one.
                const usize lo = begin + count * c / chunks;
                const usize hi = begin + count * (c + 1) / chunks;
                queue_.push_back([this, &body, &batch, lo, hi] {
                    for (usize i = lo; i < hi; ++i) {
                        body(i);
                    }
                    // Reporting completion is the last act, so a chunk's writes
                    // are visible to the thread that observes the batch drain.
                    batch_finish(batch);
                });
            }
            work_cv_.notify_all();
        }
        help(batch);
    }

    [[nodiscard]] Error run(const TaskGraph& graph) {
        const usize n = graph.task_count();
        if (n == 0) {
            return Error::none();
        }
        Result<GraphLayout> layout = build_layout(graph);
        if (!layout) {
            return layout.error();
        }

        // The run state is shared: every task's callable holds a reference, so it
        // stays alive until the last task finishes, which is before run() returns.
        auto state = std::make_shared<GraphRun>(graph, std::move(layout.value()));
        {
            std::lock_guard lock(mutex_);
            for (u32 i = 0; i < n; ++i) {
                if (state->layout.indegree[i] == 0) {
                    push_task(state, i);
                }
            }
            work_cv_.notify_all();
        }
        help(state->batch);
        return Error::none();
    }

private:
    struct Batch {
        u32 remaining = 0;
    };

    // Everything a graph run needs, kept alive by the shared_ptr every task
    // callable captures. `layout.indegree` is decremented as tasks complete, so
    // it is the run's mutable state, not the caller's graph.
    struct GraphRun {
        GraphRun(const TaskGraph& g, GraphLayout l) : graph(&g), layout(std::move(l)) {}

        const TaskGraph* graph;
        GraphLayout layout;
        Batch batch;
    };

    // Completion, callable from a worker or the helping thread. Takes the same
    // mutex the queue uses; the caller must not hold it.
    void batch_finish(Batch& batch) {
        std::lock_guard lock(mutex_);
        --batch.remaining;
        work_cv_.notify_all();
    }

    // The number of chunks a range is split into: enough to fill every worker a
    // few times over for load balance, never more than the range itself, so an
    // empty chunk is never submitted.
    [[nodiscard]] u32 chunk_count(usize count) const {
        const usize target = static_cast<usize>(workers_.size()) * 4;
        const usize chunks = count < target ? count : target;
        return chunks == 0 ? 1 : static_cast<u32>(chunks);
    }

    // Queues task `id` of `state` and counts it against the run's batch. The lock
    // must be held: callers are the submit loop and a finishing task advancing its
    // successors.
    void push_task(const std::shared_ptr<GraphRun>& state, u32 id) {
        ++state->batch.remaining;
        queue_.push_back([this, state, id] {
            state->graph->task(id)();
            // Advance the graph: release each successor whose dependencies are now
            // all met, and finish this task. The successor push happens under the
            // lock so the batch count and the queue stay consistent for the
            // waiting thread.
            {
                std::lock_guard lock(mutex_);
                for (const u32 next : state->layout.successors[id]) {
                    if (--state->layout.indegree[next] == 0) {
                        push_task(state, next);
                    }
                }
            }
            batch_finish(state->batch);
        });
    }

    // Waits until `batch` is done, running queued tasks while it waits. Helping is
    // what makes a nested call safe: a worker that calls parallel_for has no other
    // worker guaranteed to pick up its chunks, so it must be willing to run them.
    void help(Batch& batch) {
        std::unique_lock lock(mutex_);
        while (batch.remaining != 0) {
            if (queue_.empty()) {
                work_cv_.wait(lock, [&] { return batch.remaining == 0 || !queue_.empty(); });
                continue;
            }
            std::function<void()> task = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            task();
            lock.lock();
        }
    }

    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                work_cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

JobSystem::JobSystem() = default;

JobSystem::JobSystem(u32 worker_count) : worker_count_(worker_count) {
    if (worker_count_ > 0) {
        impl_ = std::make_unique<Impl>(worker_count_);
    }
}

JobSystem::~JobSystem() = default;

void JobSystem::parallel_for(usize begin, usize end, const std::function<void(usize)>& body) {
    if (end <= begin) {
        return;
    }
    // A single index is the caller's own work, not a chunk worth queueing; the
    // single-threaded mode never has a pool to fan out to.
    if (!is_threaded() || end - begin == 1) {
        for (usize i = begin; i < end; ++i) {
            body(i);
        }
        return;
    }
    impl_->parallel_for(begin, end, body);
}

Error JobSystem::run(const TaskGraph& graph) {
    if (!is_threaded()) {
        // Single-threaded: validate first (so a malformed graph is still refused
        // before anything runs), then execute in topological order.
        Result<GraphLayout> layout = build_layout(graph);
        if (!layout) {
            return layout.error();
        }
        for (const u32 task : layout.value().order) {
            graph.task(task)();
        }
        return Error::none();
    }
    return impl_->run(graph);
}

} // namespace fe
