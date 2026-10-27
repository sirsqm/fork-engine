// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/UserData.h"

#include <map>
#include <string>
#include <vector>

namespace fe {

// The null user-data backend: a store that keeps everything in memory and
// persists nothing.
//
// It is the ADR-0041 "prove the contract with no OS" backend for the user-data
// row, and it is what a test and a tool use. A run that wants no persistence --
// a headless test, a benchmark, a tool that must not touch the developer's real
// home directory -- gets this rather than a surprise write to disk.
//
// A write to it always succeeds and a read of a key written in the same process
// returns it; nothing survives the process. root() is empty and backend_name()
// is "null", so a caller can tell that a save did not reach a disk.
//
// Internal to the platform module: a caller selects it with
// `UserData::Desc::backend = Backend::Null` and receives a `UserData`.
class NullUserData final : public UserData {
public:
    NullUserData() = default;
    ~NullUserData() override = default;

    NullUserData(const NullUserData&) = delete;
    NullUserData& operator=(const NullUserData&) = delete;

    [[nodiscard]] Result<std::vector<u8>> read(std::string_view key) const override;
    [[nodiscard]] Error write(std::string_view key, const void* data, usize size) override;
    [[nodiscard]] bool contains(std::string_view key) const override;
    [[nodiscard]] Error remove(std::string_view key) override;
    [[nodiscard]] std::vector<std::string> keys() const override;
    [[nodiscard]] const std::string& root() const noexcept override;
    [[nodiscard]] const char* backend_name() const noexcept override;

private:
    // std::map, not unordered_map: keys() is then sorted for free, which is what
    // makes a listing deterministic for a test.
    std::map<std::string, std::vector<u8>, std::less<>> entries_;
    std::string empty_root_;
};

} // namespace fe
