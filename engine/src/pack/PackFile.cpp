// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/PackFile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace fe::pack {
namespace {

// The fixed header size: magic, format_version, entry_count, index_offset,
// payload_offset.
constexpr usize kHeaderSize = 4 + 4 + 4 + 8 + 8;
// One index entry's fixed part: key length, scheme length, guid, offset, length.
constexpr usize kEntryFixedSize = 4 + 4 + 16 + 8 + 8;

// Little-endian writers. The format is defined as little-endian on every host,
// so a pack written on one platform reads on another.
void write_u32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFFU));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFU));
    out.push_back(static_cast<u8>((value >> 16) & 0xFFU));
    out.push_back(static_cast<u8>((value >> 24) & 0xFFU));
}

void write_u64(std::vector<u8>& out, u64 value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<u8>((value >> (8 * i)) & 0xFFU));
    }
}

// A cursor over a byte buffer with bounds-checked little-endian reads. Every
// read advances it; a read past the end reports failure so the caller can
// return a clear Error instead of reading garbage.
class Reader {
public:
    Reader(const u8* data, usize size) : data_(data), size_(size) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] usize offset() const noexcept { return offset_; }
    [[nodiscard]] usize remaining() const noexcept { return offset_ <= size_ ? size_ - offset_ : 0; }

    bool read_bytes(void* out, usize count) {
        if (!ok_ || count > remaining()) {
            ok_ = false;
            return false;
        }
        if (count > 0) {
            std::memcpy(out, data_ + offset_, count);
        }
        offset_ += count;
        return true;
    }

    bool read_u32(u32& out) {
        u8 bytes[4];
        if (!read_bytes(bytes, sizeof(bytes))) {
            return false;
        }
        out = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) | (static_cast<u32>(bytes[2]) << 16) |
              (static_cast<u32>(bytes[3]) << 24);
        return true;
    }

    bool read_u64(u64& out) {
        u8 bytes[8];
        if (!read_bytes(bytes, sizeof(bytes))) {
            return false;
        }
        u64 value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<u64>(bytes[i]) << (8 * i);
        }
        out = value;
        return true;
    }

    // Jumps to an absolute offset. Fails if it is outside the buffer.
    bool seek(usize target) {
        if (!ok_ || target > size_) {
            ok_ = false;
            return false;
        }
        offset_ = target;
        return true;
    }

private:
    const u8* data_;
    usize size_;
    usize offset_ = 0;
    bool ok_ = true;
};

} // namespace

void PackFile::add_file(std::string key, std::string scheme, Uuid guid, std::vector<u8> data) {
    PackEntry entry;
    entry.key = std::move(key);
    entry.scheme = std::move(scheme);
    entry.guid = guid;
    entry.data = std::move(data);
    entries_.push_back(std::move(entry));
}

