// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/Uuid.h"

#include <random>
#include <string_view>

namespace fe {
namespace {

// A process-wide generator. std::random_device seeds it once; subsequent calls
// are fast. This is a uniqueness mechanism, not a cryptographic one.
std::mt19937_64& rng() {
    static std::mt19937_64 engine = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    return engine;
}

[[nodiscard]] u8 hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<u8>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<u8>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<u8>(c - 'A' + 10);
    }
    return 0xFF;
}

} // namespace

Uuid Uuid::generate() {
    Uuid id;
    std::uniform_int_distribution<u32> dist(0, 255);
    for (auto& byte : id.bytes_) {
        byte = static_cast<u8>(dist(rng()));
    }
    // Mark as a random (version 4) UUID so the value is self-describing.
    id.bytes_[6] = static_cast<u8>((id.bytes_[6] & 0x0F) | 0x40);
    id.bytes_[8] = static_cast<u8>((id.bytes_[8] & 0x3F) | 0x80);
    return id;
}

bool Uuid::try_parse(std::string_view text, Uuid& out) noexcept {
    // Accept exactly 36 characters: 32 hex digits plus 4 dashes at the
    // canonical positions.
    if (text.size() != 36) {
        return false;
    }
    constexpr std::size_t kDashPositions[] = {8, 13, 18, 23};
    std::size_t dash_index = 0;
    std::size_t byte_index = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (dash_index < 4 && i == kDashPositions[dash_index]) {
            if (text[i] != '-') {
                return false;
            }
            ++dash_index;
            continue;
        }
        const u8 value = hex_value(text[i]);
        if (value == 0xFF) {
            return false;
        }
        // Two hex digits per byte, high nibble first.
        if (byte_index % 2 == 0) {
            out.bytes_[byte_index / 2] = static_cast<u8>(value << 4);
        } else {
            out.bytes_[byte_index / 2] = static_cast<u8>(out.bytes_[byte_index / 2] | value);
        }
        ++byte_index;
    }
    return byte_index == 32 && dash_index == 4;
}

Uuid Uuid::from_bytes(const std::array<u8, 16>& bytes) noexcept {
    Uuid id;
    id.bytes_ = bytes;
    return id;
}

bool Uuid::is_nil() const noexcept {
    for (const u8 byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string Uuid::to_string() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            result.push_back('-');
        }
        result.push_back(kHex[bytes_[i] >> 4]);
        result.push_back(kHex[bytes_[i] & 0x0F]);
    }
    return result;
}

} // namespace fe
