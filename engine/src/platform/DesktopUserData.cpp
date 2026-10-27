// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "DesktopUserData.h"

#include "fe/core/FileSystem.h"
#include "fe/core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace fe {
namespace {

// The environment variable that overrides the per-user location, or null.
//
// This is the one place the engine names one, and it is inside the platform
// backend, which is where an OS-specific location rule belongs (ADR-0041). The
// variable differs per OS; the caller never sees either name.
[[nodiscard]] const char* data_home_variable() {
#if defined(FE_PLATFORM_WINDOWS)
    return "APPDATA";
#else
    return "XDG_DATA_HOME";
#endif
}

// The fallback used when the override variable is unset: the OS's conventional
// per-user data directory.
[[nodiscard]] std::string default_data_home() {
#if defined(FE_PLATFORM_WINDOWS)
    // Windows always sets APPDATA, so a miss here means a stripped environment;
    // fall back to the user profile so a save still has a home.
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
        return fs::join(profile, "AppData\\Roaming");
    }
    return {};
#elif defined(FE_PLATFORM_MACOS)
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::join(home, "Library/Application Support");
    }
    return {};
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::join(home, ".local/share");
    }
    return {};
#endif
}

// The per-user data directory for `app_id`, honouring root_override.
[[nodiscard]] std::string resolve_root(const UserData::Desc& desc) {
    const std::string base = !desc.root_override.empty() ? desc.root_override : [&] {
        if (const char* override_home = std::getenv(data_home_variable());
            override_home != nullptr && *override_home != '\0') {
            return std::string(override_home);
        }
        return default_data_home();
    }();
    if (base.empty()) {
        return {};
    }
    // A save belongs under a directory named for the game, not loose in the
    // user's data root, so two games never collide on a key.
    return desc.app_id.empty() ? base : fs::join(base, desc.app_id);
}

} // namespace

std::string DesktopUserData::path_for(std::string_view key) const {
    // One file per key. The key was validated before it got here, so it carries
    // no separator and cannot escape `root_`.
    return fs::join(root_, std::string(key) + ".dat");
}

Result<std::unique_ptr<DesktopUserData>> DesktopUserData::open(const Desc& desc) {
    const std::string root = resolve_root(desc);
    if (root.empty()) {
        return Error("user data: could not resolve a per-user data directory");
    }
    if (const Error e = fs::create_directories(root); !e.ok()) {
        return e;
    }
    return std::unique_ptr<DesktopUserData>(new DesktopUserData(root));
}

Result<std::vector<u8>> DesktopUserData::read(std::string_view key) const {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    return fs::read_binary_file(path_for(key));
}

Error DesktopUserData::write(std::string_view key, const void* data, usize size) {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    if (data == nullptr && size != 0) {
        return Error("user data: write of '" + std::string(key) + "' with a null pointer");
    }
    return fs::write_binary_file(path_for(key), data, size);
}

bool DesktopUserData::contains(std::string_view key) const {
    if (!is_valid_key(key)) {
        return false;
    }
    return fs::exists(path_for(key));
}

Error DesktopUserData::remove(std::string_view key) {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    // std::filesystem::remove returns false when the path is absent; that is a
    // success here, because the postcondition ("the key is gone") holds.
    std::error_code ec;
    std::filesystem::remove(path_for(key), ec);
    if (ec) {
        return Error("user data: could not remove '" + std::string(key) + "': " + ec.message());
    }
    return Error::none();
}

std::vector<std::string> DesktopUserData::keys() const {
    std::vector<std::string> out;
    for (const fs::DirectoryEntry& entry : fs::list_directory(root_)) {
        if (entry.is_directory) {
            continue;
        }
        const std::string& name = entry.name;
        // Only the files this store wrote: `<key>.dat`. Anything else in the
        // directory (a stray file, a future sidecar) is not a key.
        constexpr std::string_view suffix = ".dat";
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string key = name.substr(0, name.size() - suffix.size());
        if (is_valid_key(key)) {
            out.push_back(key);
        }
    }
    // list_directory does not promise an order, so sort for a deterministic
    // listing (the null backend is sorted too).
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace fe
