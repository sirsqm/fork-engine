// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace fe {

// A 128-bit universally unique identifier used to give scenes, nodes, and
// resources identity that survives renaming, moving, and reloading.
//
// Stored as 16 raw bytes. Formatted as the canonical lowercase
// `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` string. Value type: cheap to copy,
// comparable, and hashable.
class Uuid {
public:
    Uuid() = default;

    [[nodiscard]] static Uuid generate();

    // Parses the canonical textual form. Returns std::nullopt on malformed
    // input. Parsing never throws.
    [[nodiscard]] static bool try_parse(std::string_view text, Uuid& out) noexcept;

    [[nodiscard]] const std::array<u8, 16>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_nil() const noexcept;

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const Uuid&) const noexcept = default;

private:
    std::array<u8, 16> bytes_{};
};

} // namespace fe

namespace std {
template <> struct hash<fe::Uuid> {
    std::size_t operator()(const fe::Uuid& id) const noexcept {
        // FNV-1a over the 16 bytes. Cheap and adequate for hash maps.
        std::size_t h = 1469598103934665603ULL;
        for (const fe::u8 b : id.bytes()) {
            h ^= static_cast<std::size_t>(b);
            h *= 1099511628211ULL;
        }
        return h;
    }
};
} // namespace std
