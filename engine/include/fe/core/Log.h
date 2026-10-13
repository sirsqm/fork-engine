// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <memory>
#include <string>
#include <string_view>

namespace fe {

// Severity levels, ordered from most to least verbose. A message is emitted
// when its level is at least as severe as the active minimum level.
enum class LogLevel : u8 {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Critical,
    Off,
};

[[nodiscard]] const char* to_string(LogLevel level) noexcept;

// A single destination for log records. Implement this to route engine logs
// into a file, an in-editor console, or an external telemetry sink.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogLevel level, std::string_view message) = 0;
};

// Process-wide logging state.
//
// The logger is intentionally global: logging is a cross-cutting concern and
// passing a logger through every constructor would pollute every interface in
// the engine. The state is guarded so that logs emitted from worker threads do
// not interleave.
namespace log {

void set_min_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel min_level() noexcept;
[[nodiscard]] bool is_enabled(LogLevel level) noexcept;

// Adds a sink. Ownership is shared; the sink stays alive until it is removed or
// the process exits. Passing nullptr is ignored.
void add_sink(std::shared_ptr<LogSink> sink);
void remove_sink(const std::shared_ptr<LogSink>& sink);
void clear_sinks();

// Writes a pre-formatted record to every sink. The default sink (stderr) is
// always present unless removed via clear_sinks().
void write(LogLevel level, std::string_view message);

} // namespace log

// printf-style logging. `format` is checked at compile time by the compiler's
// format attribute, which catches argument mismatches without runtime cost.
#if defined(__GNUC__) || defined(__clang__)
#define FE_LOG_ATTR __attribute__((format(printf, 2, 3)))
#else
#define FE_LOG_ATTR
#endif

void log_message(LogLevel level, const char* format, ...) FE_LOG_ATTR;

#define FE_LOG_TRACE(...) ::fe::log_message(::fe::LogLevel::Trace, __VA_ARGS__)
#define FE_LOG_DEBUG(...) ::fe::log_message(::fe::LogLevel::Debug, __VA_ARGS__)
#define FE_LOG_INFO(...) ::fe::log_message(::fe::LogLevel::Info, __VA_ARGS__)
#define FE_LOG_WARN(...) ::fe::log_message(::fe::LogLevel::Warn, __VA_ARGS__)
#define FE_LOG_ERROR(...) ::fe::log_message(::fe::LogLevel::Error, __VA_ARGS__)
#define FE_LOG_CRITICAL(...) ::fe::log_message(::fe::LogLevel::Critical, __VA_ARGS__)

} // namespace fe
