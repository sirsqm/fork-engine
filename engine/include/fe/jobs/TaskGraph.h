// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <functional>
#include <vector>

namespace fe {

// A task's handle within a TaskGraph. Ids are dense indices assigned by add() in
// call order, so a graph built the same way twice has the same ids.
using TaskId = u32;

inline constexpr TaskId kInvalidTaskId = static_cast<TaskId>(-1);

// A small directed acyclic graph of tasks, built by the caller and executed by a
// JobSystem (ADR-0017, ADR-0068).
//
// This is the "task graph with dependencies" half of the jobs module. It is a
// *builder*: add() records a task and the tasks it depends on, and nothing runs
// until a JobSystem is handed the graph. Keeping the two apart is what lets a
// caller build a graph once and inspect it (count, dependencies) without a
// thread pool, and lets a test assert the shape of a graph with no concurrency.
//
// A dependency is a hard ordering edge: a task runs only after every task it
// depends on has finished. Tasks with no unmet dependency may run in any order,
// including in parallel, so a graph is the right shape only when that is true -
// two tasks that write the same memory must be ordered by a dependency.
//
// A task body must not throw: the JobSystem runs bodies on worker threads and
// does not translate an escaping exception into an Error (ADR-0003 - errors are
// values). A body that can fail should record the failure itself (an error flag
// a later task reads, a counter the caller inspects).
//
// Lifetime: a graph holds the task bodies by value, so it may outlive the
// JobSystem that ran it, but a body must not capture anything that dies before
// the graph runs.
class TaskGraph {
public:
    TaskGraph() = default;

    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;
    TaskGraph(TaskGraph&&) noexcept = default;
    TaskGraph& operator=(TaskGraph&&) noexcept = default;

    // Records a task and the tasks it must run after. Returns its id. `deps` may
    // name tasks added later, so a graph can be built in any order; a dependency
    // on an id that is never added is rejected when the graph runs, not here.
    TaskId add(std::function<void()> task, std::vector<TaskId> deps = {});

    [[nodiscard]] usize task_count() const noexcept { return tasks_.size(); }

    // The task body with this id. The id must be in range; run() checks that
    // before it reads one.
    [[nodiscard]] const std::function<void()>& task(TaskId id) const { return tasks_[id].fn; }

    // The dependencies of the task with this id, in the order they were given.
    [[nodiscard]] const std::vector<TaskId>& dependencies(TaskId id) const { return tasks_[id].deps; }

    // Drops every task. The graph is reusable afterwards.
    void clear() noexcept { tasks_.clear(); }

private:
    struct Task {
        std::function<void()> fn;
        std::vector<TaskId> deps;
    };

    std::vector<Task> tasks_;
};

} // namespace fe