const PackEntry* PackFile::find_path(std::string_view key) const {
    for (const PackEntry& entry : entries_) {
        if (entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

const PackEntry* PackFile::find_guid(const Uuid& id) const {
    if (id.is_nil()) {
        return nullptr;
    }
    for (const PackEntry& entry : entries_) {
        if (!entry.guid.is_nil() && entry.guid == id) {
            return &entry;
        }
    }
    return nullptr;
}

Result<std::vector<u8>> PackFile::write() const {
    // Write in a stable order (by key) so a rebuild from the same inputs is
    // byte-identical, which lets a build system treat an unchanged pack as up
    // to date. Reject a duplicate key rather than letting one shadow the other.
    std::vector<const PackEntry*> ordered;
    ordered.reserve(entries_.size());
    for (const PackEntry& entry : entries_) {
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const PackEntry* a, const PackEntry* b) { return a->key < b->key; });
    for (usize i = 1; i < ordered.size(); ++i) {
        if (ordered[i - 1]->key == ordered[i]->key) {
            return Error("pack: duplicate key '" + ordered[i]->key + "'");
        }
    }

    // The index length depends only on the keys and schemes, so it can be sized
    // before the payload offset is known.
    usize index_size = 0;
    for (const PackEntry* entry : ordered) {
        if (entry->key.size() > 0xFFFFFFFFULL || entry->scheme.size() > 0xFFFFFFFFULL) {
            return Error("pack: key or scheme is too long for the format");
        }
        index_size += kEntryFixedSize + entry->key.size() + entry->scheme.size();
    }

    const u64 index_offset = kHeaderSize;
    const u64 payload_offset = index_offset + index_size;

    std::vector<u8> out;
    out.reserve(static_cast<usize>(payload_offset) + 64);

    write_u32(out, kMagic);
    write_u32(out, static_cast<u32>(kFormatVersion));
    write_u32(out, static_cast<u32>(ordered.size()));
    write_u64(out, index_offset);
    write_u64(out, payload_offset);

    // The payload starts after the index; each entry's data is appended in
    // index order, so offsets are assigned as the index is written.
    std::vector<u64> data_offsets;
    data_offsets.reserve(ordered.size());
    u64 cursor = payload_offset;
    for (const PackEntry* entry : ordered) {
        data_offsets.push_back(cursor);
        cursor += entry->data.size();
    }

    for (usize i = 0; i < ordered.size(); ++i) {
        const PackEntry* entry = ordered[i];
        write_u32(out, static_cast<u32>(entry->key.size()));
        out.insert(out.end(), entry->key.begin(), entry->key.end());
        write_u32(out, static_cast<u32>(entry->scheme.size()));
        out.insert(out.end(), entry->scheme.begin(), entry->scheme.end());
        out.insert(out.end(), entry->guid.bytes().begin(), entry->guid.bytes().end());
        write_u64(out, data_offsets[i]);
        write_u64(out, static_cast<u64>(entry->data.size()));
    }

    for (const PackEntry* entry : ordered) {
        out.insert(out.end(), entry->data.begin(), entry->data.end());
    }

    return out;
}

Result<PackFile> PackFile::parse(const u8* data, usize size) {
    if (data == nullptr || size < kHeaderSize) {
        return Error("pack: buffer is smaller than the header");
    }

    Reader reader(data, size);
    u32 magic = 0;
    u32 version = 0;
    u32 entry_count = 0;
    u64 index_offset = 0;
    u64 payload_offset = 0;
    if (!reader.read_u32(magic) || !reader.read_u32(version) || !reader.read_u32(entry_count) ||
        !reader.read_u64(index_offset) || !reader.read_u64(payload_offset)) {
        return Error("pack: could not read the header");
    }

    if (magic != kMagic) {
        return Error("pack: bad magic (not a fork pack)");
    }
    if (version > static_cast<u32>(kFormatVersion)) {
        return Error("pack: format version " + std::to_string(version) + " is newer than supported version " +
                     std::to_string(kFormatVersion));
    }
    if (index_offset < kHeaderSize || index_offset > size || payload_offset > size || payload_offset < index_offset) {
        return Error("pack: header offsets are out of range");
    }

    PackFile pack;
    pack.entries_.reserve(entry_count);
    if (!reader.seek(static_cast<usize>(index_offset))) {
        return Error("pack: index offset is out of range");
    }

    for (u32 i = 0; i < entry_count; ++i) {
        u32 key_length = 0;
        if (!reader.read_u32(key_length)) {
            return Error("pack: truncated index (key length)");
        }
        std::string key(key_length, '\0');
        if (!reader.read_bytes(key.data(), key_length)) {
            return Error("pack: truncated index (key)");
        }

        u32 scheme_length = 0;
        if (!reader.read_u32(scheme_length)) {
            return Error("pack: truncated index (scheme length)");
        }
        std::string scheme(scheme_length, '\0');
        if (!reader.read_bytes(scheme.data(), scheme_length)) {
            return Error("pack: truncated index (scheme)");
        }

        PackEntry entry;
        entry.key = std::move(key);
        entry.scheme = std::move(scheme);
        std::array<u8, 16> guid_bytes{};
        if (!reader.read_bytes(guid_bytes.data(), guid_bytes.size())) {
            return Error("pack: truncated index (guid)");
        }
        // A nil guid is all-zero bytes, which is Uuid's default value, so this
        // round-trips an unset identity unchanged.
        entry.guid = Uuid::from_bytes(guid_bytes);

        u64 offset = 0;
        u64 length = 0;
        if (!reader.read_u64(offset) || !reader.read_u64(length)) {
            return Error("pack: truncated index (payload location)");
        }

        // The payload span must lie within the buffer and within the payload
        // region, so a corrupt index cannot make the reader hand out bytes from
        // the header or the index itself.
        if (offset < payload_offset || length > size || offset > size || length > size - static_cast<usize>(offset)) {
            return Error("pack: entry '" + entry.key + "' has an out-of-range payload");
        }
        entry.data.assign(data + offset, data + offset + length);
        pack.entries_.push_back(std::move(entry));
    }

    return pack;
}

} // namespace fe::pack
