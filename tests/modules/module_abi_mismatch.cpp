// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// A gameplay module that reports the wrong ABI version (ADR-0061). The loader
// must refuse it before calling into it. It registers nothing and exports only
// the version and entry symbols a module is required to have.
#include "fe/script/ScriptModuleApi.h"
#include "fe/script/ScriptRegistry.h"

FE_SCRIPT_MODULE_EXPORT ::fe::u32 fe_script_module_abi_version() {
    return ::fe::kScriptModuleAbiVersion + 99;
}

FE_SCRIPT_MODULE_EXPORT ::fe::u32 fe_script_module_register(::fe::ScriptRegistry&) {
    return 0;
}
