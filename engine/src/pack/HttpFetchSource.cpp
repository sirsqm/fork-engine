// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/HttpFetchSource.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The two implementations of http_get below share nothing but their contract:
// a desktop build opens a socket, a web build asks the browser. The socket
// headers are therefore desktop-only, and the web build includes Emscripten's
// header for the synchronous-XHR macros instead. See the note on http_get in the
// header for why the web path is synchronous.
#if defined(FE_PLATFORM_WEB)
#include <emscripten.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fe::pack {
namespace {

#if !defined(FE_PLATFORM_WEB)
#if !defined(_WIN32)
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
void close_socket(socket_t fd) noexcept {
    ::close(fd);
}
#else
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
void close_socket(socket_t fd) noexcept {
    ::closesocket(fd);
}
#endif

// A one-shot WSA lifetime for Windows. Winsock needs WSAStartup before any
// socket call; doing it per request keeps the header free of global state and
// costs nothing measurable next to the network round trip.
class SocketRuntime {
public:
    SocketRuntime() {
#if defined(_WIN32)
        WSADATA data;
        started_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        started_ = true;
#endif
    }
    ~SocketRuntime() {
#if defined(_WIN32)
        if (started_) {
            ::WSACleanup();
        }
#endif
    }
    [[nodiscard]] bool ok() const noexcept { return started_; }

private:
    bool started_ = false;
};

struct Url {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

// Splits `http://host[:port]/path`. Only the http scheme is accepted: a shipped
// web build is served over http(s) by the page, and https needs TLS, which this
// tiny client does not carry.
[[nodiscard]] Result<Url> parse_url(std::string_view url) {
    constexpr std::string_view scheme = "http://";
    if (!url.starts_with(scheme)) {
        return Error("http: only 'http://' URLs are supported, got '" + std::string(url) + "'");
    }
    std::string_view rest = url.substr(scheme.size());
    Url out;
    const std::size_t slash = rest.find('/');
    std::string_view authority = rest;
    if (slash == std::string_view::npos) {
        out.path = "/";
    } else {
        authority = rest.substr(0, slash);
        out.path = std::string(rest.substr(slash));
    }
    const std::size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        out.host = std::string(authority);
    } else {
        out.host = std::string(authority.substr(0, colon));
        out.port = std::string(authority.substr(colon + 1));
    }
    if (out.host.empty()) {
        return Error("http: URL has no host: '" + std::string(url) + "'");
    }
    return out;
}

[[nodiscard]] Result<std::string> receive_all(socket_t fd) {
    std::string response;
    char buffer[4096];
    while (true) {
        const auto received = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    return response;
}

#else // FE_PLATFORM_WEB

// The browser's side of the same contract. A synchronous XMLHttpRequest is the
// only way to fetch without giving up the synchronous `ResourceSource` shape, so
// the pack is fetched once at mount time and every later read is answered from
// memory (which is what HttpFetchSource does anyway).
//
// The body is read through `responseText` with the `x-user-defined` charset,
// which maps every byte to the code point of the same value; the response is
// then copied into a Uint8Array by masking to 8 bits. Reading `responseText`
// with the default charset would run the bytes through UTF-8 decoding and
// corrupt a binary pack, and `responseType = 'arraybuffer'` is not available on
// a synchronous request in every browser, so this is the portable byte-exact
// path.
//
// The EM_ASM body deliberately contains no strict-equality operator: Emscripten
// splices it in verbatim and clang-format parses it as C++, where `a === b`
// becomes `a == = b` and breaks the module at run time with no build error.
[[nodiscard]] Result<std::string> web_http_get(std::string_view url) {
    const std::string target(url);
    const int length = MAIN_THREAD_EM_ASM_INT(
        {
            var url = UTF8ToString($0);
            var xhr = new XMLHttpRequest();
            try {
                xhr.open('GET', url, false);
                xhr.overrideMimeType('text/plain; charset=x-user-defined');
                xhr.send(null);
            } catch (error) {
                Module.__feHttpStatus = 0;
                return -1;
            }
            Module.__feHttpStatus = xhr.status;
            if (xhr.status < 200 || xhr.status >= 300) {
                return -2;
            }
            var text = xhr.responseText;
            var bytes = new Uint8Array(text.length);
            for (var i = 0; i < text.length; i++) {
                bytes[i] = text.charCodeAt(i) & 0xff;
            }
            Module.__feHttpBytes = bytes;
            return bytes.length;
        },
        target.c_str());

    const int status = MAIN_THREAD_EM_ASM_INT({ return Module.__feHttpStatus | 0; });
    if (length == -1) {
        // The request never completed: a blocked origin, a bad URL, or a network
        // error. The browser's own message is not carried across (it would need a
        // heap allocation whose ownership the caller cannot free through this
        // API), so the URL is named and the page's console has the rest.
        return Error("http: cannot fetch '" + target + "' (request failed or was blocked)");
    }
    if (length < 0) {
        return Error("http: '" + target + "' returned status " + std::to_string(status));
    }

    std::string body(static_cast<std::size_t>(length), '\0');
    if (length > 0) {
        MAIN_THREAD_EM_ASM(
            {
                var bytes = Module.__feHttpBytes;
                var pointer = $0;
                HEAPU8.set(bytes, pointer);
            },
            body.data());
    }
    return body;
}

#endif // FE_PLATFORM_WEB

} // namespace

#if defined(FE_PLATFORM_WEB)

Result<std::string> http_get(std::string_view url) {
    // A browser resolves a relative URL against the page, so the export's
    // manifest-relative pack path works without the shell computing an origin.
    return web_http_get(url);
}

#else

Result<std::string> http_get(std::string_view url) {
    auto parsed = parse_url(url);
    if (!parsed) {
        return parsed.error();
    }
    const Url& target = parsed.value();

    SocketRuntime runtime;
    if (!runtime.ok()) {
        return Error("http: could not initialise the socket layer");
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(target.host.c_str(), target.port.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
        return Error("http: cannot resolve host '" + target.host + "'");
    }

    socket_t fd = kInvalidSocket;
    for (addrinfo* it = addresses; it != nullptr; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }
        if (::connect(fd, it->ai_addr, static_cast<int>(it->ai_addrlen)) == 0) {
            break;
        }
        close_socket(fd);
        fd = kInvalidSocket;
    }
    ::freeaddrinfo(addresses);
    if (fd == kInvalidSocket) {
        return Error("http: cannot connect to '" + target.host + ":" + target.port + "'");
    }

    const std::string request =
        "GET " + target.path + " HTTP/1.1\r\nHost: " + target.host + "\r\nConnection: close\r\n\r\n";
    int flags = 0;
#ifdef MSG_NOSIGNAL
    // A closed peer must return EPIPE, not raise SIGPIPE and kill the process.
    flags = MSG_NOSIGNAL;
#endif
    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto n = ::send(fd, request.data() + sent, static_cast<int>(request.size() - sent), flags);
        if (n <= 0) {
            close_socket(fd);
            return Error("http: cannot send the request to '" + target.host + "'");
        }
        sent += static_cast<std::size_t>(n);
    }

