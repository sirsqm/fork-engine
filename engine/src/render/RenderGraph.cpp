// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/render/RenderGraph.h"

#include "fe/core/Profiler.h"

#include <algorithm>

namespace fe::render {

namespace {

// A dependency edge, resolved to indices once so ordering does not compare
// strings per pass.
struct Edge {
    u32 pass = 0;
    u32 after = 0;
};

} // namespace

u32 RenderGraph::find(std::string_view name) const noexcept {
    for (u32 i = 0; i < passes_.size(); ++i) {
        if (passes_[i].name == name) {
            return i;
        }
    }
    return kInvalid;
}

Result<void> RenderGraph::add_pass(std::string_view name) {
    if (name.empty()) {
        return Error{"render graph: a pass name must not be empty"};
    }
    if (find(name) != kInvalid) {
        return Error{"render graph: duplicate pass name '" + std::string{name} + "'"};
    }
    passes_.push_back(Pass{std::string{name}, {}, {}, {}});
    return {};
}

Result<void> RenderGraph::add_dependency(std::string_view pass, std::string_view dependency) {
    const u32 pass_index = find(pass);
    if (pass_index == kInvalid) {
        return Error{"render graph: unknown pass '" + std::string{pass} + "'"};
    }
    if (find(dependency) == kInvalid) {
        return Error{"render graph: unknown dependency '" + std::string{dependency} + "'"};
    }
    if (pass == dependency) {
        return Error{"render graph: pass '" + std::string{pass} + "' cannot depend on itself"};
    }
    passes_[pass_index].after.push_back(std::string{dependency});
    return {};
}

Result<void> RenderGraph::add_read(std::string_view pass, std::string_view resource) {
    const u32 pass_index = find(pass);
    if (pass_index == kInvalid) {
        return Error{"render graph: unknown pass '" + std::string{pass} + "'"};
    }
    if (resource.empty()) {
        return Error{"render graph: a resource name must not be empty"};
    }
    std::vector<std::string>& reads = passes_[pass_index].reads;
    if (std::find(reads.begin(), reads.end(), resource) == reads.end()) {
        reads.emplace_back(resource);
    }
    return {};
}

Result<void> RenderGraph::add_write(std::string_view pass, std::string_view resource) {
    const u32 pass_index = find(pass);
    if (pass_index == kInvalid) {
        return Error{"render graph: unknown pass '" + std::string{pass} + "'"};
    }
    if (resource.empty()) {
        return Error{"render graph: a resource name must not be empty"};
    }
    std::vector<std::string>& writes = passes_[pass_index].writes;
    if (std::find(writes.begin(), writes.end(), resource) == writes.end()) {
        writes.emplace_back(resource);
    }
    return {};
}

Result<void> RenderGraph::mark_output(std::string_view resource) {
    if (resource.empty()) {
        return Error{"render graph: a resource name must not be empty"};
    }
    if (!is_output(resource)) {
        outputs_.emplace_back(resource);
    }
    return {};
}

bool RenderGraph::is_output(std::string_view resource) const noexcept {
    return std::find(outputs_.begin(), outputs_.end(), resource) != outputs_.end();
}

std::vector<u32> RenderGraph::live_pass_indices() const {
    const usize count = passes_.size();
    // `live[i]` starts true and is cleared by culling. A pass is culled when it
    // declares writes (so it is a producer, not a presenter) and none of those
    // writes is read by a still-live pass or marked as an output. Iterating to a
    // fixed point is required because culling a consumer makes its producer's
    // write dead too.
    std::vector<bool> live(count, true);
    bool changed = true;
    while (changed) {
        changed = false;
        for (usize i = 0; i < count; ++i) {
            if (!live[i]) {
                continue;
            }
            const Pass& pass = passes_[i];
            // A pass that writes nothing is assumed to have an external effect
            // (it presents) and is never culled.
            if (pass.writes.empty()) {
                continue;
            }
            bool output_live = false;
            for (const std::string& written : pass.writes) {
                if (is_output(written)) {
                    output_live = true;
                    break;
                }
                for (usize j = 0; j < count; ++j) {
                    if (j == i || !live[j]) {
                        continue;
                    }
                    const Pass& consumer = passes_[j];
                    if (std::find(consumer.reads.begin(), consumer.reads.end(), written) != consumer.reads.end()) {
                        output_live = true;
                        break;
                    }
                }
                if (output_live) {
                    break;
                }
            }
            if (!output_live) {
                live[i] = false;
                changed = true;
            }
        }
    }

    std::vector<u32> indices;
    for (usize i = 0; i < count; ++i) {
        if (live[i]) {
            indices.push_back(static_cast<u32>(i));
        }
    }
    return indices;
}

std::vector<std::string> RenderGraph::culled() const {
    const std::vector<u32> live = live_pass_indices();
    std::vector<std::string> dropped;
    for (usize i = 0; i < passes_.size(); ++i) {
        if (std::find(live.begin(), live.end(), static_cast<u32>(i)) == live.end()) {
            dropped.push_back(passes_[i].name);
        }
    }
    return dropped;
}

Result<std::string_view> RenderGraph::pass_name(usize index) const {
    if (index >= passes_.size()) {
        return Error{"render graph: pass index out of range"};
    }
    return std::string_view{passes_[index].name};
}

void RenderGraph::clear() noexcept {
    passes_.clear();
    outputs_.clear();
}

Result<std::vector<std::string>> RenderGraph::order() const {
    // Only live passes are ordered and run. A pass the culling rule drops is
    // absent from the result entirely, and a dependency on it is dropped with
    // it: the dependent was written against a producer whose output nobody reads.
    const std::vector<u32> live = live_pass_indices();
    const usize count = live.size();
    std::vector<Edge> edges;
    edges.reserve(count);
    // remaining[k] is how many of live pass k's dependencies have not yet been
    // emitted. `position_of` maps a pass index to its slot in the live list, so
    // an edge can be resolved to a live slot or recognised as dangling.
    std::vector<u32> remaining(count, 0);
    std::vector<u32> position_of(passes_.size(), kInvalid);
    for (usize k = 0; k < count; ++k) {
        position_of[live[k]] = static_cast<u32>(k);
    }
    for (usize k = 0; k < count; ++k) {
        for (const std::string& dependency : passes_[live[k]].after) {
            const u32 after = find(dependency);
            // A dependency can only be unknown if a caller mutated the pass list
            // directly; add_dependency validates it, so this is a guard, not a
            // reachable path.
            if (after == kInvalid) {
                return Error{"render graph: pass '" + passes_[live[k]].name + "' depends on unknown '" + dependency +
                             "'"};
            }
            // A dependency on a culled pass is dropped rather than an error.
            const u32 after_slot = position_of[after];
            if (after_slot == kInvalid) {
                continue;
            }
            edges.push_back(Edge{static_cast<u32>(k), after_slot});
            ++remaining[k];
        }
    }

    std::vector<std::string> result;
    result.reserve(count);
    std::vector<bool> emitted(count, false);

    // Kahn's algorithm, choosing the lowest unemitted live slot each round.
    // Picking by position rather than by any traversal order is what makes the
    // output stable and declaration-order preserving: a pass with no unmet
    // dependency runs as early as its position allows, so an unconstrained graph
    // comes out in exactly the order it was declared.
    for (usize step = 0; step < count; ++step) {
        usize next = count;
        for (usize i = 0; i < count; ++i) {
            if (!emitted[i] && remaining[i] == 0) {
                next = i;
                break;
            }
        }
        if (next == count) {
            // No pass is ready, so a cycle holds the rest. Name them: that is
            // what a caller needs to find the loop.
            std::string stuck;
            for (usize i = 0; i < count; ++i) {
                if (!emitted[i]) {
                    if (!stuck.empty()) {
                        stuck += ", ";
                    }
                    stuck += passes_[live[i]].name;
                }
            }
            return Error{"render graph: dependency cycle among: " + stuck};
        }
        emitted[next] = true;
        result.push_back(passes_[live[next]].name);
        for (const Edge& edge : edges) {
            if (edge.after == next) {
                --remaining[edge.pass];
            }
        }
    }

    return result;
}

Error RenderGraph::run(const std::function<Error(std::string_view)>& run_pass) const {
    auto ordered = order();
    if (!ordered) {
        return ordered.error();
    }
    for (const std::string& pass : ordered.value()) {
#if FE_PROFILING
        // The scope is named per pass so a FrameReport attributes time to each
        // one. `scope_name` must outlive the scope object's construction, which
        // it does: it lives to the end of the iteration.
        const std::string scope_name = "pass/" + pass;
        ProfileScope scope(scope_name);
#endif
        if (Error error = run_pass(pass); error) {
            return error;
        }
    }
    return Error::none();
}

} // namespace fe::render
