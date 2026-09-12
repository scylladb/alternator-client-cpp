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

#include "integration_test_config.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace scylladb::alternator;

namespace {

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

std::string FetchIntegrationLocalNodesBody() {
    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    auto client = NewDefaultHttpClient(cfg);
    auto url = Url::FromHostPort(
                   "http",
                   scylladb::alternator::testing::IntegrationNodes()[0],
                   scylladb::alternator::testing::IntegrationPort(
                       testinfra::AlternatorTransport::Http))
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
        if (!scylladb::alternator::testing::IntegrationEnabled()) {                              \
            GTEST_SKIP() << "set ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 to run live Alternator integration tests"; \
        }                                                                                        \
    } while (false)

TEST(AlternatorLiveNodesIntegration, RoutingFallbackLearnsNodes) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.routing_scope = NewDCScope(
        "wrongDC",
        NewDCScope(scylladb::alternator::testing::IntegrationDatacenter()));

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    const auto discovered = Hosts(nodes.GetNodes());
    ASSERT_FALSE(discovered.empty());
    EXPECT_NE(discovered, scylladb::alternator::testing::IntegrationNodes());
}

TEST(AlternatorLiveNodesIntegration, CompressedHttpDiscoveryWorks) {
    REQUIRE_INTEGRATION();
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    GTEST_SKIP() << "zlib support is not enabled";
#endif

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_FALSE(nodes.GetNodes().empty());
}

TEST(AlternatorLiveNodesIntegration, DnsEntrypointDiscoversLiveClusterNodes) {
    REQUIRE_INTEGRATION();

    LocalDnsEntrypointServer server(FetchIntegrationLocalNodesBody());
    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.port = server.Port();

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

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.routing_scope = NewDCScope("wrongDC");

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodesIntegration, AcceptsCorrectDatacenter) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.routing_scope = NewDCScope(
        scylladb::alternator::testing::IntegrationDatacenter());

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
}

TEST(AlternatorLiveNodesIntegration, RejectsWrongRack) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.routing_scope = NewRackScope(
        scylladb::alternator::testing::IntegrationDatacenter(), "wrongRack");

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodesIntegration, AcceptsCorrectRack) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.routing_scope = NewRackScope(
        scylladb::alternator::testing::IntegrationDatacenter(),
        scylladb::alternator::testing::IntegrationRack());

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
}

TEST(AlternatorLiveNodesIntegration, DetectsRackDatacenterFeatureSupport) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.routing_scope = NewDCScope(
        scylladb::alternator::testing::IntegrationDatacenter());

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
}

TEST(AlternatorLiveNodesIntegration, HttpsDiscoveryWorksWhenCertificateVerificationIsDisabled) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Https);
    cfg.verify_ssl = false;
    cfg.routing_scope = NewDCScope(
        scylladb::alternator::testing::IntegrationDatacenter());

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_FALSE(nodes.GetNodes().empty());
}
