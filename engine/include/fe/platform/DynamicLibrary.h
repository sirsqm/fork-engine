// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string>
#include <string_view>
#include <utility>

namespace fe {

// A dynamically loaded library (a shared object / DLL).
//
// This is the "dynamic libraries" row of the platform contract (ADR-0041). It
// is the OS-specific half of loading a C++ gameplay module (M7, ADR-0061):
// every dlopen/LoadLibrary detail lives here, so nothing above the platform
// module names a native API. The surface is deliberately the smallest one a
// module loader needs — open a file, resolve a symbol by name, close it — and it
// makes no assumption about what is inside the library.
//
// A DynamicLibrary is move-only. Create one with load(); the image stays loaded
// until it is closed with unload() or the owning DynamicLibrary is destroyed.
//
// Threading: an image may be opened and closed from any single thread, but
// loading or unloading an image that another thread is executing code from is a
// use-after-free the caller must prevent. The module loader closes an image only
// after every script that lived in it has been destroyed (ADR-0061).
class DynamicLibrary {
public:
    DynamicLibrary() = default;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    // Opens the image at `path`. On failure returns an Error carrying the OS
    // message; the caller can then try a decorated name via decorate().
    [[nodiscard]] static Result<DynamicLibrary> load(std::string_view path);

    // True when an image is currently open.
    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

    // Resolves an exported symbol. Returns nullptr when the library is closed
    // or the symbol is absent. A missing symbol is a normal lookup result, not
    // an error, because a loader tries several known names in turn.
    [[nodiscard]] void* symbol(std::string_view name) const;

    // A typed convenience over symbol(). `T` is normally a function-pointer type
    // or a plain pointer-to-object type.
    template <typename T>
    [[nodiscard]] T symbol_as(std::string_view name) const {
        return reinterpret_cast<T>(symbol(name));
    }

    // Closes the image if open. Idempotent.
    void unload() noexcept;

    // The path this image was opened from, or empty.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // True if `name` already ends in this platform's shared-library suffix
    // (".so" on Linux, ".dylib" on macOS, ".dll" on Windows).
    [[nodiscard]] static bool has_shared_suffix(std::string_view name) noexcept;

    // Returns `name` with this platform's shared-library suffix appended when it
    // does not already end in one, so a caller can turn a module's logical name
    // into a filename without naming the OS.
    [[nodiscard]] static std::string decorate(std::string_view name);

private:
    explicit DynamicLibrary(void* handle, std::string path) noexcept : handle_(handle), path_(std::move(path)) {}

    void* handle_ = nullptr;
    std::string path_;
};

} // namespace fe
