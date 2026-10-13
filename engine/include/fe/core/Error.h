// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace fe {

// A recoverable error. Errors are values, not exceptions: the engine's hot
// paths (rendering, resource streaming) must not pay for unwinding, and
// explicit propagation keeps failure modes visible in signatures.
//
// Conventions:
//   * Return `Error` (or a `Result<T>`) from functions that can fail for
//     ordinary, expected reasons (missing file, bad asset, invalid argument).
//   * Reserve FE_ASSERT / FE_CHECK for programming errors that must never
//     happen, and exceptions for nothing at all.
class Error {
public:
    Error() = default;

    explicit Error(std::string message) : message_(std::move(message)) {}

    [[nodiscard]] bool ok() const noexcept { return message_.empty(); }
    [[nodiscard]] explicit operator bool() const noexcept { return !message_.empty(); }

    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string_view view() const noexcept { return message_; }

    void clear() { message_.clear(); }

    [[nodiscard]] static Error none() { return Error{}; }

private:
    std::string message_;
};

// A value-or-error. Construct from `T` for success, from `Error` for failure.
//
// Example:
//     Result<int> parse(std::string_view text);
//     auto r = parse("42");
//     if (!r) { FE_LOG_ERROR("%s", r.error().message().c_str()); }
//     else    { use(r.value()); }
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)), error_() {}
    Result(Error error) : value_(), error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() noexcept { return value_; }
    [[nodiscard]] const T& value() const noexcept { return value_; }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    T value_{};
    Error error_;
};

// Specialisation for functions that either succeed or fail with no value.
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }

private:
    Error error_;
};

} // namespace fe
