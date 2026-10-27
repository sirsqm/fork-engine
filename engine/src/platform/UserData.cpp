// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/UserData.h"

#include "NullUserData.h"

// The persistent backend is chosen at compile time, from the platform macro that
// cmake/FEPlatform.cmake defines. On desktop it is a per-user directory; on web
// it is the browser's localStorage. The preprocessor branch is what keeps the
// desktop backend out of a web build entirely, where a filesystem does not exist,
// and keeps the browser API out of a desktop build. See
// docs/adr/0096-writable-user-data-storage.md.
#if defined(FE_PLATFORM_WEB)
#include "WebUserData.h"
#else
#include "DesktopUserData.h"
#endif

#include "fe/core/Log.h"

#include <algorithm>
#include <cstring>

namespace fe {

// The UserData base class: the factory, the key rule, and the two convenience
// overloads. This file is deliberately platform-free: it names no directory rule
// and no browser API, and it compiles unchanged for desktop and for web.
//
// The backend selection mirrors Window::create() (ADR-0074): a field on the
// descriptor picks the backend, and the concrete implementation lives in a
// translation unit the build only compiles for the platform it targets.

bool UserData::is_valid_key(std::string_view key) noexcept {
    if (key.empty() || key == "." || key == "..") {
        return false;
    }
    for (const char c : key) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '.' || c == '_' || c == '-';
        if (!safe) {
            return false;
        }
    }
    return true;
}

Error UserData::check_key(std::string_view key) {
    if (!is_valid_key(key)) {
        return Error("user data: invalid key '" + std::string(key) +
                     "' (use only letters, digits, '.', '_' and '-')");
    }
    return Error::none();
}

UserData::~UserData() = default;

Result<std::string> UserData::read_text(std::string_view key) const {
    auto bytes = read(key);
    if (!bytes) {
        return bytes.error();
    }
    const std::vector<u8>& data = bytes.value();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

Error UserData::write(std::string_view key, std::string_view text) {
    return write(key, text.data(), text.size());
}

std::unique_ptr<UserData> UserData::create(const Desc& desc) {
    // A run that asked for no persistence gets the in-memory store, and so does
    // one that never named an application: a game must not write to a shared
    // location by accident.
    const bool wants_null = desc.backend == Desc::Backend::Null || desc.app_id.empty();

#if defined(FE_PLATFORM_WEB)
    if (!wants_null) {
        auto store = WebUserData::open(desc.app_id);
        if (store) {
            return std::move(store.value());
        }
        FE_LOG_WARN("user data: falling back to the null store: %s", store.error().message().c_str());
    }
#else
    if (!wants_null) {
        auto store = DesktopUserData::open(desc);
        if (store) {
            return std::move(store.value());
        }
        FE_LOG_WARN("user data: falling back to the null store: %s", store.error().message().c_str());
    }
#endif

    return std::make_unique<NullUserData>();
}

} // namespace fe
