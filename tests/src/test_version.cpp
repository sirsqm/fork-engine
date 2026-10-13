// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/serialization/SceneSerializer.h"
#include "fe/version/Version.h"

#include <string>

using namespace fe;

TEST_CASE("the engine reports a well-formed semantic version") {
    CHECK(version::kEngineVersion.major >= 0);
    CHECK(version::kEngineVersion.minor >= 0);
    CHECK(version::kEngineVersion.patch >= 0);

    const std::string expected = std::to_string(version::kEngineVersion.major) + "." +
                                 std::to_string(version::kEngineVersion.minor) + "." +
                                 std::to_string(version::kEngineVersion.patch);
    CHECK(std::string(version::kEngineVersionString) == expected);
}

TEST_CASE("the scene format version is positive") {
    CHECK(serialization::scene::kFormatVersion >= 1);
}
