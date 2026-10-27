// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "WebUserData.h"

#include "fe/core/Log.h"

#include <emscripten.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace fe {
namespace {

// The base64 alphabet, in the standard order, as an EM_ASM-free table. The
// encoding is done in C++ rather than in JavaScript so the only JavaScript this
// backend contains is the two localStorage calls, which keeps the clang-format
// hazard (an EM_ASM body parsed as C++) to a minimum.
constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::string base64_encode(const u8* data, usize size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (usize i = 0; i < size; i += 3) {
        const u32 b0 = data[i];
        const u32 b1 = i + 1 < size ? data[i + 1] : 0;
        const u32 b2 = i + 2 < size ? data[i + 2] : 0;
        const u32 triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < size ? kBase64Alphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < size ? kBase64Alphabet[triple & 0x3f] : '=');
    }
    return out;
}

// The value of a base64 character, or -1 when it is not one (padding included).
[[nodiscard]] int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

// Decodes base64, ignoring padding. Returns an Error on a malformed input, which
// can only come from a foreign writer to the same localStorage key.
[[nodiscard]] Result<std::vector<u8>> base64_decode(std::string_view text) {
    std::vector<u8> out;
    out.reserve((text.size() / 4) * 3);
    u32 accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const int value = base64_value(c);
        if (value < 0) {
            return Error("user data: malformed stored value");
        }
        accumulator = (accumulator << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((accumulator >> bits) & 0xff));
        }
    }
    return out;
}

// --- localStorage, through the Emscripten API --------------------------------
//
// The four helpers below are the only place in the engine that names a browser
// storage API, and they are inside the platform backend, which is where a
// browser-specific rule belongs (ADR-0041).
//
// The EM_ASM bodies deliberately contain no strict-equality operator: Emscripten
// splices the body in verbatim and clang-format parses it as C++, where `a === b`
// becomes `a == = b` and breaks the module at run time with no build error. This
// is the trap AGENTS.md records, so `startsWith` and `==` are used instead.
[[nodiscard]] bool web_storage_available() {
    return MAIN_THREAD_EM_ASM_INT({ return typeof localStorage === 'undefined' ? 0 : 1; }) != 0;
}

// Returns 1 when the key is present, 0 when it is not. The presence test is
// `getItem` rather than `in`, because localStorage is a key/value store whose
// keys may include one named like a property.
[[nodiscard]] bool web_storage_has(const std::string& key) {
    return MAIN_THREAD_EM_ASM_INT(
               {
                   var key = UTF8ToString($0);
                   try {
                       return localStorage.getItem(key) == null ? 0 : 1;
                   } catch (error) {
                       return 0;
                   }
               },
               key.c_str()) != 0;
}

// Writes and reads back `value`. Returns 0 on success and a negative code when
// the write failed (quota exceeded, storage disabled); a successful return means
// the value is readable, which is what makes the write durable.
[[nodiscard]] int web_storage_put(const std::string& key, const std::string& value) {
    return MAIN_THREAD_EM_ASM_INT(
        {
            var key = UTF8ToString($0);
            var value = UTF8ToString($1);
            try {
                localStorage.setItem(key, value);
                // Read back so a write the browser silently dropped (a full
                // quota, a storage policy) is reported as a failure here rather
                // than discovered by the next run.
                return localStorage.getItem(key) == null ? -2 : 0;
            } catch (error) {
                return -1;
            }
        },
        key.c_str(), value.c_str());
}

[[nodiscard]] bool web_storage_erase(const std::string& key) {
    return MAIN_THREAD_EM_ASM_INT(
               {
                   var key = UTF8ToString($0);
                   try {
                       localStorage.removeItem(key);
                       return 1;
                   } catch (error) {
                       return 0;
                   }
               },
               key.c_str()) != 0;
}

