// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"
#include "fe/pack/PackSource.h"
#include "fe/resources/ResourceSource.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fe::pack {

// A minimal synchronous HTTP/1.1 GET.
//
// Returns the response body for a 2xx status, and an Error for a malformed URL,
// a transport failure, or a non-2xx status (the status is named in the message).
// The connection is closed after one request, so a caller that needs several
// reads should expect a fresh connection each time.
//
// This is deliberately tiny: it exists so the pack source below can be tested
// against an in-process server without pulling a networking dependency into the
// build. It is not a general HTTP client, and it does not follow redirects or
// decode chunked transfer encoding - a static file server answers with
// Content-Length and closes, which is the case a shipped build meets.
[[nodiscard]] Result<std::string> http_get(std::string_view url);

// A ResourceSource that reads its pack over HTTP: the browser-shaped runtime
// mount.
//
// A shipped web build has no filesystem, so it cannot open its pack from disk
// the way `PackSource::open` does. This source fetches the pack once, at open
// time, and answers every subsequent read from the parsed pack, exactly as
// `PackSource` does - so the loaders and the scene reader serve the same
// requests they did on disk and nothing above the manager knows the difference.
//
// Preloading the whole pack at open is what keeps `fetch`'s asynchrony out of
// the synchronous `ResourceSource` contract: by the time a load is asked for,
// the bytes are already in memory. That is also what a shipped build wants (one
// request, no per-asset latency), and it matches `PackSource`'s "a pack is read
// whole" model. Streaming is later work, on both sources.
//
// A path the pack does not contain falls back to a plain HTTP GET of
// `base_url + path`, so a build that serves loose assets beside its pack still
// loads them; a 404 there is an Error, not an empty read.
class HttpFetchSource final : public ResourceSource {
public:
    // Fetches and parses the pack at `pack_path` (relative to `base_url`).
    // Returns an Error when the pack cannot be fetched or is not a valid pack,
    // so a broken deployment fails at mount time with a clear message rather
    // than at the first load.
    [[nodiscard]] static Result<std::shared_ptr<HttpFetchSource>> open(std::string base_url,
                                                                       std::string_view pack_path);

    // Wraps an already-parsed pack, mirroring `PackSource`'s value constructor.
    // The fallback fetches still use `base_url`.
    HttpFetchSource(std::shared_ptr<PackSource> pack, std::string base_url);

    [[nodiscard]] Result<std::vector<u8>> read_binary(std::string_view path) const override;
    [[nodiscard]] Result<std::string> read_text(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] const AssetLocation* resolve_asset(const AssetId& id) const override;

    [[nodiscard]] const PackFile& pack() const noexcept { return pack_->pack(); }
    [[nodiscard]] std::string_view base_url() const noexcept { return base_url_; }

private:
    std::shared_ptr<PackSource> pack_;
    // Always ends in '/', so joining a relative asset path is a plain append.
    std::string base_url_;
};

} // namespace fe::pack
