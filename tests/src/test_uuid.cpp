// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "fe/core/Uuid.h"

#include <string>
#include <unordered_set>

using namespace fe;

TEST_CASE("generated uuids are not nil and are unique") {
    std::unordered_set<Uuid> seen;
    for (int i = 0; i < 1000; ++i) {
        const Uuid id = Uuid::generate();
        CHECK_FALSE(id.is_nil());
        CHECK(seen.insert(id).second);
    }
}

TEST_CASE("uuid string round-trips through parse") {
    const Uuid original = Uuid::generate();
    const std::string text = original.to_string();

    CHECK(text.size() == 36);
    CHECK(text[8] == '-');
    CHECK(text[13] == '-');
    CHECK(text[18] == '-');
    CHECK(text[23] == '-');

    Uuid parsed;
    REQUIRE(Uuid::try_parse(text, parsed));
    CHECK(parsed == original);
}

TEST_CASE("parse rejects malformed input without throwing") {
    Uuid parsed;
    CHECK_FALSE(Uuid::try_parse("", parsed));
    CHECK_FALSE(Uuid::try_parse("not-a-uuid", parsed));
    CHECK_FALSE(Uuid::try_parse("00000000-0000-0000-0000-00000000000", parsed));  // one short
    CHECK_FALSE(Uuid::try_parse("gggggggg-0000-0000-0000-000000000000", parsed)); // bad hex
}

TEST_CASE("the nil uuid is nil and round-trips") {
    const Uuid nil;
    CHECK(nil.is_nil());

    Uuid parsed;
    REQUIRE(Uuid::try_parse("00000000-0000-0000-0000-000000000000", parsed));
    CHECK(parsed.is_nil());
    CHECK(parsed == nil);
}
