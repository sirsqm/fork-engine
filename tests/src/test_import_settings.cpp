// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/resources/ImportSettings.h"

#include <string>

using namespace fe;

TEST_CASE("generic settings are default and serialize as a bare kind") {
    const ImportSettings settings;
    CHECK(settings.kind == ImportSettings::Kind::Generic);
    CHECK(settings.is_default());

    const std::string json = import_settings_to_json(settings);
    auto parsed = import_settings_from_json(json);
    REQUIRE(parsed);
    CHECK(parsed.value().kind == ImportSettings::Kind::Generic);
    CHECK(parsed.value().is_default());
}

TEST_CASE("texture settings round-trip through JSON") {
    ImportSettings settings = ImportSettings::texture_defaults();
    settings.filter = rhi::TextureFilter::Nearest;
    settings.wrap = rhi::TextureWrap::Repeat;
    settings.mipmaps = true;
    CHECK_FALSE(settings.is_default());

    auto parsed = import_settings_from_json(import_settings_to_json(settings));
    REQUIRE(parsed);
    CHECK(parsed.value().kind == ImportSettings::Kind::Texture);
    CHECK(parsed.value().filter == rhi::TextureFilter::Nearest);
    CHECK(parsed.value().wrap == rhi::TextureWrap::Repeat);
    CHECK(parsed.value().mipmaps);
}

TEST_CASE("texture defaults reproduce the pre-import-settings behaviour") {
    const ImportSettings settings = ImportSettings::texture_defaults();
    CHECK(settings.kind == ImportSettings::Kind::Texture);
    CHECK(settings.filter == rhi::TextureFilter::Linear);
    CHECK(settings.wrap == rhi::TextureWrap::ClampToEdge);
    CHECK_FALSE(settings.mipmaps);
    // All-default texture settings are "default", so a sidecar can omit them.
    CHECK(settings.is_default());
}

TEST_CASE("a document missing optional fields takes their defaults") {
    auto parsed = import_settings_from_json(R"({"kind":"texture"})");
    REQUIRE(parsed);
    CHECK(parsed.value().filter == rhi::TextureFilter::Linear);
    CHECK(parsed.value().wrap == rhi::TextureWrap::ClampToEdge);
    CHECK_FALSE(parsed.value().mipmaps);
}

TEST_CASE("settings kinds have stable names and unknown names are rejected") {
    CHECK(std::string(to_string(ImportSettings::Kind::Generic)) == "generic");
    CHECK(std::string(to_string(ImportSettings::Kind::Texture)) == "texture");

    auto generic = import_kind_from_string("generic");
    REQUIRE(generic);
    CHECK(generic.value() == ImportSettings::Kind::Generic);

    auto texture = import_kind_from_string("texture");
    REQUIRE(texture);
    CHECK(texture.value() == ImportSettings::Kind::Texture);

    auto unknown = import_kind_from_string("mesh");
    CHECK_FALSE(unknown);
    CHECK(unknown.error().message().find("unknown import settings kind") != std::string::npos);
}

TEST_CASE("a malformed settings document is rejected, not silently defaulted") {
    auto not_json = import_settings_from_json("{ not json");
    CHECK_FALSE(not_json);
    CHECK(not_json.error().message().find("parse error") != std::string::npos);

    auto not_object = import_settings_from_json("[1, 2, 3]");
    CHECK_FALSE(not_object);
    CHECK(not_object.error().message().find("must be a JSON object") != std::string::npos);

    auto bad_kind = import_settings_from_json(R"({"kind":"nonsense"})");
    CHECK_FALSE(bad_kind);
    CHECK(bad_kind.error().message().find("unknown import settings kind") != std::string::npos);

    auto bad_filter = import_settings_from_json(R"({"kind":"texture","filter":"cubic"})");
    CHECK_FALSE(bad_filter);
    CHECK(bad_filter.error().message().find("unknown texture filter") != std::string::npos);

    auto bad_wrap = import_settings_from_json(R"({"kind":"texture","wrap":"mirror"})");
    CHECK_FALSE(bad_wrap);
    CHECK(bad_wrap.error().message().find("unknown texture wrap") != std::string::npos);
}
