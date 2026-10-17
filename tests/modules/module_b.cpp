// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Version B of the test gameplay module (ADR-0061). Identical to version A
// except that it advances a node by 2.0 units per frame, so a hot reload that
// swaps A for B is observable in the node's position.
#include "module_shared.h"

#include "fe/script/ScriptModuleApi.h"
#include "fe/script/ScriptRegistry.h"

#include <memory>
#include <string>

namespace {
fe::u32 g_ticks = 0;
fe::u32 g_attaches = 0;
} // namespace

namespace fe_test_module {

fe::Error SpinScript::attach(fe::Node& node) {
    (void)node;
    ++g_attaches;
    return fe::Error::none();
}

fe::Error SpinScript::update(const fe::FrameContext& context) {
    (void)context;
    ++g_ticks;
    if (node() != nullptr) {
        node()->transform().position.x += 2.0f;
    }
    return fe::Error::none();
}

} // namespace fe_test_module

// Counters the test reads out of the loaded image to prove which code ran.
extern "C" fe::u32 fe_module_tick_count() {
    return g_ticks;
}

extern "C" fe::u32 fe_module_attach_count() {
    return g_attaches;
}

static fe::u32 register_module(fe::ScriptRegistry& registry) {
    registry.register_script(std::string(fe_test_module::kBehaviour),
                             [] { return std::make_unique<fe_test_module::SpinScript>(2.0f); });
    return 1;
}

FE_SCRIPT_MODULE(register_module)
