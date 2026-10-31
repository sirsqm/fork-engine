// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Tests for the console panel's view model and the log buffer it reads. The view
// model is the headless half of the console: it merges the engine log and the
// script host's contained failures into one bounded, filtered ring. These tests
// drive it with a real LogBuffer and a real ScriptDiagnostics, and assert on the
// records it holds -- never on ImGui, which the panel owns.

#include <doctest/doctest.h>

#include "fe/core/LogBuffer.h"
#include "fe/editor/ConsolePanelViewModel.h"
#include "fe/editor/EditorContext.h"
#include "fe/script/ScriptDiagnostics.h"

#include <memory>
#include <string>
#include <vector>

using namespace fe;
using namespace fe::editor;

TEST_CASE("console view model copies new log records once") {
    LogBuffer log;
    ConsolePanelViewModel view;

    log.write(LogLevel::Info, "first");
    CHECK(view.poll(log) == 1);
    CHECK(view.size() == 1);

    // Nothing new: the second poll copies nothing.
    CHECK(view.poll(log) == 0);
    CHECK(view.size() == 1);

    log.write(LogLevel::Warn, "second");
    CHECK(view.poll(log) == 1);
    REQUIRE(view.size() == 2);
    CHECK(view.entries()[0].message == "first");
    CHECK(view.entries()[1].level == LogLevel::Warn);
}

TEST_CASE("console view model filters by level and reports hidden rows") {
    LogBuffer log;
    ConsolePanelViewModel view;

    log.write(LogLevel::Debug, "noisy");
    log.write(LogLevel::Error, "important");
    view.poll(log);

    // The default filter is Info, so Debug is hidden but kept.
    CHECK(view.size() == 2);
    CHECK(view.visible().size() == 1);
    CHECK(view.visible()[0].message == "important");
    CHECK(view.has_hidden());
    CHECK(view.hidden_count() == 1);

    // Lowering the filter reveals the Debug row without re-polling.
    view.set_filter(LogLevel::Trace);
    CHECK(view.visible().size() == 2);
    CHECK_FALSE(view.has_hidden());
}

TEST_CASE("console view model is bounded, dropping the oldest rows") {
    LogBuffer log;
    ConsolePanelViewModel view(3);

    for (int i = 0; i < 10; ++i) {
        log.write(LogLevel::Info, "row " + std::to_string(i));
        view.poll(log);
    }

    CHECK(view.size() == 3);
    CHECK(view.entries().front().message == "row 7");
    CHECK(view.entries().back().message == "row 9");
}

TEST_CASE("console view model surfaces a log that wrapped past it") {
    LogBuffer log(2);
    ConsolePanelViewModel view;

    log.write(LogLevel::Info, "a"); // sequence 1
    view.poll(log);

    // Write more than the log can hold without polling, so a record is dropped
    // before the reader sees it.
    log.write(LogLevel::Info, "b"); // 2
    log.write(LogLevel::Info, "c"); // 3
    log.write(LogLevel::Info, "d"); // 4

    CHECK_FALSE(view.log_truncated());
    view.poll(log);
    CHECK(view.log_truncated());
    // Exactly one record (sequence 2) fell in the gap: "c" and "d" are still
    // held and are read now.
    CHECK(view.log_dropped() == 1);
}

TEST_CASE("console view model does not report a gap when it keeps up") {
    LogBuffer log(2);
    ConsolePanelViewModel view;

    // Poll after every write, so the reader is never behind. The buffer evicts
    // records the reader has already seen, which is not a gap and must not be
    // reported as one.
    for (int i = 0; i < 10; ++i) {
        log.write(LogLevel::Info, "row " + std::to_string(i));
        CHECK(view.poll(log) == 1);
        CHECK_FALSE(view.log_truncated());
    }
    CHECK(view.log_dropped() == 0);
}

TEST_CASE("console view model merges script failures as marked rows") {
    ScriptDiagnostics diagnostics;
    ConsolePanelViewModel view;

    const Uuid id = Uuid::generate();
    diagnostics.record(id, "Player", "Spin", "boom", 1);
    diagnostics.record(id, "Enemy", "Chase", "kaput", 2);

    CHECK(view.poll(diagnostics) == 2);
    REQUIRE(view.size() == 2);

    // Failures are shown as errors and marked, so the panel can colour them.
    CHECK(view.entries()[0].script_failure);
    CHECK(view.entries()[0].level == LogLevel::Error);
    CHECK(view.entries()[0].message.find("Spin") != std::string::npos);
    CHECK(view.entries()[0].message.find("Player") != std::string::npos);
    CHECK(view.entries()[0].message.find("boom") != std::string::npos);

    // Polling again adds nothing.
    CHECK(view.poll(diagnostics) == 0);
    CHECK(view.size() == 2);

    diagnostics.record(id, "Boss", "Flee", "nope", 3);
    CHECK(view.poll(diagnostics) == 1);
    CHECK(view.size() == 3);
}

TEST_CASE("console view model clear drops the ring and the read marks") {
    LogBuffer log;
    ScriptDiagnostics diagnostics;
    ConsolePanelViewModel view;

    log.write(LogLevel::Info, "hello");
    diagnostics.record(Uuid::generate(), "N", "B", "m", 1);
    view.poll(log);
    view.poll(diagnostics);
    REQUIRE(view.size() == 2);

    view.clear();
    CHECK(view.size() == 0);

    // After a clear, records already read are read again (the marks were reset),
    // which is what a "clear and keep watching" console should do.
    CHECK(view.poll(log) == 1);
    CHECK(view.poll(diagnostics) == 1);
}

TEST_CASE("editor context captures its own log into the console buffer") {
    // A context installs an in-memory sink when constructed, so anything logged
    // while it is alive -- including by the editor itself -- lands in a buffer a
    // console can read. This is the end-to-end path the console panel uses.
    EditorContext context;
    context.log_buffer().clear();

    FE_LOG_WARN("editor context console probe");

    const std::vector<LogBuffer::Record> records = context.log_buffer().snapshot();
    REQUIRE(records.size() >= 1);
    CHECK(records.back().level == LogLevel::Warn);
    CHECK(records.back().message == "editor context console probe");

    ConsolePanelViewModel view;
    CHECK(view.poll(context.log_buffer()) >= 1);
    CHECK(view.entries().back().message == "editor context console probe");
}
