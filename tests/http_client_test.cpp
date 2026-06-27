#include <scylladb/alternator/http_client.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>

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