// The value stored under `key`, or an Error when the key is absent or storage is
// unavailable. The returned string is copied out of the module heap by the
// caller through `Module.__feUserDataValue`, the same one-slot hand-off
// HttpFetchSource uses for a fetched body.
[[nodiscard]] Result<std::string> web_storage_get(const std::string& key) {
    const int length = MAIN_THREAD_EM_ASM_INT(
        {
            var key = UTF8ToString($0);
            try {
                var value = localStorage.getItem(key);
                if (value == null) {
                    return -1;
                }
                Module.__feUserDataValue = value;
                return value.length;
            } catch (error) {
                return -2;
            }
        },
        key.c_str());
    if (length < 0) {
        return Error("user data: no stored value for that key");
    }
    std::string value(static_cast<usize>(length), '\0');
    if (length > 0) {
        MAIN_THREAD_EM_ASM(
            {
                var value = Module.__feUserDataValue;
                var pointer = $0;
                for (var i = 0; i < value.length; i++) {
                    HEAPU8[pointer + i] = value.charCodeAt(i) & 0xff;
                }
            },
            value.data());
    }
    return value;
}

// The store keys present under `prefix`, collected by walking localStorage.
// `Object.keys(localStorage)` is not used: it is not defined by the storage spec
// to include only own keys, and the `length`/`key(i)` pair is.
[[nodiscard]] std::vector<std::string> web_storage_keys(const std::string& prefix) {
    const int count = MAIN_THREAD_EM_ASM_INT(
        {
            var prefix = UTF8ToString($0);
            try {
                var found = [];
                for (var i = 0; i < localStorage.length; i++) {
                    var name = localStorage.key(i);
                    if (name != null && name.startsWith(prefix)) {
                        found.push(name.substring(prefix.length));
                    }
                }
                Module.__feUserDataKeys = found;
                return found.length;
            } catch (error) {
                return -1;
            }
        },
        prefix.c_str());
    if (count <= 0) {
        return {};
    }
    std::vector<std::string> keys;
    keys.reserve(static_cast<usize>(count));
    for (int i = 0; i < count; i++) {
        const int length = MAIN_THREAD_EM_ASM_INT({ return Module.__feUserDataKeys[$0].length; }, i);
        std::string key(static_cast<usize>(length), '\0');
        if (length > 0) {
            MAIN_THREAD_EM_ASM(
                {
                    var name = Module.__feUserDataKeys[$1];
                    var pointer = $0;
                    for (var j = 0; j < name.length; j++) {
                        HEAPU8[pointer + j] = name.charCodeAt(j) & 0xff;
                    }
                },
                key.data(), i);
        }
        keys.push_back(std::move(key));
    }
    return keys;
}

} // namespace

std::string WebUserData::storage_key(std::string_view key) const {
    return prefix_ + "/" + std::string(key);
}

Result<std::unique_ptr<WebUserData>> WebUserData::open(std::string_view app_id) {
    if (!web_storage_available()) {
        return Error("user data: localStorage is not available in this browser context");
    }
    // A leading namespace keeps two games on one origin apart. An empty app id
    // still gets a prefix, so the keys are never loose in the origin's storage.
    const std::string prefix = std::string(app_id.empty() ? "fork_engine" : app_id);
    return std::unique_ptr<WebUserData>(new WebUserData(prefix));
}

Result<std::vector<u8>> WebUserData::read(std::string_view key) const {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    auto stored = web_storage_get(storage_key(key));
    if (!stored) {
        return stored.error();
    }
    return base64_decode(stored.value());
}

Error WebUserData::write(std::string_view key, const void* data, usize size) {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    if (data == nullptr && size != 0) {
        return Error("user data: write of '" + std::string(key) + "' with a null pointer");
    }
    const std::string encoded = base64_encode(static_cast<const u8*>(data), size);
    const int result = web_storage_put(storage_key(key), encoded);
    if (result == -1) {
        return Error("user data: could not write '" + std::string(key) + "' (storage unavailable or full)");
    }
    if (result == -2) {
        return Error("user data: write of '" + std::string(key) + "' did not persist");
    }
    return Error::none();
}

bool WebUserData::contains(std::string_view key) const {
    if (!is_valid_key(key)) {
        return false;
    }
    return web_storage_has(storage_key(key));
}

Error WebUserData::remove(std::string_view key) {
    if (Error e = check_key(key); !e.ok()) {
        return e;
    }
    if (!web_storage_erase(storage_key(key))) {
        return Error("user data: could not remove '" + std::string(key) + "'");
    }
    return Error::none();
}

std::vector<std::string> WebUserData::keys() const {
    std::vector<std::string> keys = web_storage_keys(prefix_ + "/");
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [](const std::string& key) { return !is_valid_key(key); }),
               keys.end());
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace fe
