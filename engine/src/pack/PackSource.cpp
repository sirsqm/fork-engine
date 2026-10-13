// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/PackSource.h"

#include "fe/core/FileSystem.h"

#include <string>
#include <utility>
#include <vector>

namespace fe::pack {

Result<std::shared_ptr<PackSource>> PackSource::open(std::string_view path) {
    auto bytes = fs::read_binary_file(path);
    if (!bytes) {
        return bytes.error();
    }
    auto parsed = PackFile::parse(bytes.value().data(), bytes.value().size());
    if (!parsed) {
        return parsed.error();
    }
    return std::make_shared<PackSource>(std::move(parsed.value()));
}

Result<std::vector<u8>> PackSource::read_binary(std::string_view path) const {
    const PackEntry* entry = pack_.find_path(path);
    if (entry == nullptr) {
        return Error("pack: no entry '" + std::string(path) + "'");
    }
    return entry->data;
}

Result<std::string> PackSource::read_text(std::string_view path) const {
    const PackEntry* entry = pack_.find_path(path);
    if (entry == nullptr) {
        return Error("pack: no entry '" + std::string(path) + "'");
    }
    return std::string(reinterpret_cast<const char*>(entry->data.data()), entry->data.size());
}

bool PackSource::exists(std::string_view path) const {
    return pack_.find_path(path) != nullptr;
}

const ResourceSource::AssetLocation* PackSource::resolve_asset(const AssetId& id) const {
    const PackEntry* entry = pack_.find_guid(id);
    if (entry == nullptr) {
        return nullptr;
    }
    location_.path = entry->key;
    location_.scheme = entry->scheme;
    return &location_;
}

} // namespace fe::pack
