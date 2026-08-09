/*
 * Copyright ScyllaDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <scylladb/alternator/http_client.h>
#include <scylladb/alternator/live_nodes.h>

#include <arpa/inet.h>
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL && defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#endif
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
#include <zlib.h>

#include <array>
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
#include <limits>
#endif
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace scylladb::alternator;

namespace {

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value)
        : name_(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) {
            previous_ = previous;
        }
        if (setenv(name, value, 1) != 0) {
            throw std::runtime_error("setenv failed");
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class LocalHttpServer {
public:
    explicit LocalHttpServer(
        std::string body = "[\"node1.local\"]",
        std::string content_encoding = {},
        int address_family = AF_INET)
        : body_(std::move(body))
        , content_encoding_(std::move(content_encoding)) {
        fd_ = socket(address_family, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket failed");
        }

        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (address_family == AF_INET6) {
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_addr = in6addr_loopback;
            addr.sin6_port = 0;
            if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                throw std::runtime_error("IPv6 bind failed");
            }
            socklen_t len = sizeof(addr);
            if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
                throw std::runtime_error("IPv6 getsockname failed");
            }
            port_ = ntohs(addr.sin6_port);
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                throw std::runtime_error("bind failed");
            }
            socklen_t len = sizeof(addr);
            if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
                throw std::runtime_error("getsockname failed");
            }
            port_ = ntohs(addr.sin_port);
        }
        if (listen(fd_, 1) != 0) {
            throw std::runtime_error("listen failed");
        }

        worker_ = std::thread([this] {
            int client = accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return;
            }

            char buffer[1024];
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n > 0) {
                request_.assign(buffer, static_cast<std::size_t>(n));
            }

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n";
            if (!content_encoding_.empty()) {
                response << "Content-Encoding: " << content_encoding_ << "\r\n";
            }
            response << "Content-Length: " << body_.size() << "\r\n"
                     << "Connection: close\r\n"
                     << "\r\n"
                     << body_;
            const auto response_text = response.str();
            send(client, response_text.data(), response_text.size(), 0);
            close(client);
        });
    }

    ~LocalHttpServer() {
        if (fd_ >= 0) {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            fd_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

    [[nodiscard]] const std::string& Request() const {
        return request_;
    }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::string body_;
    std::string content_encoding_;
    std::string request_;
    std::thread worker_;
};

class UnavailableIpv6Socket {
public:
    explicit UnavailableIpv6Socket(std::uint16_t port) {
        fd_ = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("IPv6 socket failed");
        }
        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_loopback;
        address.sin6_port = htons(port);
        if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("IPv6 unavailable-address bind failed");
        }
    }

    ~UnavailableIpv6Socket() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    UnavailableIpv6Socket(const UnavailableIpv6Socket&) = delete;
    UnavailableIpv6Socket& operator=(const UnavailableIpv6Socket&) = delete;

private:
    int fd_ = -1;
};

#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL && defined(__linux__)
class SaturatedTcpListener {
public:
    SaturatedTcpListener() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(fd_, 1) != 0) {
            Close();
            return;
        }
        socklen_t address_size = sizeof(address);
        if (getsockname(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) != 0) {
            Close();
            return;
        }
        port_ = ntohs(address.sin_port);

        // Linux leaves further loopback connects pending once this deliberately
        // tiny accept queue is full. That provides a local, packet-loss-free
        // connect-timeout fixture without depending on an external address.
        for (int attempt = 0; attempt < 32; ++attempt) {
            const int client = socket(AF_INET, SOCK_STREAM, 0);
            if (client < 0) {
                break;
            }
            const int flags = fcntl(client, F_GETFL, 0);
            if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) != 0) {
                close(client);
                break;
            }
            const int result = connect(
                client,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address));
            if (result != 0 && errno != EINPROGRESS) {
                close(client);
                continue;
            }
            clients_.push_back(client);
            if (result == 0) {
                continue;
            }

            pollfd descriptor{};
            descriptor.fd = client;
            descriptor.events = POLLOUT;
            int poll_result = 0;
            do {
                poll_result = poll(&descriptor, 1, 20);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result == 0) {
                saturated_ = true;
                break;
            }
            if (poll_result < 0) {
                break;
            }
            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (getsockopt(
                    client,
                    SOL_SOCKET,
                    SO_ERROR,
                    &socket_error,
                    &socket_error_size) != 0 ||
                socket_error != 0) {
                close(client);
                clients_.pop_back();
            }
        }
    }

    ~SaturatedTcpListener() {
        Close();
    }

    SaturatedTcpListener(const SaturatedTcpListener&) = delete;
    SaturatedTcpListener& operator=(const SaturatedTcpListener&) = delete;

    [[nodiscard]] bool Ready() const {
        return saturated_;
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

private:
    void Close() {
        for (const int client : clients_) {
            close(client);
        }
        clients_.clear();
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    int fd_ = -1;
    std::uint16_t port_ = 0;
    bool saturated_ = false;
    std::vector<int> clients_;
};
#endif

class FixedAddressHttpClient final : public HttpClient {
public:
    FixedAddressHttpClient(Config config, std::vector<std::string> addresses)
        : delegate_(std::move(config))
        , addresses_(std::move(addresses)) {}

    HttpResponse Get(const Url& url) const override {
        return delegate_.Get(url);
    }

    std::vector<std::string> Resolve(const Url&) const override {
        return addresses_;
    }

    HttpResponse GetResolved(
        const Url& url,
        const std::string& resolved_address) const override {
        RecordAttempt(resolved_address);
        try {
            auto response = delegate_.GetResolved(url, resolved_address);
            FinishAttempt();
            return response;
        } catch (...) {
            FinishAttempt();
            throw;
        }
    }

    HttpResponse GetResolvedWithTimeout(
        const Url& url,
        const std::string& resolved_address,
        std::chrono::milliseconds timeout) const override {
        RecordAttempt(resolved_address);
        try {
            auto response =
                delegate_.GetResolvedWithTimeout(url, resolved_address, timeout);
            FinishAttempt();
            return response;
        } catch (...) {
            FinishAttempt();
            throw;
        }
    }

    [[nodiscard]] std::vector<std::string> Attempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return attempts_;
    }

    [[nodiscard]] bool WaitForAttempts(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds{1}) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, expected] {
            return attempts_.size() >= expected;
        });
    }

    [[nodiscard]] bool WaitForIdle(
        std::chrono::milliseconds timeout = std::chrono::seconds{1}) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] {
            return active_attempts_ == 0U;
        });
    }

private:
    void RecordAttempt(const std::string& resolved_address) const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            attempts_.push_back(resolved_address);
            ++active_attempts_;
        }
        condition_.notify_all();
    }

    void FinishAttempt() const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --active_attempts_;
        }
        condition_.notify_all();
    }

    CurlHttpClient delegate_;
    std::vector<std::string> addresses_;
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    mutable std::vector<std::string> attempts_;
    mutable std::size_t active_attempts_ = 0;
};

class CountingHttpServer {
public:
    CountingHttpServer(int expected_requests, bool keep_alive)
        : CountingHttpServer(std::vector<int>(static_cast<std::size_t>(expected_requests), 200), keep_alive) {}

    CountingHttpServer(std::vector<int> response_statuses, bool keep_alive)
        : expected_requests_(static_cast<int>(response_statuses.size()))
        , keep_alive_(keep_alive) {
        response_statuses_ = std::move(response_statuses);
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket failed");
        }

        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind failed");
        }
        if (listen(fd_, expected_requests_) != 0) {
            throw std::runtime_error("listen failed");
        }

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);

        worker_ = std::thread([this] {
            while (request_count_.load() < expected_requests_) {
                int client = accept(fd_, nullptr, nullptr);
                if (client < 0) {
                    return;
                }
                ++accept_count_;

                while (request_count_.load() < expected_requests_) {
                    auto request = ReadRequest(client);
                    if (request.empty()) {
                        break;
                    }
                    requests_.push_back(std::move(request));
                    const auto response_index = request_count_.fetch_add(1);

                    const std::string body = "[\"node1.local\"]";
                    const bool keep_connection =
                        keep_alive_ && request_count_.load() < expected_requests_;
                    const auto status_code = response_statuses_.at(static_cast<std::size_t>(response_index));
                    std::ostringstream response;
                    response << "HTTP/1.1 " << status_code << " " << ReasonPhrase(status_code) << "\r\n"
                             << "Content-Length: " << body.size() << "\r\n"
                             << "Connection: " << (keep_connection ? "keep-alive" : "close") << "\r\n"
                             << "\r\n"
                             << body;
                    const auto response_text = response.str();
                    send(client, response_text.data(), response_text.size(), 0);
                    if (!keep_connection) {
                        break;
                    }
                }
                close(client);
            }
        });
    }

    ~CountingHttpServer() {
        if (fd_ >= 0) {
            close(fd_);
        }
        Wait();
    }

    void Wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

    [[nodiscard]] int AcceptCount() const {
        return accept_count_.load();
    }

    [[nodiscard]] const std::vector<std::string>& Requests() const {
        return requests_;
    }

private:
    static std::string ReasonPhrase(int status_code) {
        switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Status";
        }
    }

    static std::string ReadRequest(int client) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return {};
            }
            request.append(buffer, static_cast<std::size_t>(n));
        }
        return request;
    }

    int fd_ = -1;
    int expected_requests_ = 0;
    bool keep_alive_ = false;
    std::vector<int> response_statuses_;
    std::uint16_t port_ = 0;
    std::atomic<int> accept_count_{0};
    std::atomic<int> request_count_{0};
    std::vector<std::string> requests_;
    std::thread worker_;
};

class StallingHttpServer {
public:
    StallingHttpServer() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket failed");
        }

        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(fd_, 1) != 0) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("bind/listen failed");
        }
        socklen_t address_size = sizeof(address);
        if (getsockname(
                fd_,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) != 0) {
            close(fd_);
            fd_ = -1;
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);

        worker_ = std::thread([this] {
            const int client = accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return;
            }
            accepted_.store(true);
            char buffer[1024];
            (void)recv(client, buffer, sizeof(buffer), 0);
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return release_; });
            close(client);
        });
    }

    ~StallingHttpServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_ = true;
        }
        condition_.notify_all();
        if (fd_ >= 0) {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            fd_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

    [[nodiscard]] bool Accepted() const {
        return accepted_.load();
    }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> accepted_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool release_ = false;
    std::thread worker_;
};

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
std::string CompressBody(const std::string& body, int window_bits) {
    if (body.size() > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("body too large to compress");
    }

    z_stream stream{};
    const auto init_code = deflateInit2(
        &stream,
        Z_BEST_SPEED,
        Z_DEFLATED,
        window_bits,
        8,
        Z_DEFAULT_STRATEGY);
    if (init_code != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    struct DeflateGuard {
        z_stream* stream;
        ~DeflateGuard() {
            deflateEnd(stream);
        }
    } guard{&stream};

    auto* input = reinterpret_cast<const Bytef*>(body.data());
    stream.next_in = const_cast<Bytef*>(input);
    stream.avail_in = static_cast<uInt>(body.size());

    std::array<char, 8192> buffer{};
    std::string output;
    while (true) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());

        const auto code = deflate(&stream, Z_FINISH);
        const auto produced = buffer.size() - stream.avail_out;
        output.append(buffer.data(), produced);

        if (code == Z_STREAM_END) {
            return output;
        }
        if (code != Z_OK) {
            throw std::runtime_error("deflate failed");
        }
    }
}
#endif

class TestContentEncodingDecoder final : public HttpContentEncodingDecoder {
public:
    explicit TestContentEncodingDecoder(std::vector<std::string> accepted_encodings = {"br"})
        : accepted_encodings_(std::move(accepted_encodings)) {}

    std::vector<std::string> AcceptedResponseEncodings() const override {
        return accepted_encodings_;
    }

    std::string Decode(std::string body, const std::string& content_encoding) const override {
        return content_encoding + ":" + body;
    }

private:
    std::vector<std::string> accepted_encodings_;
};

} // namespace

bool LocalhostHasAddressFamily(int family) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (getaddrinfo("localhost", nullptr, &hints, &raw) != 0) {
        return false;
    }
    freeaddrinfo(raw);
    return true;
}

bool LocalhostUnspecifiedLookupHasAddressFamily(int family) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* raw = nullptr;
    if (getaddrinfo("localhost", nullptr, &hints, &raw) != 0) {
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(raw, &freeaddrinfo);
    for (auto* current = results.get(); current != nullptr; current = current->ai_next) {
        if (current->ai_family == family) {
            return true;
        }
    }
    return false;
}

TEST(HttpClient, PerformsPlainHttpGet) {
    LocalHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes", "dc=dc1"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "[\"node1.local\"]");
    EXPECT_NE(server.Request().find("GET /localnodes?dc=dc1 HTTP/1.1"), std::string::npos);
    EXPECT_EQ(server.Request().find("Accept-Encoding:"), std::string::npos);
}

TEST(HttpClient, ResolvedAddressPreservesLogicalHostHeader) {
    LocalHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);
    const auto logical_url = Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes");

    auto response = client.GetResolved(logical_url, "127.0.0.1");

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "[\"node1.local\"]");
    EXPECT_NE(server.Request().find("GET /localnodes HTTP/1.1"), std::string::npos);
    EXPECT_NE(
        server.Request().find("Host: localhost:" + std::to_string(server.Port())),
        std::string::npos);
}

TEST(HttpClient, ResolvedAddressBypassesProxyEnvironment) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
    LocalHttpServer server;
    ScopedEnvironmentVariable proxy("http_proxy", "http://127.0.0.1:1");
    ScopedEnvironmentVariable no_proxy("no_proxy", "");

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);
    const auto logical_url = Url(
        "http",
        "proxy-must-not-resolve.invalid",
        server.Port()).WithPathAndQuery("/localnodes");

    const auto response = client.GetResolved(logical_url, "127.0.0.1");

    EXPECT_EQ(response.status_code, 200);
    EXPECT_NE(
        server.Request().find(
            "Host: proxy-must-not-resolve.invalid:" + std::to_string(server.Port())),
        std::string::npos);
#else
    GTEST_SKIP() << "proxy environment behavior is libcurl-specific";
#endif
}

TEST(HttpClient, ResolvedIPv6AddressPreservesLogicalHostHeader) {
    LocalHttpServer server("[\"::1\"]", {}, AF_INET6);

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);
    const auto logical_url = Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes");

    auto response = client.GetResolved(logical_url, "::1");

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "[\"::1\"]");
    EXPECT_NE(
        server.Request().find("Host: localhost:" + std::to_string(server.Port())),
        std::string::npos);
}

TEST(HttpClient, ResolveReturnsUniqueNumericAddresses) {
    Config cfg;
    CurlHttpClient client(cfg);

    auto addresses = client.Resolve(Url("http", "localhost", 8080));

    ASSERT_FALSE(addresses.empty());
    auto unique = addresses;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    EXPECT_EQ(addresses.size(), unique.size());
    for (const auto& address : addresses) {
        in_addr ipv4{};
        in6_addr ipv6{};
        EXPECT_TRUE(inet_pton(AF_INET, address.c_str(), &ipv4) == 1 ||
                    inet_pton(AF_INET6, address.c_str(), &ipv6) == 1);
    }
}

TEST(HttpClient, ResolveReportsDnsFailure) {
    Config cfg;
    CurlHttpClient client(cfg);

    EXPECT_THROW(
        (void)client.Resolve(Url("http", "does-not-exist.invalid", 8080)),
        std::runtime_error);
}

TEST(HttpClient, ConfiguredTimeoutBoundsStalledResolvedAddress) {
    StallingHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    cfg.connect_timeout = std::chrono::milliseconds{100};
    cfg.http_client_timeout = std::chrono::milliseconds{100};
    CurlHttpClient client(cfg);
    const auto url = Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes");

    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW((void)client.GetResolved(url, "127.0.0.1"), std::runtime_error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(server.Accepted());
    EXPECT_GE(elapsed, std::chrono::milliseconds{50});
    EXPECT_LT(elapsed, std::chrono::seconds{2});
}

TEST(HttpClient, PerCallTimeoutBoundsStalledResolvedAddress) {
    StallingHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    cfg.connect_timeout = std::chrono::seconds{1};
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::seconds{2};
    CurlHttpClient client(cfg);
    const auto url = Url("http", "localhost", server.Port())
        .WithPathAndQuery("/localnodes");

    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(
        (void)client.GetResolvedWithTimeout(
            url,
            "127.0.0.1",
            std::chrono::milliseconds{60}),
        std::runtime_error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(server.Accepted());
    EXPECT_GE(elapsed, std::chrono::milliseconds{30});
    EXPECT_LT(elapsed, std::chrono::seconds{1});
}

TEST(HttpClient, RejectsResponseBodyAboveConfiguredDiscoveryLimit) {
    LocalHttpServer server(std::string(256, 'x'));

    Config cfg;
    cfg.scheme = "http";
    cfg.max_discovery_response_bytes = 32;
    CurlHttpClient client(cfg);
    const auto url = Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes");

    EXPECT_THROW((void)client.GetResolved(url, "127.0.0.1"), std::runtime_error);
}

TEST(HttpClient, RejectsGzipExpansionAboveConfiguredDiscoveryLimit) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const auto compressed = CompressBody(std::string(64U * 1024U, 'x'), MAX_WBITS + 16);
    ASSERT_LT(compressed.size(), 512U);
    LocalHttpServer server(compressed, "gzip");

    Config cfg;
    cfg.scheme = "http";
    cfg.max_discovery_response_bytes = 512;
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};
    CurlHttpClient client(cfg);
    const auto url = Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes");

    EXPECT_THROW((void)client.GetResolved(url, "127.0.0.1"), std::runtime_error);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, NoCurlConnectTimeoutBoundsSaturatedListener) {
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL && defined(__linux__)
    SaturatedTcpListener server;
    if (!server.Ready()) {
        GTEST_SKIP() << "could not saturate the local TCP accept queue";
    }

    Config cfg;
    cfg.scheme = "http";
    cfg.connect_timeout = std::chrono::milliseconds{100};
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::seconds{1};
    CurlHttpClient client(cfg);
    const auto url = Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes");

    std::string error_message;
    const auto started = std::chrono::steady_clock::now();
    try {
        (void)client.GetResolved(url, "127.0.0.1");
    } catch (const std::runtime_error& error) {
        error_message = error.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_NE(error_message.find("connect timed out"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::milliseconds{50});
    EXPECT_LT(elapsed, std::chrono::seconds{2});
#else
    GTEST_SKIP() << "deterministic saturated-listener fixture is Linux no-curl only";
#endif
}

TEST(HttpClient, DiscoverySafetyTimeoutBoundsAllUnavailableAddresses) {
    StallingHttpServer server;
    UnavailableIpv6Socket unavailable(server.Port());

    Config cfg;
    cfg.scheme = "http";
    cfg.port = server.Port();
    cfg.connect_timeout = std::chrono::seconds{1};
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds{100};
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    auto http = std::make_shared<FixedAddressHttpClient>(
        cfg,
        std::vector<std::string>{"127.0.0.1", "::1"});
    AlternatorLiveNodes nodes({"localhost"}, cfg, http);

    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(server.Accepted());
    EXPECT_TRUE(http->WaitForAttempts(2));
    EXPECT_TRUE(http->WaitForIdle());
    EXPECT_EQ(http->Attempts(), std::vector<std::string>({"127.0.0.1", "::1"}));
    EXPECT_GE(elapsed, std::chrono::milliseconds{50});
    EXPECT_LT(elapsed, std::chrono::seconds{2});
}

TEST(HttpClient, NoCurlDiscoveryFallsBackFromUnavailableIPv6ToIPv4) {
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
    LocalHttpServer server;
    UnavailableIpv6Socket unavailable(server.Port());

    Config cfg;
    cfg.scheme = "http";
    cfg.port = server.Port();
    cfg.connect_timeout = std::chrono::milliseconds{100};
    cfg.http_client_timeout = std::chrono::milliseconds{1000};
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};
    auto http = std::make_shared<FixedAddressHttpClient>(
        cfg,
        std::vector<std::string>{"::1", "127.0.0.1"});

    AlternatorLiveNodes nodes({"localhost"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    EXPECT_TRUE(http->WaitForAttempts(2));
    EXPECT_TRUE(http->WaitForIdle());
    EXPECT_EQ(http->Attempts(), std::vector<std::string>({"::1", "127.0.0.1"}));
    EXPECT_EQ(nodes.GetNodes(),
              std::vector<Url>({Url("http", "node1.local", server.Port())}));
#else
    GTEST_SKIP() << "plain socket fallback is only used without libcurl";
#endif
}

TEST(HttpClient, PerformsPlainHttpGetOverIPv6Literal) {
    LocalHttpServer server("[\"::1\"]", {}, AF_INET6);

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);
    const auto url = Url("http", "::1", server.Port()).WithPathAndQuery("/localnodes");

    const auto response = client.Get(url);

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "[\"::1\"]");
    EXPECT_EQ(url.Authority(), "[::1]:" + std::to_string(server.Port()));
    EXPECT_EQ(url.ToString(), "http://[::1]:" + std::to_string(server.Port()) + "/localnodes");
    EXPECT_NE(server.Request().find("Host: [::1]:" + std::to_string(server.Port())), std::string::npos);
}

TEST(HttpClient, DualStackDnsFallsBackToReachableIPv4) {
    if (!LocalhostHasAddressFamily(AF_INET) || !LocalhostHasAddressFamily(AF_INET6)) {
        GTEST_SKIP() << "localhost does not resolve to both IPv4 and IPv6";
    }
    LocalHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    cfg.connect_timeout = std::chrono::milliseconds(500);
    CurlHttpClient client(cfg);

    EXPECT_EQ(client.Get(Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes")).status_code, 200);
}

TEST(HttpClient, DualStackDnsFallsBackToReachableIPv6) {
    if (!LocalhostHasAddressFamily(AF_INET) || !LocalhostHasAddressFamily(AF_INET6)) {
        GTEST_SKIP() << "localhost does not resolve to both IPv4 and IPv6";
    }
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
    if (!LocalhostUnspecifiedLookupHasAddressFamily(AF_INET6)) {
        GTEST_SKIP() << "AF_UNSPEC localhost lookup does not return IPv6";
    }
#endif
    LocalHttpServer server("[\"::1\"]", {}, AF_INET6);

    Config cfg;
    cfg.scheme = "http";
    cfg.connect_timeout = std::chrono::milliseconds(500);
    CurlHttpClient client(cfg);

    EXPECT_EQ(client.Get(Url("http", "localhost", server.Port()).WithPathAndQuery("/localnodes")).status_code, 200);
}

TEST(HttpClient, RequestsAndDecodesGzipResponse) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const std::string body = "[\"node1.local\"]";
    LocalHttpServer server(CompressBody(body, MAX_WBITS + 16), "gzip");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, body);
    EXPECT_NE(server.Request().find("Accept-Encoding: gzip"), std::string::npos);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, GzipRequestCompressorRoundTripsWithZlibDecoder) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const std::string body = R"({"TableName":"orders","Limit":10})";
    const GzipRequestCompressor compressor(0);
    const ZlibContentEncodingDecoder decoder({"gzip"});
    std::istringstream input(body);
    std::ostringstream output;

    EXPECT_EQ(compressor.ContentEncoding(), "gzip");
    EXPECT_TRUE(compressor.Compress(input, body.size(), output));
    EXPECT_EQ(decoder.Decode(output.str(), compressor.ContentEncoding()), body);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, GzipRequestCompressorSkipsBodiesBelowMinimumSize) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const std::string body = R"({"TableName":"orders"})";
    const GzipRequestCompressor compressor(1024);
    std::istringstream input(body);
    std::ostringstream output;

    EXPECT_FALSE(compressor.Compress(input, body.size(), output));
    EXPECT_EQ(output.str(), "");
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, CanUseExplicitZlibContentEncodingDecoder) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const std::string body = "[\"node1.local\"]";
    LocalHttpServer server(CompressBody(body, MAX_WBITS + 16), "gzip");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, body);
    EXPECT_NE(server.Request().find("Accept-Encoding: gzip"), std::string::npos);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, CanAdvertiseZlibContentEncodingSubset) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const std::string body = "[\"node1.local\"]";
    LocalHttpServer server(CompressBody(body, MAX_WBITS + 16), "gzip");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {
        std::make_shared<ZlibContentEncodingDecoder>(std::vector<std::string>{"gzip"}),
    };
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, body);
    EXPECT_NE(server.Request().find("Accept-Encoding: gzip"), std::string::npos);
    EXPECT_EQ(server.Request().find("deflate"), std::string::npos);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, RequestsAndDecodesDeflateResponse) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const std::string body = "[\"node1.local\"]";
    LocalHttpServer server(CompressBody(body, MAX_WBITS), "deflate");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, body);
    EXPECT_NE(server.Request().find("Accept-Encoding: gzip, deflate"), std::string::npos);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, CanAdvertiseGzipAndCustomZstdResponseEncodings) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    LocalHttpServer server("encoded-body", "zstd");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {
        std::make_shared<ZlibContentEncodingDecoder>(std::vector<std::string>{"gzip"}),
        std::make_shared<TestContentEncodingDecoder>(std::vector<std::string>{"zstd"}),
    };
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "zstd:encoded-body");
    EXPECT_NE(server.Request().find("Accept-Encoding: gzip, zstd"), std::string::npos);
    EXPECT_EQ(server.Request().find("deflate"), std::string::npos);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, RejectsUnacceptedResponseEncoding) {
    LocalHttpServer server("encoded", "deflate");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {std::make_shared<TestContentEncodingDecoder>(std::vector<std::string>{"gzip"})};
    CurlHttpClient client(cfg);

    EXPECT_THROW(
        client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes")),
        std::runtime_error);
}

TEST(HttpClient, UsesCustomContentEncodingDecoder) {
    LocalHttpServer server("encoded-body", "br");

    Config cfg;
    cfg.scheme = "http";
    cfg.content_encoding_decoders = {std::make_shared<TestContentEncodingDecoder>()};
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "br:encoded-body");
    EXPECT_NE(server.Request().find("Accept-Encoding: br"), std::string::npos);
}

TEST(HttpClient, RejectsUnsupportedZlibResponseEncoding) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    EXPECT_THROW(
        ZlibContentEncodingDecoder(std::vector<std::string>{"zstd"}),
        std::invalid_argument);
#else
    GTEST_SKIP() << "zlib support is not enabled";
#endif
}

TEST(HttpClient, DoesNotAdvertiseResponseCompressionByDefault) {
    LocalHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "[\"node1.local\"]");
    EXPECT_EQ(server.Request().find("Accept-Encoding:"), std::string::npos);
}

TEST(HttpClient, ReusesDiscoveryConnectionByDefaultWithCurl) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
    CountingHttpServer server(2, true);

    Config cfg;
    cfg.scheme = "http";
    cfg.reuse_discovery_connections = true;
    CurlHttpClient client(cfg);
    const auto url = Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes");

    EXPECT_EQ(client.Get(url).status_code, 200);
    EXPECT_EQ(client.Get(url).status_code, 200);
    server.Wait();

    EXPECT_EQ(server.Requests().size(), 2U);
    EXPECT_EQ(server.AcceptCount(), 1);
#else
    GTEST_SKIP() << "libcurl support is not enabled";
#endif
}

TEST(HttpClient, ReusesDiscoveryConnectionAfterRepeatedNonSuccessResponsesWithCurl) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
    CountingHttpServer server(std::vector<int>{500, 503, 200}, true);

    Config cfg;
    cfg.scheme = "http";
    cfg.reuse_discovery_connections = true;
    CurlHttpClient client(cfg);
    const auto url = Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes");

    EXPECT_EQ(client.Get(url).status_code, 500);
    EXPECT_EQ(client.Get(url).status_code, 503);
    EXPECT_EQ(client.Get(url).status_code, 200);
    server.Wait();

    EXPECT_EQ(server.Requests().size(), 3U);
    EXPECT_EQ(server.AcceptCount(), 1);
#else
    GTEST_SKIP() << "libcurl support is not enabled";
#endif
}

TEST(HttpClient, CanDisableDiscoveryConnectionReuseWithCurl) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
    CountingHttpServer server(2, false);

    Config cfg;
    cfg.scheme = "http";
    cfg.reuse_discovery_connections = false;
    CurlHttpClient client(cfg);
    const auto url = Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes");

    EXPECT_EQ(client.Get(url).status_code, 200);
    EXPECT_EQ(client.Get(url).status_code, 200);
    server.Wait();

    EXPECT_EQ(server.Requests().size(), 2U);
    EXPECT_EQ(server.AcceptCount(), 2);
#else
    GTEST_SKIP() << "libcurl support is not enabled";
#endif
}
