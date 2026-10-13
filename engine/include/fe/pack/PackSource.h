// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/pack/PackFile.h"
#include "fe/resources/ResourceSource.h"

#include <memory>
#include <string_view>

namespace fe::pack {

// A ResourceSource backed by a pack: the runtime mount.
//
// Mounting one on a ResourceManager (`ResourceManager::mount`) makes every load
// read from the pack instead of disk, so the loaders and the scene reader serve
// the same requests they did for a loose tree. This is the "read from a pack as
// transparently as from disk" half of
// docs/adr/0018-multi-platform-shipping.md; the format and the decision are in
// docs/adr/0030-asset-pack-format.md.
//
// The source owns the parsed pack, so it is held through a shared_ptr and stays
// alive as long as the manager holds it. Reads are answered from the in-memory
// payload (a pack is read whole; streaming is later work).
class PackSource final : public ResourceSource {
public:
    // Reads and parses a pack file. Returns an Error when the file cannot be
    // read or is not a valid pack, so a bad pack fails at mount time with a
    // clear message rather than at the first load.
    [[nodiscard]] static Result<std::shared_ptr<PackSource>> open(std::string_view path);

    // Wraps an already-parsed pack.
    explicit PackSource(PackFile pack) : pack_(std::move(pack)) {}

    [[nodiscard]] Result<std::vector<u8>> read_binary(std::string_view path) const override;
    [[nodiscard]] Result<std::string> read_text(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] const AssetLocation* resolve_asset(const AssetId& id) const override;

    [[nodiscard]] const PackFile& pack() const noexcept { return pack_; }

private:
    PackFile pack_;
    // A resolved location is returned by pointer, so the last one is cached
    // here. A source is read from one thread (the RHI contract already states a
    // device is owned by one thread, and the manager follows it), so a single
    // slot is enough.
    mutable AssetLocation location_;
};

} // namespace fe::pack
