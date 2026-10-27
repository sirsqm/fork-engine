// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The HTTP fetch resource source (docs/PLAN.md, `pack-fetch-source`).
//
// A shipped web build has no filesystem, so it fetches its pack over HTTP. This
// serves a real pack from an in-process HTTP server and reads an asset through
// HttpFetchSource, asserting the bytes match what the disk PackSource returns.
// Headless: one loopback socket, no GPU, no threads beyond the server's.

#include <doctest/doctest.h>

#include "fe/core/FileSystem.h"
#include "fe/core/Uuid.h"
#include "fe/pack/HttpFetchSource.h"
#include "fe/pack/PackFile.h"
#include "fe/pack/PackSource.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace fe;
using namespace fe::pack;

namespace {

#if defined(_WIN32)
using test_socket = SOCKET;
constexpr test_socket kNoSocket = INVALID_SOCKET;
void test_close(test_socket fd) noexcept {
    ::closesocket(fd);
}
#else
using test_socket = int;
constexpr test_socket kNoSocket = -1;
void test_close(test_socket fd) noexcept {
    ::close(fd);
}
#endif

[[nodiscard]] std::vector<u8> bytes_of(std::string_view text) {
    return std::vector<u8>(text.begin(), text.end());
}

// A unique temporary directory per test case, removed on destruction.
class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("fe_fetch_test_" + name + "_" + std::to_string(counter_++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// A tiny single-threaded HTTP/1.1 server on 127.0.0.1, serving an in-memory
// table of URL path -> bytes. It exists so the fetch source is tested against a
// real socket and real HTTP framing, not a stubbed transport: the acceptance
// test for this item is that the source reads over HTTP, which only a server
// can prove.
class TestHttpServer {
public:
    TestHttpServer() {
#if defined(_WIN32)
        WSADATA data;
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return;
        }
        wsa_started_ = true;
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ == kNoSocket) {
            return;
        }
        int reuse = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        // Unqualified: on macOS htonl/ntohs are macros from <sys/_endian.h>,
        // so a `::` prefix does not parse. They are plain functions elsewhere.
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0; // let the OS choose a free port
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            test_close(listener_);
            listener_ = kNoSocket;
            return;
        }
        if (::listen(listener_, 8) != 0) {
            test_close(listener_);
            listener_ = kNoSocket;
            return;
        }
        sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
            test_close(listener_);
            listener_ = kNoSocket;
            return;
        }
        port_ = ntohs(bound.sin_port);
        thread_ = std::thread([this] { serve(); });
    }

    ~TestHttpServer() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ != kNoSocket) {
            test_close(listener_);
        }
#if defined(_WIN32)
        if (wsa_started_) {
            ::WSACleanup();
        }
#endif
    }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    void serve_file(std::string path, std::vector<u8> body) { files_.push_back({std::move(path), std::move(body)}); }

    [[nodiscard]] bool ok() const noexcept { return listener_ != kNoSocket; }
    [[nodiscard]] std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

