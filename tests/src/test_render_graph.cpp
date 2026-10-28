// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Render-graph seam tests (ADR-0100). The graph is a pure value -- it holds no
// GPU state and touches no device -- so these run headlessly and assert on the
// run order, the cycle refusal, and the per-pass profiler scope.

#include <doctest/doctest.h>

#include "fe/core/Profiler.h"
#include "fe/render/RenderGraph.h"

#include <string>
#include <vector>

using namespace fe;
using render::RenderGraph;

namespace {

// Runs a graph, recording the pass names in the order they were invoked.
std::vector<std::string> run_order(const RenderGraph& graph) {
    std::vector<std::string> visited;
    const Error error = graph.run([&](std::string_view name) {
        visited.emplace_back(name);
        return Error::none();
    });
    CHECK_FALSE(error);
    return visited;
}

} // namespace

TEST_CASE("an unconstrained graph runs in declaration order") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("shadow").ok());
    REQUIRE(graph.add_pass("main").ok());
    REQUIRE(graph.add_pass("post").ok());

    CHECK(run_order(graph) == std::vector<std::string>{"shadow", "main", "post"});
}

TEST_CASE("a dependency reorders passes ahead of their dependents") {
    RenderGraph graph;
    // Declared out of order: post first, but it depends on main, which depends
    // on shadow. The order must come out shadow, main, post regardless.
    REQUIRE(graph.add_pass("post").ok());
    REQUIRE(graph.add_pass("main").ok());
    REQUIRE(graph.add_pass("shadow").ok());
    REQUIRE(graph.add_dependency("post", "main").ok());
    REQUIRE(graph.add_dependency("main", "shadow").ok());

    CHECK(run_order(graph) == std::vector<std::string>{"shadow", "main", "post"});
}

TEST_CASE("a pass with no constraints keeps its declared position") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("a").ok());
    REQUIRE(graph.add_pass("b").ok());
    REQUIRE(graph.add_pass("c").ok());
    // c must follow a, but b is unconstrained. b keeps its slot, so the order is
    // a, b, c rather than a, c, b.
    REQUIRE(graph.add_dependency("c", "a").ok());

    CHECK(run_order(graph) == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("the order is stable across identical graphs") {
    RenderGraph first;
    RenderGraph second;
    for (RenderGraph* graph : {&first, &second}) {
        REQUIRE(graph->add_pass("world").ok());
        REQUIRE(graph->add_pass("ui").ok());
        REQUIRE(graph->add_pass("overlay").ok());
        REQUIRE(graph->add_dependency("overlay", "ui").ok());
        REQUIRE(graph->add_dependency("ui", "world").ok());
    }

    CHECK(run_order(first) == run_order(second));
}

TEST_CASE("a cycle is refused and names the passes involved") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("a").ok());
    REQUIRE(graph.add_pass("b").ok());
    REQUIRE(graph.add_pass("c").ok());
    REQUIRE(graph.add_dependency("a", "b").ok());
    REQUIRE(graph.add_dependency("b", "a").ok());
    // c is independent; only a and b are stuck. The message names them after
    // "among:", so look there -- the surrounding prose ("cycle", "graph")
    // contains letters that would otherwise match by accident.
    const auto ordered = graph.order();
    REQUIRE_FALSE(ordered.ok());
    const std::string message = ordered.error().message();
    const usize marker = message.find("among:");
    REQUIRE(marker != std::string::npos);
    const std::string stuck = message.substr(marker);
    CHECK(stuck.find('a') != std::string::npos);
    CHECK(stuck.find('b') != std::string::npos);
    CHECK(stuck.find('c') == std::string::npos);
}

TEST_CASE("adding a pass validates its name") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("main").ok());
    // A duplicate name is ambiguous and refused.
    CHECK_FALSE(graph.add_pass("main").ok());
    // An empty name is refused.
    CHECK_FALSE(graph.add_pass("").ok());
    CHECK(graph.pass_count() == 1);
}

TEST_CASE("a dependency requires both passes to exist") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("main").ok());
    CHECK_FALSE(graph.add_dependency("main", "missing").ok());
    CHECK_FALSE(graph.add_dependency("missing", "main").ok());
    CHECK_FALSE(graph.add_dependency("main", "main").ok());
}

TEST_CASE("run stops at the first failing pass and propagates its error") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("first").ok());
    REQUIRE(graph.add_pass("second").ok());
    REQUIRE(graph.add_pass("third").ok());
    REQUIRE(graph.add_dependency("second", "first").ok());
    REQUIRE(graph.add_dependency("third", "second").ok());

    std::vector<std::string> visited;
    const Error error = graph.run([&](std::string_view name) {
        visited.emplace_back(name);
        if (name == "second") {
            return Error{"boom"};
        }
        return Error::none();
    });
    REQUIRE(error);
    CHECK(error.message() == "boom");
    // The third pass never ran.
    CHECK(visited == std::vector<std::string>{"first", "second"});
}

TEST_CASE("each pass runs inside a profiler scope named pass/<name>") {
    // The graph brackets every pass so a FrameReport attributes time per pass.
    // This drives a real profiler frame and reads the names back.
    Profiler profiler;
    Profiler* previous = Profiler::current();
    Profiler::set_current(&profiler);
    REQUIRE(profiler.begin_frame().ok());

    RenderGraph graph;
    REQUIRE(graph.add_pass("shadow").ok());
    REQUIRE(graph.add_pass("main").ok());
    REQUIRE(graph.add_dependency("main", "shadow").ok());
    CHECK_FALSE(graph.run([](std::string_view) { return Error::none(); }));

    REQUIRE(profiler.end_frame().ok());
    Profiler::set_current(previous);

    const auto report = profiler.frame_report();
    REQUIRE(report.ok());
    bool saw_shadow = false;
    bool saw_main = false;
    for (u32 i = 0; i < report.value().scope_count; ++i) {
        const std::string& name = report.value().scopes[i].name;
        saw_shadow = saw_shadow || name == "pass/shadow";
        saw_main = saw_main || name == "pass/main";
    }
    CHECK(saw_shadow);
    CHECK(saw_main);
}
