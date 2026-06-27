#include <scylladb/alternator/live_nodes.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace scylladb::alternator;

class FakeHttpClient final : public HttpClient {
public:
    using Handler = std::function<HttpResponse(const Url&)>;

    explicit FakeHttpClient(Handler handler)
        : handler_(std::move(handler)) {}

    HttpResponse Get(const Url& url) const override {
        return handler_(url);
    }

private:
    Handler handler_;
};

static std::vector<std::string> Hosts(const std::vector<Url>& nodes) {
    std::vector<std::string> out;
    out.reserve(nodes.size());
    for (const auto& node : nodes) {
        out.push_back(node.host);
    }
    return out;
}

static std::int64_t HashWhereFirstNodeIs(const std::vector<Url>& nodes, const Url& target) {
    for (std::int64_t hash = 0; hash < 100000; ++hash) {
        if (FirstNodeWithSeed(nodes, hash) == target) {
            return hash;
        }
    }
    throw std::runtime_error("failed to find hash for target node");
}

TEST(AlternatorLiveNodes, RoutingScopeFallbackRetriesKnownNodes) {
    Config cfg;
    cfg.routing_scope = NewDCScope("wrong", NewDCScope("target"));
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> fallback_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        if (url.query == "dc=wrong") {
            return HttpResponse{200, "[]"};
        }
        if (url.query == "dc=target") {
            ++fallback_requests;
            return HttpResponse{200, "[\"node3.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"node1.local", "node2.local"}, cfg, http);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node3.local"}));
    EXPECT_GT(fallback_requests.load(), 0);
}

TEST(AlternatorLiveNodes, ClusterScopeMergesSeedNodes) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> dc1_requests{0};
    std::atomic<int> dc2_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "");
        if (url.host == "dc1-node1.local") {
            ++dc1_requests;
            return HttpResponse{200, "[\"dc1-node1.local\",\"dc1-node2.local\"]"};
        }
        if (url.host == "dc2-node1.local") {
            ++dc2_requests;
            return HttpResponse{200, "[\"dc2-node1.local\",\"dc2-node2.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"dc1-node1.local", "dc2-node1.local"}, cfg, http);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({
                  "dc1-node1.local",
                  "dc1-node2.local",
                  "dc2-node1.local",
                  "dc2-node2.local",
              }));
    EXPECT_GT(dc1_requests.load(), 0);
    EXPECT_GT(dc2_requests.load(), 0);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyRejectsWrongDatacenter) {
    Config cfg;
    cfg.routing_scope = NewDCScope("wrongDC");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=wrongDC");
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyAcceptsCorrectDatacenter) {
    Config cfg;
    cfg.routing_scope = NewDCScope("datacenter1");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=datacenter1");
        ++requests;
        return HttpResponse{200, "[\"node1.local\"]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
    EXPECT_GT(requests.load(), 0);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyRejectsWrongRack) {
    Config cfg;
    cfg.routing_scope = NewRackScope("datacenter1", "wrongRack");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=datacenter1&rack=wrongRack");
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyAcceptsCorrectRack) {
    Config cfg;
    cfg.routing_scope = NewRackScope("datacenter1", "rack1");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=datacenter1&rack=rack1");
        ++requests;
        return HttpResponse{200, "[\"node1.local\"]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
    EXPECT_GT(requests.load(), 0);
}

TEST(AlternatorLiveNodes, CheckRackDatacenterFeatureSupport) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        if (url.path == "/localnodes" && url.query == "rack=fakeRack") {
            return HttpResponse{200, "[]"};
        }
        if (url.path == "/localnodes") {
            return HttpResponse{200, "[\"node1.local\"]"};
        }
        return HttpResponse{200, ""};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
}

TEST(AlternatorLiveNodes, FeatureSupportProbeUsesSingleNode) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::vector<std::string> requested_hosts;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        requested_hosts.push_back(url.host);
        if (url.path == "/localnodes" && url.query == "rack=fakeRack") {
            return HttpResponse{200, "[]"};
        }
        if (url.path == "/localnodes") {
            return HttpResponse{200, "[\"node1.local\",\"node2.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"node1.local", "node2.local"}, cfg, http);

    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
    ASSERT_EQ(requested_hosts.size(), 2U);
    EXPECT_EQ(requested_hosts[0], requested_hosts[1]);
}

TEST(AlternatorLiveNodes, ProbeDownNodesMovesResponsiveNodeToQuarantine) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    auto responsive = nodes.ProbeDownNodes();

    EXPECT_EQ(responsive, std::vector<Url>({recovering}));
    EXPECT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));
    EXPECT_TRUE(nodes.GetDownNodes().empty());
}

TEST(AlternatorLiveNodes, QuarantineTrafficIsSampledByConfiguredInterval) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url active("http", "active.local", 8080);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();

    EXPECT_EQ(nodes.NextNode(), active);
    EXPECT_EQ(nodes.NextNode(), recovering);
    EXPECT_EQ(nodes.NextNode(), active);
}

TEST(AlternatorLiveNodes, QuarantineHashAssignmentStaysExposedUntilVerified) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url active("http", "active.local", 8080);
    Url recovering("http", "recovering.local", 8080);
    const auto affinity_nodes = std::vector<Url>{active, recovering};
    const auto active_hash = HashWhereFirstNodeIs(affinity_nodes, active);
    const auto recovering_hash = HashWhereFirstNodeIs(affinity_nodes, recovering);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();

    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(active_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(active_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(active_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::Success);
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::Success);
    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"active.local", "recovering.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"active.local", "recovering.local"}));
}

TEST(AlternatorLiveNodes, QuarantineHashAssignmentIsRemovedWhenNodeGoesDown) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 1;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url active("http", "active.local", 8080);
    Url recovering("http", "recovering.local", 8080);
    const auto recovering_hash = HashWhereFirstNodeIs({active, recovering}, recovering);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();

    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    EXPECT_EQ(nodes.GetDownNodes(), std::vector<Url>({recovering}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)), std::vector<std::string>({"active.local"}));
}

TEST(AlternatorLiveNodes, QuarantinePromotesAfterSuccessfulTraffic) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();
    ASSERT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::Success);

    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
    EXPECT_TRUE(nodes.GetDownNodes().empty());
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"active.local", "recovering.local"}));
}

TEST(AlternatorLiveNodes, BackgroundProbesDownNodesPeriodically) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{10};
    cfg.node_health.quarantine_success_threshold = 2;

    std::atomic<int> probes{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.host == "recovering.local" && url.path == "/localnodes") {
            ++probes;
        }
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    nodes.Start();
    for (int i = 0; i < 50 && nodes.GetQuarantinedNodes().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    nodes.Stop();

    EXPECT_GT(probes.load(), 0);
    EXPECT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));
}
