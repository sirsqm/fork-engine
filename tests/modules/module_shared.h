// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// Shared fixture for the two gameplay-module images the loader tests load
// (ADR-0061). Both versions register the same behaviour name and differ only in
// the step they advance a node's X position by, so the only thing a test observes
// after a hot reload is the new code.
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/scene/Node.h"
#include "fe/script/Script.h"
#include "fe/script/ScriptModuleApi.h"
#include "fe/systems/FrameContext.h"

#include <string_view>

// The probe counters each image exports are resolved by name through
// `DynamicLibrary`, exactly like the module ABI's own entry point. That is a
// dynamic lookup, so the symbol must be *exported*, not merely defined: on
// Windows a plain `extern "C"` definition stays internal to the image and
// `GetProcAddress` returns null, which is why this uses the module ABI's own
// export macro rather than a second, weaker one. The engine defines the macro
// for the same reason (ADR-0061); the fixtures inherit it.
#define FE_TEST_MODULE_PROBE_EXPORT FE_SCRIPT_MODULE_EXPORT

namespace fe_test_module {

// The behaviour both module versions register, so a reload rebinds the same node.
inline constexpr std::string_view kBehaviour = "SpinBehaviour";

// A behaviour that advances the attached node's X position by `step` per frame.
// attach()/update() are defined in each module image, so each image carries its
// own counters and its own step.
class SpinScript final : public fe::Script {
public:
    explicit SpinScript(fe::f32 step) noexcept : step_(step) {}

    [[nodiscard]] std::string_view name() const noexcept override { return kBehaviour; }

    fe::Error attach(fe::Node& node) override;
    fe::Error update(const fe::FrameContext& context) override;

    [[nodiscard]] fe::f32 step() const noexcept { return step_; }

private:
    fe::f32 step_;
};

} // namespace fe_test_module
