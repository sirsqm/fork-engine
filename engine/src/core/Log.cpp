// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/Log.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <vector>

namespace fe {
namespace {

std::mutex g_mutex;
LogLevel g_min_level = LogLevel::Info;
std::vector<std::shared_ptr<LogSink>> g_sinks;

// Writes to stderr. Always available unless the user clears all sinks.
class StderrSink final : public LogSink {
public:
    void write(LogLevel level, std::string_view message) override {
        std::fputs(to_string(level), stderr);
        std::fputs(": ", stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
    }
};

std::shared_ptr<LogSink>& default_sink() {
    static std::shared_ptr<LogSink> sink = std::make_shared<StderrSink>();
    return sink;
}

} // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    case LogLevel::Off:
        return "off";
    }
    return "unknown";
}

namespace log {

void set_min_level(LogLevel level) noexcept {
    std::lock_guard lock(g_mutex);
    g_min_level = level;
}

LogLevel min_level() noexcept {
    std::lock_guard lock(g_mutex);
    return g_min_level;
}

bool is_enabled(LogLevel level) noexcept {
    std::lock_guard lock(g_mutex);
    return level >= g_min_level && g_min_level != LogLevel::Off;
}

void add_sink(std::shared_ptr<LogSink> sink) {
    if (!sink) {
        return;
    }
    std::lock_guard lock(g_mutex);
    g_sinks.push_back(std::move(sink));
}

void remove_sink(const std::shared_ptr<LogSink>& sink) {
    std::lock_guard lock(g_mutex);
    for (auto it = g_sinks.begin(); it != g_sinks.end(); ++it) {
        if (it->get() == sink.get()) {
            g_sinks.erase(it);
            return;
        }
    }
}

void clear_sinks() {
    std::lock_guard lock(g_mutex);
    g_sinks.clear();
}

void write(LogLevel level, std::string_view message) {
    std::lock_guard lock(g_mutex);
    if (level < g_min_level || g_min_level == LogLevel::Off) {
        return;
    }
    default_sink()->write(level, message);
    for (const auto& sink : g_sinks) {
        sink->write(level, message);
    }
}

} // namespace log

void log_message(LogLevel level, const char* format, ...) {
    if (!log::is_enabled(level)) {
        return;
    }

    // Fast path: most records fit in this buffer, so no allocation occurs.
    std::array<char, 1024> stack_buffer{};

    va_list args;
    va_start(args, format);
    const int needed = std::vsnprintf(stack_buffer.data(), stack_buffer.size(), format, args);
    va_end(args);

    if (needed < 0) {
        log::write(level, "<log formatting failed>");
        return;
    }

    const auto size = static_cast<std::size_t>(needed);
    if (size < stack_buffer.size()) {
        log::write(level, std::string_view(stack_buffer.data(), size));
        return;
    }

    std::string heap_buffer;
    heap_buffer.resize(size + 1);
    va_start(args, format);
    std::vsnprintf(heap_buffer.data(), heap_buffer.size(), format, args);
    va_end(args);
    heap_buffer.resize(size);
    log::write(level, heap_buffer);
}

} // namespace fe
