// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/UserData.h"

#include <memory>
#include <string>
#include <vector>

namespace fe {

// The desktop user-data backend: a real per-user directory, one file per key.
//
// This is the "user data" row of the platform contract (ADR-0041) on a machine
// with a filesystem. The location rule lives here and nowhere else, so no caller
// names an environment variable: `$XDG_DATA_HOME` or `~/.local/share` on Linux,
// `~/Library/Application Support` on macOS, `%APPDATA%` on Windows, each under an
// application id. `Desc::root_override` replaces the OS location entirely, which
// is what a test uses to point the store at a temporary directory (ADR-0096).
//
// A key is one file, so a key never names a path: the file is
// `<root>/<key>.dat` and a key is validated before it becomes a filename. A write
// is `core::fs::write_binary_file`, which is a real file write and therefore
// durable when it returns -- no extra flush step is needed on this backend.
//
// Internal to the platform module: a caller selects it with
// `UserData::Desc::backend = Backend::Auto` and receives a `UserData`.
class DesktopUserData final : public UserData {
public:
    ~DesktopUserData() override = default;

    DesktopUserData(const DesktopUserData&) = delete;
    DesktopUserData& operator=(const DesktopUserData&) = delete;

    [[nodiscard]] Result<std::vector<u8>> read(std::string_view key) const override;
    [[nodiscard]] Error write(std::string_view key, const void* data, usize size) override;
    [[nodiscard]] bool contains(std::string_view key) const override;
    [[nodiscard]] Error remove(std::string_view key) override;
    [[nodiscard]] std::vector<std::string> keys() const override;
    [[nodiscard]] const std::string& root() const noexcept override { return root_; }
    [[nodiscard]] const char* backend_name() const noexcept override { return "desktop"; }

    // Resolves the per-user directory for `app_id` (or `root_override`), creates
    // it, and returns the store. Returns an Error when no directory can be
    // resolved or created, which is the caller's signal to fall back to the null
    // backend rather than run without a place to save.
    [[nodiscard]] static Result<std::unique_ptr<DesktopUserData>> open(const Desc& desc);

private:
    explicit DesktopUserData(std::string root) : root_(std::move(root)) {}

    // `<root>/<key>.dat`. The key is assumed already validated.
    [[nodiscard]] std::string path_for(std::string_view key) const;

    std::string root_;
};

} // namespace fe
