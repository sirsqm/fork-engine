// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "NullUserData.h"

#include <algorithm>
#include <cstring>

namespace fe {

Result<std::vector<u8>> NullUserData::read(std::string_view key) const {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return Error("user data: no key '" + std::string(key) + "'");
    }
    return it->second;
}

Error NullUserData::write(std::string_view key, const void* data, usize size) {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    if (data == nullptr && size != 0) {
        return Error("user data: write of '" + std::string(key) + "' with a null pointer");
    }
    std::vector<u8> bytes(size);
    if (size != 0) {
        std::memcpy(bytes.data(), data, size);
    }
    entries_[std::string(key)] = std::move(bytes);
    return Error::none();
}

bool NullUserData::contains(std::string_view key) const {
    if (!is_valid_key(key)) {
        return false;
    }
    return entries_.find(key) != entries_.end();
}

Error NullUserData::remove(std::string_view key) {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    entries_.erase(std::string(key));
    return Error::none();
}

std::vector<std::string> NullUserData::keys() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [key, value] : entries_) {
        out.push_back(key);
    }
    return out;
}

const std::string& NullUserData::root() const noexcept {
    return empty_root_;
}

const char* NullUserData::backend_name() const noexcept {
    return "null";
}

} // namespace fe
