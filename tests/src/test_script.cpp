// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the scripting seam (ADR-0044): the Script interface, the ScriptHost
// that owns per-node instances by stable node id, the ScriptRegistry, and the
// ScriptSystem that drives the host from the ordinary schedule. All of it is
// headless -- no window, no RHI, no editor.
#include <doctest/doctest.h>

#include "fe/core/Log.h"
#include "fe/scene/Node.h"
#include "fe/scene/Scene.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptHost.h"
#include "fe/script/ScriptRegistry.h"
#include "fe/script/ScriptSystem.h"
#include "fe/systems/FrameContext.h"
#include "fe/systems/SceneSchedule.h"

#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace fe;

namespace {

// External observation point for a script. It lives outside the script so a
// test can read attach/detach counts after the host has destroyed the instance.
struct Probe {
    int updates = 0;
    int attaches = 0;
    int detaches = 0;
    std::string owner;
};

// Records lifecycle and update calls. `order` is optional and shared across
// scripts so a test can assert the order the host ran them in.
class RecordingScript final : public Script {
public:
    RecordingScript(std::string_view behaviour, Probe* probe, std::vector<std::string>* order = nullptr)
        : behaviour_(behaviour), probe_(probe), order_(order) {}

    [[nodiscard]] std::string_view name() const noexcept override { return behaviour_; }

    Error attach(Node& node) override {
        if (probe_ != nullptr) {
            ++probe_->attaches;
            probe_->owner = node.name();
        }
        return Error::none();
    }

    void detach(Node& node) override {
        (void)node;
        if (probe_ != nullptr) {
            ++probe_->detaches;
        }
    }

    Error update(const FrameContext&) override {
        if (probe_ != nullptr) {
            ++probe_->updates;
        }
        if (order_ != nullptr) {
            order_->push_back(std::string(behaviour_));
        }
        return Error::none();
    }

private:
    std::string behaviour_;
    Probe* probe_;
    std::vector<std::string>* order_;
};

// A script that reports failure as a value, the way ADR-0003 prefers.
class FailingScript final : public Script {
public:
    explicit FailingScript(Probe* probe) : probe_(probe) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "Failing"; }

    Error update(const FrameContext&) override {
        if (probe_ != nullptr) {
            ++probe_->updates;
        }
        return Error("boom");
    }

private:
    Probe* probe_;
};

// A script that throws, to prove the host contains exceptions too.
class ThrowingScript final : public Script {
public:
    explicit ThrowingScript(Probe* probe) : probe_(probe) {}

    [[nodiscard]] std::string_view name() const noexcept override { return "Throwing"; }

    Error update(const FrameContext&) override {
        if (probe_ != nullptr) {
            ++probe_->updates;
        }
        throw std::runtime_error("kaboom");
    }

private:
    Probe* probe_;
};

// A script that refuses its node in attach().
class RefusingScript final : public Script {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "Refusing"; }

    Error attach(Node&) override { return Error("refused"); }

    Error update(const FrameContext&) override { return Error::none(); }
};

// A default-constructible script, for the registry tests.
class PlainScript final : public Script {
public:
    PlainScript() = default;
    explicit PlainScript(std::string_view behaviour) : behaviour_(behaviour) {}

    [[nodiscard]] std::string_view name() const noexcept override { return behaviour_; }

    Error update(const FrameContext&) override { return Error::none(); }

private:
    std::string behaviour_ = "plain";
};

// Captures log output so a test can assert a failure was reported. Installs
// itself as a sink and clears it on teardown, like tests/src/test_log.cpp.
class CaptureSink final : public LogSink {
public:
    struct Entry {
        LogLevel level;
        std::string message;
    };

    void write(LogLevel level, std::string_view message) override {
        entries.push_back(Entry{level, std::string(message)});
    }

    std::vector<Entry> entries;
};

class ScopedLogCapture {
public:
    ScopedLogCapture() : sink_(std::make_shared<CaptureSink>()) {
        log::clear_sinks();
        log::add_sink(sink_);
    }

    ~ScopedLogCapture() { log::clear_sinks(); }

    [[nodiscard]] const std::vector<CaptureSink::Entry>& entries() const { return sink_->entries; }

private:
    std::shared_ptr<CaptureSink> sink_;
};

