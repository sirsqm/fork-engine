// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/LogBuffer.h"

#include <memory>
#include <string>
#include <string_view>

using namespace fe;

namespace {

// Installs `buffer` as the only sink for the scope and restores the logger's
// sink set on teardown, so a test never leaks a sink into another test.
class ScopedSink {
public:
    explicit ScopedSink(std::shared_ptr<LogSink> sink) : sink_(std::move(sink)) {
        log::clear_sinks();
        log::add_sink(sink_);
    }

    ~ScopedSink() { log::clear_sinks(); }

private:
    std::shared_ptr<LogSink> sink_;
};

} // namespace

TEST_CASE("log buffer records what is written to it") {
    auto buffer = std::make_shared<LogBuffer>();
    ScopedSink sink(buffer);

    FE_LOG_INFO("first");
    FE_LOG_WARN("second");

    const std::vector<LogBuffer::Record> records = buffer->snapshot();
    REQUIRE(records.size() == 2);
    CHECK(records[0].level == LogLevel::Info);
    CHECK(records[0].message == "first");
    CHECK(records[1].level == LogLevel::Warn);
    CHECK(records[1].message == "second");
}

TEST_CASE("log buffer assigns strictly increasing sequences") {
    auto buffer = std::make_shared<LogBuffer>();
    ScopedSink sink(buffer);

    FE_LOG_INFO("a");
    FE_LOG_INFO("b");
    FE_LOG_INFO("c");

    const std::vector<LogBuffer::Record> records = buffer->snapshot();
    REQUIRE(records.size() == 3);
    CHECK(records[0].sequence < records[1].sequence);
    CHECK(records[1].sequence < records[2].sequence);
    CHECK(buffer->newest_sequence() == records[2].sequence);
}

TEST_CASE("log buffer is bounded and reports what it dropped") {
    auto buffer = std::make_shared<LogBuffer>(3);
    ScopedSink sink(buffer);

    for (int i = 0; i < 10; ++i) {
        FE_LOG_INFO("record %d", i);
    }

    const std::vector<LogBuffer::Record> records = buffer->snapshot();
    REQUIRE(records.size() == 3);
    // The three most recent survive, oldest first.
    CHECK(records[0].message == "record 7");
    CHECK(records[2].message == "record 9");
    CHECK(buffer->dropped_count() == 7);
}

TEST_CASE("log buffer newest sequence is zero when empty") {
    LogBuffer buffer;
    CHECK(buffer.newest_sequence() == 0);
    CHECK(buffer.size() == 0);
}

TEST_CASE("log buffer clear empties it and resets the drop count") {
    auto buffer = std::make_shared<LogBuffer>(2);
    ScopedSink sink(buffer);

    FE_LOG_INFO("a");
    FE_LOG_INFO("b");
    FE_LOG_INFO("c");
    REQUIRE(buffer->dropped_count() == 1);

    buffer->clear();
    CHECK(buffer->size() == 0);
    CHECK(buffer->dropped_count() == 0);
    CHECK(buffer->newest_sequence() == 0);
}

TEST_CASE("log buffer honours the level filter") {
    auto buffer = std::make_shared<LogBuffer>();
    ScopedSink sink(buffer);

    log::set_min_level(LogLevel::Warn);
    FE_LOG_INFO("suppressed");
    FE_LOG_ERROR("kept");

    const std::vector<LogBuffer::Record> records = buffer->snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records[0].message == "kept");

    log::set_min_level(LogLevel::Info);
}