private:
    void serve() {
        while (!stop_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval timeout{};
            timeout.tv_usec = 20000; // 20 ms, so stop_ is noticed promptly
            const int ready = ::select(static_cast<int>(listener_ + 1), &set, nullptr, nullptr, &timeout);
            if (ready <= 0) {
                continue;
            }
            test_socket client = ::accept(listener_, nullptr, nullptr);
            if (client == kNoSocket) {
                continue;
            }
            handle(client);
            test_close(client);
        }
    }

    void handle(test_socket client) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto n = ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (n <= 0) {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(n));
        }
        // "GET /path HTTP/1.1"
        std::string path = "/";
        const std::size_t first_space = request.find(' ');
        if (first_space != std::string::npos) {
            const std::size_t second_space = request.find(' ', first_space + 1);
            path = request.substr(first_space + 1, second_space - first_space - 1);
        }

        const std::vector<u8>* body = nullptr;
        for (const auto& [key, data] : files_) {
            if (key == path) {
                body = &data;
                break;
            }
        }

        std::string header;
        if (body == nullptr) {
            header = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, header);
            return;
        }
        header = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                 std::to_string(body->size()) + "\r\nConnection: close\r\n\r\n";
        send_all(client, header);
        if (!body->empty()) {
            send_all(client, std::string_view(reinterpret_cast<const char*>(body->data()), body->size()));
        }
    }

    void send_all(test_socket client, std::string_view data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const auto n = ::send(client, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (n <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    test_socket listener_ = kNoSocket;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::vector<std::pair<std::string, std::vector<u8>>> files_;
#if defined(_WIN32)
    // Only Windows needs WSAStartup/WSACleanup, so the flag is declared only
    // there; elsewhere it would be an unused private field (-Wunused-private-field).
    bool wsa_started_ = false;
#endif
};

} // namespace

TEST_CASE("an http fetch source reads a packed asset matching the disk source") {
    TestHttpServer server;
    REQUIRE(server.ok());

    // Build a real pack in memory and also write it to disk, so the same bytes
    // can be read through both the HTTP source and the disk PackSource.
    PackFile pack;
    const Uuid hero_id = Uuid::generate();
    const std::vector<u8> hero = bytes_of("HERO PNG BYTES");
    pack.add_file("assets/hero.png", "tex", hero_id, hero);
    pack.add_file("config/settings.txt", "", Uuid{}, bytes_of("volume=7\n"));
    auto pack_bytes = pack.write();
    REQUIRE(pack_bytes);

    server.serve_file("/game.fepack", pack_bytes.value());

    TempDir dir("fetch");
    REQUIRE_FALSE(fs::write_binary_file(dir.file("game.fepack"), pack_bytes.value().data(), pack_bytes.value().size()));

    auto disk = PackSource::open(dir.file("game.fepack"));
    REQUIRE(disk);

    auto fetched = HttpFetchSource::open(server.base_url(), "game.fepack");
    REQUIRE(fetched);

    // The bytes read through HTTP match the bytes read from disk, entry for
    // entry: this is the acceptance test for the item.
    CHECK(fetched.value()->read_binary("assets/hero.png").value() == hero);
    CHECK(fetched.value()->read_binary("assets/hero.png").value() ==
          disk.value()->read_binary("assets/hero.png").value());
    CHECK(fetched.value()->read_text("config/settings.txt").value() == "volume=7\n");
    CHECK(fetched.value()->read_text("config/settings.txt").value() ==
          disk.value()->read_text("config/settings.txt").value());

    // The pack's own identity index is served through the seam unchanged.
    CHECK(fetched.value()->exists("assets/hero.png"));
    CHECK_FALSE(fetched.value()->exists("assets/absent.png"));
    const ResourceSource::AssetLocation* location = fetched.value()->resolve_asset(hero_id);
    REQUIRE(location != nullptr);
    CHECK(location->path == "assets/hero.png");
    CHECK(location->scheme == "tex");
}

TEST_CASE("an http fetch source serves a loose file beside the pack") {
    TestHttpServer server;
    REQUIRE(server.ok());

    PackFile pack;
    pack.add_file("in/pack.txt", "", Uuid{}, bytes_of("packed"));
    auto pack_bytes = pack.write();
    REQUIRE(pack_bytes);
    server.serve_file("/game.fepack", pack_bytes.value());
    server.serve_file("/loose/extra.txt", bytes_of("loose bytes"));

    auto source = HttpFetchSource::open(server.base_url(), "game.fepack");
    REQUIRE(source);

    CHECK(source.value()->read_text("in/pack.txt").value() == "packed");
    // A path the pack does not carry falls back to a plain GET.
    CHECK(source.value()->read_text("loose/extra.txt").value() == "loose bytes");
    CHECK(source.value()->read_binary("loose/extra.txt").value() == bytes_of("loose bytes"));
    CHECK(source.value()->exists("loose/extra.txt"));

    // A path neither the pack nor the server has fails cleanly.
    auto missing = source.value()->read_binary("missing.bin");
    CHECK_FALSE(missing);
    CHECK_FALSE(source.value()->exists("missing.bin"));
}

TEST_CASE("opening a pack that the server does not serve fails") {
    TestHttpServer server;
    REQUIRE(server.ok());

    auto source = HttpFetchSource::open(server.base_url(), "absent.fepack");
    CHECK_FALSE(source);
    CHECK(source.error().message().find("404") != std::string::npos);
}

TEST_CASE("http_get rejects a non-http url and a malformed response") {
    auto bad_scheme = http_get("https://example.com/pack");
    CHECK_FALSE(bad_scheme);
    CHECK(bad_scheme.error().message().find("http://") != std::string::npos);
}
