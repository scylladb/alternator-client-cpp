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

#include <arpa/inet.h>
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
#include <chrono>
#include <cstring>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
#include <limits>
#endif
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace scylladb::alternator;

namespace {

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
            close(fd_);
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
