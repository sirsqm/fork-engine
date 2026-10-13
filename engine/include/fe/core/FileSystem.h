// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Error.h"
#include "fe/core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace fe {

// Filesystem access.
//
// All platform-specific path handling lives behind this interface so that no
// other module calls a native filesystem API directly. Paths are treated as
// UTF-8 strings; they are normalized to forward slashes internally, which the
// Windows APIs accept as well.
namespace fs {

// Reads an entire file into memory. Returns the contents, or an Error when the
// file cannot be opened or read. An empty file is a successful read of size 0.
[[nodiscard]] Result<std::vector<u8>> read_binary_file(std::string_view path);

// Reads an entire file into a string (text mode is not used; the bytes are
// returned verbatim).
[[nodiscard]] Result<std::string> read_text_file(std::string_view path);

// Writes the buffer, creating parent directories as needed. Overwrites any
// existing file. Returns an Error on failure.
[[nodiscard]] Error write_binary_file(std::string_view path, const void* data, usize size);
[[nodiscard]] Error write_text_file(std::string_view path, std::string_view text);

[[nodiscard]] bool exists(std::string_view path);

// Creates a directory and all missing parents. Returns an Error on failure.
[[nodiscard]] Error create_directories(std::string_view path);

// Returns the parent directory of a path, or "" if there is none.
[[nodiscard]] std::string parent_path(std::string_view path);

// Returns the final component of a path (file name or directory name).
[[nodiscard]] std::string filename(std::string_view path);

// Returns the extension including the leading dot, lower-cased (".png"), or ""
// when there is no extension.
[[nodiscard]] std::string extension(std::string_view path);

// Joins two path fragments with a single separator.
[[nodiscard]] std::string join(std::string_view a, std::string_view b);

// Normalizes separators and collapses redundant "./" segments. Does not touch
// the filesystem and does not resolve "..".
[[nodiscard]] std::string normalize(std::string_view path);

// Returns `path` expressed relative to `base` when `path` is inside `base`
// (lexically, after normalization), or `path` unchanged when it is not. Both
// are treated as absolute-or-not exactly as given; no filesystem access, and
// ".." is not resolved. This is what a project file uses to store a path that
// travels with the project instead of one that names the machine it was
// authored on.
[[nodiscard]] std::string relative_to(std::string_view base, std::string_view path);

// True when `path` is absolute for the host platform. On Windows this means a
// drive letter or a UNC prefix; elsewhere, a leading '/'. This is the one place
// platform path syntax is decided, so callers that need to know whether a path
// can be joined onto a base ask here rather than testing the first character.
[[nodiscard]] bool is_absolute(std::string_view path);

// Recursively lists the files under `directory`, returning paths relative to
// it, normalized with forward slashes and sorted lexicographically so the
// result is deterministic. Directories are not returned. A directory that does
// not exist (or is not a directory) yields an empty list, not an error: callers
// that scan for optional asset trees treat absence as "nothing to scan".
[[nodiscard]] std::vector<std::string> list_files_recursive(std::string_view directory);

} // namespace fs
} // namespace fe
