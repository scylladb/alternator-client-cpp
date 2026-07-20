#include <scylladb/alternator/http_client.h>
#include <scylladb/alternator/live_nodes.h>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace scylladb::alternator;

namespace {

bool IntegrationEnabled() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION");
    return value != nullptr && std::string(value) == "1";
}

std::vector<std::string> IntegrationNodes() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_NODE");
    return {value != nullptr && std::string(value).size() > 0 ? std::string(value) : "172.41.0.2"};
}

std::uint16_t IntegrationHttpPort() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_HTTP_PORT");
    return static_cast<std::uint16_t>(value != nullptr ? std::stoi(value) : 9998);
}

std::uint16_t IntegrationHttpsPort() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_HTTPS_PORT");
    return static_cast<std::uint16_t>(value != nullptr ? std::stoi(value) : 9999);
}

std::vector<std::string> Hosts(const std::vector<Url>& nodes) {
    std::vector<std::string> out;
    out.reserve(nodes.size());
    for (const auto& node : nodes) {
        out.push_back(node.host);
    }
    return out;
}

class LocalDnsEntrypointServer {
public:
    explicit LocalDnsEntrypointServer(std::string body)
        : body_(std::move(body)) {
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

            char buffer[2048];
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n > 0) {
                request_.assign(buffer, static_cast<std::size_t>(n));
            }

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << body_.size() << "\r\n"
                     << "Connection: close\r\n"
                     << "\r\n"
                     << body_;
            const auto response_text = response.str();
            send(client, response_text.data(), response_text.size(), 0);
            close(client);
        });
    }

    ~LocalDnsEntrypointServer() {
        Wait();
    }

    void Wait() {
        if (fd_ >= 0) {
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
    std::string request_;
    std::thread worker_;
};

Config IntegrationConfig(std::uint16_t port) {
    Config cfg;
    cfg.port = port;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.disabled = true;
    return cfg;
}

std::string FetchIntegrationLocalNodesBody() {
    auto cfg = IntegrationConfig(IntegrationHttpPort());
    auto client = NewDefaultHttpClient(cfg);
    auto url = Url::FromHostPort("http", IntegrationNodes()[0], IntegrationHttpPort())
        .WithPathAndQuery("/localnodes");
    auto response = client->Get(url);
    if (response.status_code != 200) {
        throw std::runtime_error("integration /localnodes returned HTTP " + std::to_string(response.status_code));
    }
    return response.body;
}

} // namespace

#define REQUIRE_INTEGRATION()                                                                    \
    do {                                                                                         \
        if (!IntegrationEnabled()) {                                                             \
            GTEST_SKIP() << "set ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 to run live Alternator integration tests"; \
        }                                                                                        \
    } while (false)

TEST(AlternatorLiveNodesIntegration, RoutingFallbackLearnsNodes) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.routing_scope = NewDCScope("wrongDC", NewDCScope("datacenter1"));

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    const auto discovered = Hosts(nodes.GetNodes());
    ASSERT_FALSE(discovered.empty());
    EXPECT_NE(discovered, IntegrationNodes());
}

TEST(AlternatorLiveNodesIntegration, CompressedHttpDiscoveryWorks) {
    REQUIRE_INTEGRATION();
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    GTEST_SKIP() << "zlib support is not enabled";
#endif

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_FALSE(nodes.GetNodes().empty());
}

TEST(AlternatorLiveNodesIntegration, DnsEntrypointDiscoversLiveClusterNodes) {
    REQUIRE_INTEGRATION();

    LocalDnsEntrypointServer server(FetchIntegrationLocalNodesBody());
    auto cfg = IntegrationConfig(server.Port());

    AlternatorLiveNodes nodes({"localhost"}, cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    server.Wait();

    EXPECT_NE(server.Request().find("GET /localnodes HTTP/1.1"), std::string::npos);
    EXPECT_TRUE(server.Request().find("Host: localhost:") != std::string::npos ||
        server.Request().find("host: localhost:") != std::string::npos);
    EXPECT_FALSE(nodes.GetNodes().empty());
}

TEST(AlternatorLiveNodesIntegration, RejectsWrongDatacenter) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.routing_scope = NewDCScope("wrongDC");

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodesIntegration, AcceptsCorrectDatacenter) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.routing_scope = NewDCScope("datacenter1");

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
}

TEST(AlternatorLiveNodesIntegration, RejectsWrongRack) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.routing_scope = NewRackScope("datacenter1", "wrongRack");

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodesIntegration, AcceptsCorrectRack) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.routing_scope = NewRackScope("datacenter1", "rack1");

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
}

TEST(AlternatorLiveNodesIntegration, DetectsRackDatacenterFeatureSupport) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpPort());
    cfg.routing_scope = NewDCScope("datacenter1");

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
}

TEST(AlternatorLiveNodesIntegration, HttpsDiscoveryWorksWhenCertificateVerificationIsDisabled) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpsPort());
    cfg.scheme = "https";
    cfg.verify_ssl = false;
    cfg.routing_scope = NewDCScope("datacenter1");

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_FALSE(nodes.GetNodes().empty());
}