[[nodiscard]] bool has_error_mentioning(const ScopedLogCapture& capture, std::string_view needle) {
    for (const CaptureSink::Entry& entry : capture.entries()) {
        if (entry.level == LogLevel::Error && entry.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("scripts update in attachment order") {
    Scene scene;
    Node* first = scene.create_node("First");
    Node* second = scene.create_node("Second");
    std::vector<std::string> order;
    ScriptHost host;

    CHECK_FALSE(host.has_script(first->id()));
    CHECK(host.attach_script(*first, std::make_unique<RecordingScript>("first", nullptr, &order)).ok());
    CHECK(host.attach_script(*second, std::make_unique<RecordingScript>("second", nullptr, &order)).ok());
    CHECK(host.script_count() == 2);

    const FrameContext context;
    host.update(context);

    REQUIRE(order.size() == 2);
    CHECK(order[0] == "first");
    CHECK(order[1] == "second");

    // The order is stable frame to frame.
    order.clear();
    host.update(context);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "first");
    CHECK(order[1] == "second");
}

TEST_CASE("detaching a script stops its updates and calls detach") {
    Scene scene;
    Node* node = scene.create_node("Node");
    Probe probe;
    ScriptHost host;

    CHECK(host.attach_script(*node, std::make_unique<RecordingScript>("spin", &probe)).ok());
    CHECK(probe.attaches == 1);
    CHECK(probe.owner == "Node");

    const FrameContext context;
    host.update(context);
    CHECK(probe.updates == 1);

    CHECK(host.detach_script(node->id()));
    CHECK(probe.detaches == 1);
    CHECK_FALSE(host.has_script(node->id()));
    CHECK(host.script(node->id()) == nullptr);

    // No further updates reach the detached script.
    host.update(context);
    CHECK(probe.updates == 1);

    // Detaching again is a no-op, not an error.
    CHECK_FALSE(host.detach_script(node->id()));
}

TEST_CASE("detaching by scene looks the node up by id") {
    Scene scene;
    Node* node = scene.create_node("Node");
    Probe probe;
    ScriptHost host;
    CHECK(host.attach_script(*node, std::make_unique<RecordingScript>("spin", &probe)).ok());

    CHECK(host.detach_script(scene, node->id()));
    CHECK(probe.detaches == 1);

    // An unknown id is a clean false, not a crash.
    const Uuid missing = Uuid::generate();
    CHECK_FALSE(host.detach_script(scene, missing));
    CHECK_FALSE(host.detach_script(missing));
}

TEST_CASE("two nodes get independent script instances") {
    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");
    Probe probe_a;
    Probe probe_b;
    ScriptHost host;

    CHECK(host.attach_script(*a, std::make_unique<RecordingScript>("spin", &probe_a)).ok());
    CHECK(host.attach_script(*b, std::make_unique<RecordingScript>("spin", &probe_b)).ok());

    // Distinct instances, even though they share a behaviour name.
    CHECK((host.script(a->id()) != host.script(b->id())));

    const FrameContext context;
    host.update(context);
    host.update(context);

    // Each instance counted only its own updates, and bound to its own node.
    CHECK(probe_a.updates == 2);
    CHECK(probe_b.updates == 2);
    CHECK(probe_a.owner == "A");
    CHECK(probe_b.owner == "B");

    // Detaching one leaves the other running.
    CHECK(host.detach_script(a->id()));
    host.update(context);
    CHECK(probe_a.updates == 2);
    CHECK(probe_b.updates == 3);
}

TEST_CASE("a failing script is contained, disabled, and reported") {
    Scene scene;
    Node* good = scene.create_node("Good");
    Node* bad = scene.create_node("Bad");
    Probe probe_good;
    Probe probe_bad;
    ScriptHost host;
    CHECK(host.attach_script(*good, std::make_unique<RecordingScript>("good", &probe_good)).ok());
    CHECK(host.attach_script(*bad, std::make_unique<FailingScript>(&probe_bad)).ok());

    const FrameContext context;
    {
        ScopedLogCapture capture;
        host.update(context);

        // The failure was reported with the script name and the node it is on.
        CHECK(has_error_mentioning(capture, "Failing"));
        CHECK(has_error_mentioning(capture, "Bad"));
    }

    // The failing script ran once and was then disabled; the other kept running.
    CHECK(probe_bad.updates == 1);
    CHECK(probe_good.updates == 1);
    CHECK(host.failed_count() == 1);
    REQUIRE(host.script(bad->id()) != nullptr);
    CHECK_FALSE(host.script(bad->id())->enabled());

    // It is owned but skipped on later frames; the good script keeps running.
    host.update(context);
    CHECK(probe_bad.updates == 1);
    CHECK(probe_good.updates == 2);
    CHECK(host.failed_count() == 1);
}

TEST_CASE("a throwing script is contained and reported") {
    Scene scene;
    Node* node = scene.create_node("Thrower");
    Probe probe;
    ScriptHost host;
    CHECK(host.attach_script(*node, std::make_unique<ThrowingScript>(&probe)).ok());

    const FrameContext context;
    {
        ScopedLogCapture capture;
        host.update(context);
        CHECK(has_error_mentioning(capture, "Throwing"));
    }

    CHECK(probe.updates == 1);
    CHECK(host.failed_count() == 1);
    CHECK_FALSE(host.script(node->id())->enabled());

    // Disabled, so it does not throw again.
    host.update(context);
    CHECK(probe.updates == 1);
}

TEST_CASE("an attach refusal leaves the node unscripted") {
    Scene scene;
    Node* node = scene.create_node("Node");
    ScriptHost host;

    const Error error = host.attach_script(*node, std::make_unique<RefusingScript>());
    CHECK_FALSE(error.ok());
    CHECK_FALSE(host.has_script(node->id()));
    CHECK(host.script_count() == 0);
}

TEST_CASE("re-attaching replaces the node's script, one per node") {
    Scene scene;
    Node* node = scene.create_node("Node");
    Probe probe_one;
    Probe probe_two;
    ScriptHost host;

    CHECK(host.attach_script(*node, std::make_unique<RecordingScript>("one", &probe_one)).ok());
    CHECK(host.attach_script(*node, std::make_unique<RecordingScript>("two", &probe_two)).ok());

    CHECK(host.script_count() == 1);
    CHECK(probe_one.detaches == 1); // the replaced script was detached
    REQUIRE(host.script(node->id()) != nullptr);
    CHECK(host.script(node->id())->name() == std::string_view("two"));

    const FrameContext context;
    host.update(context);
    CHECK(probe_two.updates == 1);
    CHECK(probe_one.updates == 0);
}

TEST_CASE("a disabled script is skipped but still owned") {
    Scene scene;
    Node* node = scene.create_node("Node");
    Probe probe;
    ScriptHost host;
    CHECK(host.attach_script(*node, std::make_unique<RecordingScript>("spin", &probe)).ok());

    host.script(node->id())->set_enabled(false);

    const FrameContext context;
    host.update(context);
    CHECK(probe.updates == 0);
    CHECK(host.script_count() == 1);

    host.script(node->id())->set_enabled(true);
    host.update(context);
    CHECK(probe.updates == 1);
}

TEST_CASE("clear detaches and forgets every script") {
    Scene scene;
    Node* a = scene.create_node("A");
    Node* b = scene.create_node("B");
    Probe probe_a;
    Probe probe_b;
    ScriptHost host;
    CHECK(host.attach_script(*a, std::make_unique<RecordingScript>("a", &probe_a)).ok());
    CHECK(host.attach_script(*b, std::make_unique<RecordingScript>("b", &probe_b)).ok());

    host.clear();

    CHECK(host.script_count() == 0);
    CHECK(probe_a.detaches == 1);
    CHECK(probe_b.detaches == 1);

    // The host is reusable afterwards.
    CHECK(host.attach_script(*a, std::make_unique<RecordingScript>("a", &probe_a)).ok());
    CHECK(host.script_count() == 1);
}

TEST_CASE("ScriptSystem drives the host through the schedule") {
    Scene scene;
    Node* node = scene.create_node("Node");
    Probe probe;
    ScriptHost host;
    CHECK(host.attach_script(*node, std::make_unique<RecordingScript>("spin", &probe)).ok());

    SceneSchedule schedule;
    schedule.add_system<ScriptSystem>(host);
    CHECK(schedule.system_count() == 1);

    const FrameContext context;
    schedule.update(context, scene);
    schedule.update(context, scene);

    CHECK(probe.updates == 2);
}

TEST_CASE("ScriptRegistry creates behaviours by name") {
    ScriptRegistry registry;
    CHECK_FALSE(registry.contains("spin"));
    CHECK(registry.script_count() == 0);
    CHECK(registry.create("spin") == nullptr);

    registry.register_script("spin", [] { return std::make_unique<PlainScript>("spin"); });
    CHECK(registry.contains("spin"));
    CHECK(registry.script_count() == 1);

    std::unique_ptr<Script> instance = registry.create("spin");
    REQUIRE(instance != nullptr);
    CHECK(instance->name() == std::string_view("spin"));

    // A later registration of the same name replaces the earlier one.
    registry.register_script("spin", [] { return std::make_unique<PlainScript>("spin2"); });
    CHECK(registry.script_count() == 1);
    std::unique_ptr<Script> replaced = registry.create("spin");
    REQUIRE(replaced != nullptr);
    CHECK(replaced->name() == std::string_view("spin2"));

    // register_type<T> is the convenience form.
    registry.register_type<PlainScript>("plain");
    CHECK(registry.contains("plain"));
    CHECK(registry.create("plain") != nullptr);

    const std::vector<std::string> names = registry.names();
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "spin");
    CHECK(names[1] == "plain");
}
