#include <scylladb/alternator/live_nodes.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
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

Config IntegrationConfig(std::uint16_t port) {
    Config cfg;
    cfg.port = port;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.disabled = true;
    return cfg;
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
