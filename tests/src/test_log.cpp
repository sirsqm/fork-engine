// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/Log.h"

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

using namespace fe;

namespace {

// Captures formatted log output so tests can assert on it without touching the
// process's stdout. Installs itself as a sink and removes itself on teardown.
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

// RAII helper: installs a capture sink and restores the logger's previous
// state (level and sinks) when it goes out of scope.
class ScopedLogCapture {
public:
    ScopedLogCapture() : sink_(std::make_shared<CaptureSink>()), previous_level_(log::min_level()) {
        log::clear_sinks();
        log::add_sink(sink_);
    }

    ~ScopedLogCapture() {
        log::clear_sinks();
        log::set_min_level(previous_level_);
    }

    [[nodiscard]] const std::vector<CaptureSink::Entry>& entries() const { return sink_->entries; }

private:
    std::shared_ptr<CaptureSink> sink_;
    LogLevel previous_level_;
};

} // namespace

TEST_CASE("log sink receives formatted messages") {
    ScopedLogCapture capture;
    FE_LOG_INFO("hello %d", 42);

    REQUIRE(capture.entries().size() == 1);
    CHECK(capture.entries()[0].level == LogLevel::Info);
    CHECK(capture.entries()[0].message == "hello 42");
}

TEST_CASE("log level filtering suppresses messages below the minimum") {
    ScopedLogCapture capture;
    log::set_min_level(LogLevel::Warn);

    FE_LOG_INFO("suppressed");
    FE_LOG_WARN("kept");

    REQUIRE(capture.entries().size() == 1);
    CHECK(capture.entries()[0].level == LogLevel::Warn);
    CHECK(capture.entries()[0].message == "kept");
}

TEST_CASE("log level names are stable") {
    CHECK(std::string_view(to_string(LogLevel::Trace)) == "trace");
    CHECK(std::string_view(to_string(LogLevel::Debug)) == "debug");
    CHECK(std::string_view(to_string(LogLevel::Info)) == "info");
    CHECK(std::string_view(to_string(LogLevel::Warn)) == "warn");
    CHECK(std::string_view(to_string(LogLevel::Error)) == "error");
    CHECK(std::string_view(to_string(LogLevel::Critical)) == "critical");
}
