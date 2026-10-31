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

// ---------------------------------------------------------------------------
// Pass culling (ADR-0110)
// ---------------------------------------------------------------------------

TEST_CASE("a pass whose written resource nobody reads is culled") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("gbuffer").ok()); // writes "albedo", nobody reads it
    REQUIRE(graph.add_pass("present").ok()); // writes nothing -> an external effect
    REQUIRE(graph.add_write("gbuffer", "albedo").ok());

    CHECK(graph.culled() == std::vector<std::string>{"gbuffer"});
    CHECK(run_order(graph) == std::vector<std::string>{"present"});
}

TEST_CASE("a pass whose output is read by a live pass is kept") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("shadow").ok());
    REQUIRE(graph.add_pass("main").ok());
    REQUIRE(graph.add_write("shadow", "shadow_map").ok());
    REQUIRE(graph.add_read("main", "shadow_map").ok());
    // `main` writes nothing, so it presents and is live; `shadow` feeds it.
    CHECK(graph.culled().empty());
    CHECK(run_order(graph) == std::vector<std::string>{"shadow", "main"});
}

TEST_CASE("a pass that writes a marked output is never culled") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("post").ok());
    REQUIRE(graph.add_write("post", "swapchain").ok());
    REQUIRE(graph.mark_output("swapchain").ok());

    CHECK(graph.culled().empty());
    CHECK(run_order(graph) == std::vector<std::string>{"post"});
}

TEST_CASE("culling is iterative: a dead chain is dropped end to end") {
    // a -> b -> c, where c reads nothing live and writes nothing live either.
    // a and b write resources only the dead c reads, so both fall with it.
    RenderGraph graph;
    REQUIRE(graph.add_pass("a").ok());
    REQUIRE(graph.add_pass("b").ok());
    REQUIRE(graph.add_pass("c").ok());
    REQUIRE(graph.add_write("a", "x").ok());
    REQUIRE(graph.add_read("b", "x").ok());
    REQUIRE(graph.add_write("b", "y").ok());
    REQUIRE(graph.add_read("c", "y").ok());
    REQUIRE(graph.add_write("c", "z").ok()); // "z" read by nobody, not an output

    CHECK(graph.culled() == std::vector<std::string>{"a", "b", "c"});
    CHECK(run_order(graph).empty());
}

TEST_CASE("a pass with no resource declarations is never culled") {
    // The regression guarantee: a graph built the old way -- names and
    // dependencies only -- runs exactly as before.
    RenderGraph graph;
    REQUIRE(graph.add_pass("world").ok());
    REQUIRE(graph.add_pass("ui").ok());
    REQUIRE(graph.add_dependency("ui", "world").ok());

    CHECK(graph.culled().empty());
    CHECK(run_order(graph) == std::vector<std::string>{"world", "ui"});
}

TEST_CASE("a dependency on a culled pass is dropped, not an error") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("shadow").ok());
    REQUIRE(graph.add_pass("main").ok());
    REQUIRE(graph.add_write("shadow", "shadow_map").ok()); // read by nobody
    REQUIRE(graph.add_dependency("main", "shadow").ok());

    CHECK(graph.culled() == std::vector<std::string>{"shadow"});
    // `main` survives; the dangling dependency is simply gone.
    CHECK(run_order(graph) == std::vector<std::string>{"main"});
}

TEST_CASE("a cycle among live passes is still refused") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("a").ok());
    REQUIRE(graph.add_pass("b").ok());
    REQUIRE(graph.add_write("a", "x").ok());
    REQUIRE(graph.add_write("b", "y").ok());
    REQUIRE(graph.add_read("a", "y").ok());
    REQUIRE(graph.add_read("b", "x").ok());
    REQUIRE(graph.add_dependency("a", "b").ok());
    REQUIRE(graph.add_dependency("b", "a").ok());

    const auto ordered = graph.order();
    REQUIRE_FALSE(ordered.ok());
    CHECK(ordered.error().message().find("cycle") != std::string::npos);
}

TEST_CASE("adding a read or write validates the pass and the resource name") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("main").ok());
    CHECK_FALSE(graph.add_read("missing", "x").ok());
    CHECK_FALSE(graph.add_write("missing", "x").ok());
    CHECK_FALSE(graph.add_read("main", "").ok());
    CHECK_FALSE(graph.add_write("main", "").ok());
    CHECK_FALSE(graph.mark_output("").ok());
}

TEST_CASE("declaring the same read or write twice is idempotent") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("shadow").ok());
    REQUIRE(graph.add_pass("main").ok());
    REQUIRE(graph.add_write("shadow", "shadow_map").ok());
    REQUIRE(graph.add_write("shadow", "shadow_map").ok());
    REQUIRE(graph.add_read("main", "shadow_map").ok());
    REQUIRE(graph.add_read("main", "shadow_map").ok());

    CHECK(graph.culled().empty());
    CHECK(run_order(graph) == std::vector<std::string>{"shadow", "main"});
}

TEST_CASE("clear drops resources and outputs as well as passes") {
    RenderGraph graph;
    REQUIRE(graph.add_pass("post").ok());
    REQUIRE(graph.add_write("post", "swapchain").ok());
    REQUIRE(graph.mark_output("swapchain").ok());
    graph.clear();

    CHECK(graph.empty());
    CHECK(graph.culled().empty());
    // A fresh pass writing the same name is culled: the output mark is gone.
    REQUIRE(graph.add_pass("post").ok());
    REQUIRE(graph.add_write("post", "swapchain").ok());
    CHECK(graph.culled() == std::vector<std::string>{"post"});
}