    auto received = receive_all(fd);
    close_socket(fd);
    if (!received) {
        return received.error();
    }
    const std::string& response = received.value();

    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return Error("http: malformed response from '" + target.host + "'");
    }
    const std::string_view status_line = std::string_view(response).substr(0, response.find("\r\n"));
    int status = 0;
    if (const std::size_t code_pos = status_line.find(' '); code_pos != std::string_view::npos) {
        for (const char c : status_line.substr(code_pos + 1)) {
            if (c < '0' || c > '9') {
                break;
            }
            status = status * 10 + (c - '0');
        }
    }
    if (status < 200 || status >= 300) {
        return Error("http: '" + target.path + "' returned status " + std::to_string(status));
    }
    return response.substr(header_end + 4);
}

#endif // FE_PLATFORM_WEB

Result<std::shared_ptr<HttpFetchSource>> HttpFetchSource::open(std::string base_url, std::string_view pack_path) {
    if (!base_url.empty() && base_url.back() != '/') {
        base_url.push_back('/');
    }
    auto body = http_get(base_url + std::string(pack_path));
    if (!body) {
        return body.error();
    }
    auto parsed = PackFile::parse(reinterpret_cast<const u8*>(body.value().data()), body.value().size());
    if (!parsed) {
        return parsed.error();
    }
    auto pack = std::make_shared<PackSource>(std::move(parsed.value()));
    return std::make_shared<HttpFetchSource>(std::move(pack), std::move(base_url));
}

HttpFetchSource::HttpFetchSource(std::shared_ptr<PackSource> pack, std::string base_url)
    : pack_(std::move(pack)), base_url_(std::move(base_url)) {
    if (!base_url_.empty() && base_url_.back() != '/') {
        base_url_.push_back('/');
    }
}

Result<std::vector<u8>> HttpFetchSource::read_binary(std::string_view path) const {
    if (auto packed = pack_->read_binary(path)) {
        return packed;
    }
    // Not in the pack: fall back to a plain GET of the loose asset, so a build
    // that serves files beside its pack still loads them.
    auto body = http_get(base_url_ + std::string(path));
    if (!body) {
        return Error("http source: '" + std::string(path) + "' is not in the pack and the server did not serve it (" +
                     body.error().message() + ")");
    }
    return std::vector<u8>(body.value().begin(), body.value().end());
}

Result<std::string> HttpFetchSource::read_text(std::string_view path) const {
    if (auto packed = pack_->read_text(path)) {
        return packed;
    }
    return http_get(base_url_ + std::string(path));
}

bool HttpFetchSource::exists(std::string_view path) const {
    if (pack_->exists(path)) {
        return true;
    }
    // A path outside the pack is served only if the server actually has it, so
    // the answer is the same as asking read_binary().
    return http_get(base_url_ + std::string(path)).ok();
}

const ResourceSource::AssetLocation* HttpFetchSource::resolve_asset(const AssetId& id) const {
    return pack_->resolve_asset(id);
}

} // namespace fe::pack
