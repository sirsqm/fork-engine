// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/resources/ImportSettings.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fe {

// A content-addressed store of imported artifacts.
//
// An import is a pure function of its inputs: the same source bytes, the same
// import settings, and the same importer always produce the same artifact. So
// the artifact can be stored under a key derived from those inputs and reused,
// which turns a re-import (in this run or a later one) into a cache read instead
// of a re-decode. See docs/adr/0029-import-cache.md.
//
// What it is not: an asset index. It stores *derived* data keyed by content, and
// it holds no identity, no sidecar, and no notion of "an asset". The
// AssetDatabase owns identity (ADR-0020); this owns cached importer output. A
// cache root is disposable by construction - deleting it costs only a re-import.
//
// A cache with no root set is inert: is_enabled() is false, contains() is false,
// and store() does nothing. That is what lets a headless tool, a test, or a
// project with no cache root behave exactly as it did before this existed, the
// same guarantee the AssetDatabase gives for an unset root.
class ImportCache {
public:
    ImportCache() = default;

    ImportCache(const ImportCache&) = delete;
    ImportCache& operator=(const ImportCache&) = delete;

    // Sets the cache root. An empty root makes the cache inert. The root is
    // normalized; directories are created lazily on the first store.
    void set_root(std::string root);
    [[nodiscard]] const std::string& root() const noexcept { return root_; }

    // True when a root is set. Every operation is a no-op otherwise.
    [[nodiscard]] bool is_enabled() const noexcept { return !root_.empty(); }

    // The cache key for an import, as a 16-character lowercase hex string.
    //
    // The key hashes the source bytes, the effective import settings, and the
    // importer identity, so a change to any of the three yields a different key
    // and therefore a cache miss - invalidation is a consequence of the key, not
    // a step to remember. `importer` is the scheme that owns the file ("tex", or
    // "" for the default scheme), so two importers sharing an extension never
    // collide. `settings` may be null, meaning "the importer's defaults".
    [[nodiscard]] static std::string key_for(std::span<const u8> source_bytes, const ImportSettings* settings,
                                             std::string_view importer);

    // The content hash of a byte range, as a 16-character lowercase hex string.
    // Exposed because it is a useful pure function on its own (a change check, a
    // future deduplicating store) and it is what key_for hashes into.
    [[nodiscard]] static std::string content_hash(std::span<const u8> bytes);

    // True when a valid artifact is stored under `key`. A malformed or
    // mismatched artifact reads as absent, so a corrupted cache is a miss rather
    // than a wrong result.
    [[nodiscard]] bool contains(std::string_view key) const;

    // Reads the artifact stored under `key`. Fails when the cache is inert, the
    // entry is absent, or the entry is not a valid artifact for `key`.
    [[nodiscard]] Result<std::vector<u8>> fetch(std::string_view key) const;

    // Stores `artifact` under `key`, creating the cache directory as needed.
    // Overwrites an existing entry (the key is a function of the content, so a
    // rewrite is the same artifact). A no-op when the cache is inert.
    [[nodiscard]] Error store(std::string_view key, std::span<const u8> artifact);

    // The absolute path of the artifact file for `key`, whether or not it
    // exists. Exposed for tests and diagnostics.
    [[nodiscard]] std::string artifact_path(std::string_view key) const;

private:
    std::string root_;
};

} // namespace fe
