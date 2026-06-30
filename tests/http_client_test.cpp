#include <scylladb/alternator/http_client.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
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
    LocalHttpServer() {
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
        if (listen(fd_, 1) != 0) {
            throw std::runtime_error("listen failed");
        }

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);

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

            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 15\r\n"
                "Connection: close\r\n"
                "\r\n"
                "[\"node1.local\"]";
            send(client, response.data(), response.size(), 0);
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

} // namespace

TEST(HttpClient, PerformsPlainHttpGet) {
    LocalHttpServer server;

    Config cfg;
    cfg.scheme = "http";
    CurlHttpClient client(cfg);

    auto response = client.Get(Url("http", "127.0.0.1", server.Port()).WithPathAndQuery("/localnodes", "dc=dc1"));

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "[\"node1.local\"]");
    EXPECT_NE(server.Request().find("GET /localnodes?dc=dc1 HTTP/1.1"), std::string::npos);
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
