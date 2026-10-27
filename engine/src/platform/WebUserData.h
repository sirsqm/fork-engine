// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/platform/UserData.h"

#include <memory>
#include <string>
#include <vector>

namespace fe {

// The web user-data backend: the browser's localStorage.
//
// This is the half that makes the storage seam more than a desktop helper
// (ADR-0096). A browser has no filesystem, and Emscripten's default MEMFS is
// discarded when the tab reloads, so a naive "write a file" saves nothing across
// a reload -- the exact thing a save is for.
//
// localStorage is chosen over IndexedDB (and therefore over Emscripten's IDBFS)
// because it is *synchronous*. The seam promises that a successful write() is
// durable before it returns, and localStorage is the only browser store that can
// honour that without `-sASYNCIFY`: an IndexedDB write is asynchronous, so a
// synchronous `write()` built on it would either block the whole page through
// asyncify or return before the bytes reached the store -- a save that silently
// does not survive a reload, which is the one failure this seam must not have.
//
// localStorage holds strings, so a value is base64-encoded on the way in and
// decoded on the way out; the seam stays byte-exact and the encoding is an
// implementation detail of this backend. Keys are namespaced by application id
// so two games served from one origin do not collide.
//
// Internal to the platform module: a caller selects it with
// `UserData::Desc::backend = Backend::Auto` on a web build and receives a
// `UserData`.
class WebUserData final : public UserData {
public:
    ~WebUserData() override = default;

    WebUserData(const WebUserData&) = delete;
    WebUserData& operator=(const WebUserData&) = delete;

    [[nodiscard]] Result<std::vector<u8>> read(std::string_view key) const override;
    [[nodiscard]] Error write(std::string_view key, const void* data, usize size) override;
    [[nodiscard]] bool contains(std::string_view key) const override;
    [[nodiscard]] Error remove(std::string_view key) override;
    [[nodiscard]] std::vector<std::string> keys() const override;
    // localStorage is not a place a user can open, so there is no user-visible
    // location to report.
    [[nodiscard]] const std::string& root() const noexcept override { return empty_root_; }
    [[nodiscard]] const char* backend_name() const noexcept override { return "web"; }

    // Returns the store for `app_id`. Returns an Error when the browser has no
    // usable localStorage (private mode with storage disabled, or a page with
    // storage blocked), which is the caller's signal to fall back to the null
    // backend.
    [[nodiscard]] static Result<std::unique_ptr<WebUserData>> open(std::string_view app_id);

private:
    explicit WebUserData(std::string prefix) : prefix_(std::move(prefix)) {}

    // The localStorage key for a store key: `<prefix>/<key>`.
    [[nodiscard]] std::string storage_key(std::string_view key) const;

    std::string prefix_;
    std::string empty_root_;
};

} // namespace fe
