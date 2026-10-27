// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// The writable, per-user store a game saves its own data to.
//
// This is the "user data" row of the platform contract (ADR-0041): where a game
// writes what it owns. It is deliberately *not* `core::fs` -- that is the raw OS
// file API, and on the web it has no writable path at all. A game asks this seam
// to save a named blob and never learns whether that landed in a per-user
// directory on desktop or in the browser's localStorage (ADR-0096).
//
// The seam is a key/value store, not a filesystem: the caller names a datum
// ("best_score"), the store decides where it lives. That keeps the OS-specific
// location rule (XDG, %APPDATA%, localStorage) inside the backend, so nothing
// above the platform module names an environment variable or a browser API.
//
// A key is a name, not a path. Keys are restricted to `[A-Za-z0-9._-]`, must be
// non-empty, and may not be `.` or `..`, so a key can never escape the store.
// write() returns an Error for a key that breaks the rule rather than writing
// outside the store.
//
// A successful write() is durable: the bytes will be readable after a reload.
// On desktop that is a file write; on the web it is a synchronous localStorage
// write, which the backend confirms by reading the value back before it returns.
// A caller never sees a save that "succeeded" and then vanished, which is the
// one failure a persistence seam must not have.
//
// A store is obtained from the engine (Engine::user_data()); a tool or a test
// that wants no persistence creates one explicitly with the null backend. It is
// not constructed directly by gameplay code.
//
// Threading: a store is not internally synchronised. A game writes from the
// frame thread; a caller that saves from a job must serialise its own access.
class UserData {
public:
    // How a store is created. The engine fills this in from its config; a test
    // or a tool constructs one to ask for a specific backend.
    struct Desc {
        // Which backend to use. `Auto` picks the platform default: a per-user
        // directory on desktop, localStorage on web. `Null` keeps everything in
        // memory and persists nothing, which is what a test or a tool wants.
        enum class Backend { Auto, Null };
        Backend backend = Backend::Auto;

        // The application id the per-user directory is named after, e.g.
        // "breakout". Ignored by the null backend. An empty id with `Auto` also
        // yields the null backend, so a run that never set an id cannot write to
        // a shared location by accident.
        std::string app_id;

        // Overrides the per-user directory entirely. When set, `app_id` names a
        // subdirectory inside it rather than a directory under the OS location.
        // Used by a test to point the store at a temporary directory, and by a
        // host that wants a portable install to keep saves beside the game.
        std::string root_override;
    };

    // Creates a store for `desc`. This is a factory rather than a constructor so
    // the backend is chosen at compile time exactly as the window backend is
    // (ADR-0074): a web build compiles the localStorage backend and never sees
    // the desktop one, and neither leaks into this header.
    //
    // Never fails for a reason a caller must handle: a store whose directory
    // cannot be created logs the failure and falls back to the null backend, so
    // a game that saves still runs (its save is simply not persisted). The
    // returned store always reports the backend it actually got through
    // backend_name().
    [[nodiscard]] static std::unique_ptr<UserData> create(const Desc& desc);

    UserData() = default;
    virtual ~UserData();

    UserData(const UserData&) = delete;
    UserData& operator=(const UserData&) = delete;

    // The key's bytes, or an Error naming the key when it is absent. A read of a
    // missing key is an ordinary result, not a programming error: a first run has
    // no save. A malformed key is likewise an Error, not an assertion.
    [[nodiscard]] virtual Result<std::vector<u8>> read(std::string_view key) const = 0;

    // Reads the key as text. Convenience over read() for the common case of a
    // small text or JSON blob. Not virtual: it is read() plus a copy into a
    // string.
    [[nodiscard]] Result<std::string> read_text(std::string_view key) const;

    // Writes (creating or replacing). Durable before it returns on every backend.
    [[nodiscard]] virtual Error write(std::string_view key, const void* data, usize size) = 0;
    [[nodiscard]] Error write(std::string_view key, std::string_view text);

    // True when the key is present.
    [[nodiscard]] virtual bool contains(std::string_view key) const = 0;

    // Removes the key. Removing a key that is absent succeeds: the postcondition
    // ("the key is gone") holds either way.
    [[nodiscard]] virtual Error remove(std::string_view key) = 0;

    // The keys present, sorted, so a listing is deterministic for a test.
    [[nodiscard]] virtual std::vector<std::string> keys() const = 0;

    // Where the data lives, for a log line or an "open save folder" button.
    // Empty for a backend with no user-visible location (web, null).
    [[nodiscard]] virtual const std::string& root() const noexcept = 0;

    // "desktop", "web", or "null". The one honest answer to "did this actually
    // persist?".
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;

    // True when a valid key: non-empty, and only `[A-Za-z0-9._-]`, and not `.`
    // or `..`. Exposed so a test can pin the rule and a caller can validate a
    // key before building it.
    [[nodiscard]] static bool is_valid_key(std::string_view key) noexcept;

protected:
    // A backend calls this at the top of every method so the key rule is stated
    // once. Returns an Error naming the key when it is malformed, and none when
    // it is fine.
    [[nodiscard]] static Error check_key(std::string_view key);
};

} // namespace fe
