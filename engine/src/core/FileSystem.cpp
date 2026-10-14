// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/FileSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace fe::fs {
namespace {

namespace stdfs = std::filesystem;

[[nodiscard]] stdfs::path to_native(std::string_view path) {
    return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

[[nodiscard]] std::string to_utf8(const stdfs::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace

Result<std::vector<u8>> read_binary_file(std::string_view path) {
    const stdfs::path native = to_native(path);

    std::error_code ec;
    if (!stdfs::exists(native, ec)) {
        return Error("file not found: " + std::string(path));
    }
    if (stdfs::is_directory(native, ec)) {
        return Error("expected a file but found a directory: " + std::string(path));
    }

    std::ifstream stream(native, std::ios::binary | std::ios::ate);
    if (!stream) {
        return Error("could not open file for reading: " + std::string(path));
    }

    const std::streamoff size = stream.tellg();
    if (size < 0) {
        return Error("could not determine file size: " + std::string(path));
    }

    std::vector<u8> buffer(static_cast<usize>(size));
    stream.seekg(0, std::ios::beg);
    if (!buffer.empty()) {
        stream.read(reinterpret_cast<char*>(buffer.data()), size);
        if (!stream) {
            return Error("read failed: " + std::string(path));
        }
    }
    return buffer;
}

Result<std::string> read_text_file(std::string_view path) {
    auto result = read_binary_file(path);
    if (!result) {
        return result.error();
    }
    const std::vector<u8>& bytes = result.value();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Error write_binary_file(std::string_view path, const void* data, usize size) {
    const stdfs::path native = to_native(path);

    if (native.has_parent_path()) {
        std::error_code ec;
        stdfs::create_directories(native.parent_path(), ec);
        if (ec) {
            return Error("could not create directory for " + std::string(path) + ": " + ec.message());
        }
    }

    std::ofstream stream(native, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return Error("could not open file for writing: " + std::string(path));
    }
    if (size > 0) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    if (!stream) {
        return Error("write failed: " + std::string(path));
    }
    return Error::none();
}

Error write_text_file(std::string_view path, std::string_view text) {
    return write_binary_file(path, text.data(), text.size());
}

bool exists(std::string_view path) {
    std::error_code ec;
    return stdfs::exists(to_native(path), ec);
}

u64 last_write_time_ns(std::string_view path) {
    std::error_code ec;
    const stdfs::file_time_type time = stdfs::last_write_time(to_native(path), ec);
    if (ec) {
        return 0;
    }
    // file_time_type is a clock-specific duration. Convert to nanoseconds and
    // return the raw count; callers only ever compare two of these for
    // inequality, so the epoch the count is relative to does not matter.
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

Error create_directories(std::string_view path) {
    std::error_code ec;
    stdfs::create_directories(to_native(path), ec);
    if (ec) {
        return Error("could not create directories for " + std::string(path) + ": " + ec.message());
    }
    return Error::none();
}

std::string parent_path(std::string_view path) {
    return to_utf8(to_native(path).parent_path());
}

std::string filename(std::string_view path) {
    return to_utf8(to_native(path).filename());
}

std::string extension(std::string_view path) {
    std::string ext = to_utf8(to_native(path).extension());
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext;
}

std::string join(std::string_view a, std::string_view b) {
    if (a.empty()) {
        return std::string(b);
    }
    if (b.empty()) {
        return std::string(a);
    }
    std::string result(a);
    if (result.back() != '/' && result.back() != '\\') {
        result.push_back('/');
    }
    std::size_t start = 0;
    while (start < b.size() && (b[start] == '/' || b[start] == '\\')) {
        ++start;
    }
    result.append(b.substr(start));
    return result;
}

std::string normalize(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        out.push_back(c == '\\' ? '/' : c);
    }

    std::string result;
    result.reserve(out.size());
    // Preserve the absolute-path marker so that "/a/b" does not become "a/b".
    if (!out.empty() && out[0] == '/') {
        result.push_back('/');
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        const bool at_segment_start = (result.empty() || result.back() == '/');

        // Drop a "./" segment, or a bare trailing ".", e.g. "a/./b" -> "a/b".
        if (c == '.' && at_segment_start) {
            const bool followed_by_sep = (i + 1 < out.size() && out[i + 1] == '/');
            const bool at_end = (i + 1 == out.size());
            if (followed_by_sep) {
                ++i;
                continue;
            }
            if (at_end) {
                continue;
            }
        }

        // Collapse runs of separators.
        if (c == '/' && !result.empty() && result.back() == '/') {
            continue;
        }
        if (c == '/' && result.empty()) {
            continue;
        }

        result.push_back(c);
    }
    return result;
}

std::string relative_to(std::string_view base, std::string_view path) {
    std::string normalized_base = normalize(base);
    const std::string normalized_path = normalize(path);
    if (normalized_base.empty()) {
        return normalized_path;
    }
    // Drop a trailing separator so the prefix test compares directory names:
    // "/proj/" contains "/proj/assets" the same way "/proj" does. A bare root
    // ("/", "C:/") is left intact.
    if (normalized_base.size() > 1 && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }
    // The separator after the base is part of the prefix, so "/proj" contains
    // "/proj/assets" but not "/project/assets".
    const std::string prefix = normalized_base == "/" ? std::string("/") : normalized_base + "/";
    if (normalized_path.compare(0, prefix.size(), prefix) == 0) {
        return normalized_path.substr(prefix.size());
    }
    return normalized_path;
}

bool is_absolute(std::string_view path) {
    return to_native(path).is_absolute();
}

std::vector<std::string> list_files_recursive(std::string_view directory) {
    std::vector<std::string> files;

    const stdfs::path root = to_native(directory);
    std::error_code ec;
    if (!stdfs::is_directory(root, ec)) {
        return files;
    }

    for (stdfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        // Relative to the scan root, so callers get stable asset-tree paths.
        const stdfs::path relative = stdfs::relative(it->path(), root, ec);
        if (ec) {
            continue;
        }
        files.push_back(to_utf8(relative));
    }

    // Deterministic order, so a rebuild from disk produces the same index.
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace fe::fs
