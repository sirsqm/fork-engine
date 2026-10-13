// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/resources/ImportCache.h"

#include "fe/core/FileSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fe {
namespace {

// The key format version. Bumping it invalidates every existing entry at once,
// which is what a change to the artifact format or the hash itself needs: a
// reader never has to interpret an entry written by an older engine.
constexpr std::string_view kKeyVersion = "fork-import-v1";

// FNV-1a 64-bit. Deterministic across platforms and compilers, a handful of
// lines, and adequate for a cache key: a collision costs a wrong decode, not a
// compromised system, and the artifact validates its own key on read. The engine
// already uses FNV-1a for std::hash<Uuid>. See docs/adr/0029-import-cache.md.
class Fnv1a64 {
public:
    void update(std::span<const u8> bytes) noexcept {
        for (const u8 byte : bytes) {
            hash_ ^= static_cast<u64>(byte);
            hash_ *= 1099511628211ULL;
        }
    }

    void update(std::string_view text) noexcept {
        update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    }

    // Length-prefixes a field before hashing it, so concatenating two fields is
    // unambiguous: no pair of inputs can produce the same byte stream as a
    // different pair. This defends against structural collisions (a settings
    // string that happens to contain a source prefix) rather than only against
    // hash collisions.
    void update_field(std::span<const u8> bytes) noexcept {
        const std::array<u8, 8> length = {
            static_cast<u8>(bytes.size()),       static_cast<u8>(bytes.size() >> 8),
            static_cast<u8>(bytes.size() >> 16), static_cast<u8>(bytes.size() >> 24),
            static_cast<u8>(bytes.size() >> 32), static_cast<u8>(bytes.size() >> 40),
            static_cast<u8>(bytes.size() >> 48), static_cast<u8>(bytes.size() >> 56),
        };
        update(length);
        update(bytes);
    }

    void update_field(std::string_view text) noexcept {
        update_field(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
    }

    [[nodiscard]] u64 value() const noexcept { return hash_; }

private:
    u64 hash_ = 1469598103934665603ULL;
};

// The stored artifact is prefixed with a small header so a reader can reject a
// truncated, corrupted, or renamed file instead of returning the wrong bytes.
// The key is stored in the header, so a hash collision (or a file copied to the
// wrong name) is detected and treated as a miss.
constexpr std::string_view kArtifactMarker = "fork-import";

[[nodiscard]] std::vector<u8> encode_artifact(std::string_view key, std::span<const u8> payload) {
    std::vector<u8> out;
    out.reserve(kArtifactMarker.size() + 1 + key.size() + 8 + payload.size());
    out.insert(out.end(), kArtifactMarker.begin(), kArtifactMarker.end());
    out.push_back(0); // marker terminator
    out.insert(out.end(), key.begin(), key.end());
    out.push_back(0); // key terminator
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<u8>(payload.size() >> shift));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

[[nodiscard]] bool decode_artifact(std::string_view key, std::span<const u8> artifact, std::vector<u8>& out) {
    std::size_t offset = 0;
    const auto read_cstring = [&](std::string_view& field) -> bool {
        const std::size_t end = [&] {
            for (std::size_t i = offset; i < artifact.size(); ++i) {
                if (artifact[i] == 0) {
                    return i;
                }
            }
            return artifact.size();
        }();
        if (end >= artifact.size()) {
            return false; // no terminator: truncated
        }
        field = std::string_view(reinterpret_cast<const char*>(artifact.data()) + offset, end - offset);
        offset = end + 1;
        return true;
    };

    std::string_view marker;
    std::string_view stored_key;
    if (!read_cstring(marker) || !read_cstring(stored_key)) {
        return false;
    }
    if (marker != kArtifactMarker || stored_key != key) {
        return false;
    }
    if (artifact.size() - offset < 8) {
        return false;
    }
    usize payload_size = 0;
    for (int i = 0; i < 8; ++i) {
        payload_size |= static_cast<usize>(artifact[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    offset += 8;
    if (artifact.size() - offset != payload_size) {
        return false; // truncated or trailing bytes
    }
    out.assign(artifact.begin() + static_cast<std::ptrdiff_t>(offset), artifact.end());
    return true;
}

[[nodiscard]] std::string to_hex(u64 value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

} // namespace

void ImportCache::set_root(std::string root) {
    root_ = fs::normalize(root);
}

std::string ImportCache::key_for(std::span<const u8> source_bytes, const ImportSettings* settings,
                                 std::string_view importer) {
    Fnv1a64 hash;
    hash.update_field(kKeyVersion);
    hash.update_field(importer);
    // The settings already serialize themselves (ADR-0027), so the key reuses
    // that encoding rather than inventing a second one. A null settings pointer
    // means "the importer's defaults", which hash as an empty field and are
    // therefore distinct from any explicit settings value.
    hash.update_field(settings != nullptr ? import_settings_to_json(*settings) : std::string{});
    hash.update_field(source_bytes);
    return to_hex(hash.value());
}

std::string ImportCache::content_hash(std::span<const u8> bytes) {
    Fnv1a64 hash;
    hash.update_field(bytes);
    return to_hex(hash.value());
}

std::string ImportCache::artifact_path(std::string_view key) const {
    return fs::join(fs::join(root_, "import"), std::string(key) + ".bin");
}

bool ImportCache::contains(std::string_view key) const {
    if (!is_enabled() || key.empty()) {
        return false;
    }
    auto artifact = fs::read_binary_file(artifact_path(key));
    if (!artifact) {
        return false;
    }
    std::vector<u8> payload;
    return decode_artifact(key, artifact.value(), payload);
}

Result<std::vector<u8>> ImportCache::fetch(std::string_view key) const {
    if (!is_enabled() || key.empty()) {
        return Error("import cache has no root");
    }
    auto artifact = fs::read_binary_file(artifact_path(key));
    if (!artifact) {
        return Error("import cache miss for key '" + std::string(key) + "'");
    }
    std::vector<u8> payload;
    if (!decode_artifact(key, artifact.value(), payload)) {
        // A malformed or mismatched entry is a miss, not a wrong result. The
        // caller falls back to a real import and overwrites the entry.
        return Error("import cache entry '" + std::string(key) + "' is corrupt");
    }
    return payload;
}

Error ImportCache::store(std::string_view key, std::span<const u8> artifact) {
    if (!is_enabled() || key.empty()) {
        return Error::none();
    }
    const std::vector<u8> encoded = encode_artifact(key, artifact);
    return fs::write_binary_file(artifact_path(key), encoded.data(), encoded.size());
}

} // namespace fe
