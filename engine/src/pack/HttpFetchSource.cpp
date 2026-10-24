// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/pack/HttpFetchSource.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fe::pack {
namespace {

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

} // namespace

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
