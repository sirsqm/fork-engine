// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/platform/DynamicLibrary.h"

// The one place in the engine that names a native dynamic-loading API. It is
// the "dynamic libraries" row of the platform contract (ADR-0041).
#if defined(FE_PLATFORM_WINDOWS)
// WIN32_LEAN_AND_MEAN and NOMINMAX are set directory-wide by FESettings.cmake.
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <string>
#include <utility>

namespace fe {
namespace {

#if defined(FE_PLATFORM_WINDOWS)
constexpr const char* kSharedSuffix = ".dll";
#elif defined(FE_PLATFORM_MACOS)
constexpr const char* kSharedSuffix = ".dylib";
#else
constexpr const char* kSharedSuffix = ".so";
#endif

} // namespace

DynamicLibrary::~DynamicLibrary() {
    unload();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)) {
    other.handle_ = nullptr;
    other.path_.clear();
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        handle_ = other.handle_;
        path_ = std::move(other.path_);
        other.handle_ = nullptr;
        other.path_.clear();
    }
    return *this;
}

Result<DynamicLibrary> DynamicLibrary::load(std::string_view path) {
    if (path.empty()) {
        return Error("DynamicLibrary::load: empty path");
    }

    std::string owned(path);
#if defined(FE_PLATFORM_WINDOWS)
    HMODULE module = ::LoadLibraryA(owned.c_str());
    if (module == nullptr) {
        return Error("DynamicLibrary::load: could not open '" + owned + "' (error " +
                     std::to_string(static_cast<unsigned long>(::GetLastError())) + ")");
    }
    return DynamicLibrary(static_cast<void*>(module), std::move(owned));
#else
    // RTLD_NOW resolves every symbol up front, so a module with a missing
    // dependency fails at load with a clear error rather than at first call.
    // RTLD_LOCAL keeps the module's symbols out of the process-global namespace,
    // so two modules cannot collide by exporting the same name.
    void* handle = ::dlopen(owned.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* message = ::dlerror();
        return Error("DynamicLibrary::load: could not open '" + owned +
                     "': " + (message != nullptr ? message : "unknown error"));
    }
    return DynamicLibrary(handle, std::move(owned));
#endif
}

void* DynamicLibrary::symbol(std::string_view name) const {
    if (handle_ == nullptr || name.empty()) {
        return nullptr;
    }
    std::string owned(name);
#if defined(FE_PLATFORM_WINDOWS)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle_), owned.c_str()));
#else
    return ::dlsym(handle_, owned.c_str());
#endif
}

void DynamicLibrary::unload() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#if defined(FE_PLATFORM_WINDOWS)
    ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
    path_.clear();
}

bool DynamicLibrary::has_shared_suffix(std::string_view name) noexcept {
    const std::string_view suffix(kSharedSuffix);
    return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
}

std::string DynamicLibrary::decorate(std::string_view name) {
    if (has_shared_suffix(name)) {
        return std::string(name);
    }
    return std::string(name) + kSharedSuffix;
}

} // namespace fe
