// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"
#include "fe/script/ScriptRegistry.h"

// The ABI a C++ gameplay module and the engine agree on.
//
// A gameplay module is a shared library built against the engine that registers
// its behaviours into a ScriptRegistry (M7, ADR-0044, ADR-0061). This header is
// the whole of the contract between the two sides: the exported entry point, the
// version of the contract, and the macro that emits both. A module includes this
// header, defines one registration function, and uses FE_SCRIPT_MODULE; the
// engine's ScriptLibrary opens the library, checks the version, calls the entry
// point, and reads the behaviours back out of the registry.
//
// The contract is deliberately tiny and versioned. It crosses a binary boundary,
// so it must change rarely and detectably: an engine that meets a module built
// against a different version refuses it with a clear error instead of calling
// into it and corrupting memory. Adding a behaviour or a component to the
// registration does not change the ABI; changing the entry point's signature
// does, and bumps the version.

namespace fe {

// The ABI version of the module contract. Bump it whenever the entry point's
// signature or the meaning of its arguments changes; the engine refuses a module
// whose version does not match.
inline constexpr u32 kScriptModuleAbiVersion = 1;

// The exported entry point. It registers the module's behaviours into `registry`
// and returns the number of behaviours it registered. It is called once, while
// the library is being loaded, on the thread doing the loading.
using ScriptModuleEntry = u32 (*)(ScriptRegistry&);

} // namespace fe

// The linkage and visibility a module's exports need. Extern "C" so the name is
// not mangled and the engine can find it by a fixed string; default visibility so
// it survives a -fvisibility=hidden build.
#if defined(_WIN32) || defined(__CYGWIN__)
#define FE_SCRIPT_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define FE_SCRIPT_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Emits the two symbols a gameplay module exports: the ABI version, and the
// registration entry point that calls `entry_function`.
//
// Usage, at file scope in the module:
//
//     static fe::u32 register_my_module(fe::ScriptRegistry& registry) {
//         registry.register_type<MyScript>("MyScript");
//         return 1;
//     }
//     FE_SCRIPT_MODULE(register_my_module)
#define FE_SCRIPT_MODULE(entry_function)                                                                               \
    FE_SCRIPT_MODULE_EXPORT ::fe::u32 fe_script_module_abi_version() {                                                 \
        return ::fe::kScriptModuleAbiVersion;                                                                          \
    }                                                                                                                  \
    FE_SCRIPT_MODULE_EXPORT ::fe::u32 fe_script_module_register(::fe::ScriptRegistry& registry) {                      \
        return entry_function(registry);                                                                               \
    }
