#include <scylladb/alternator/node_health.h>

#include <gtest/gtest.h>

using namespace scylladb::alternator;

TEST(NodeHealthStore, ConnectionFailureMarksNodeDownImmediately) {
    Url node("http", "node1.local", 8080);
    NodeHealthStore store({}, {node});

    ASSERT_EQ(store.GetActiveNodes(), std::vector<Url>({node}));

    store.ReportNodeResult(node, NodeHealthObservation::ConnectionFailure);

    EXPECT_TRUE(store.GetActiveNodes().empty());
    EXPECT_TRUE(store.GetQuarantinedNodes().empty());
    EXPECT_EQ(store.GetDownNodes(), std::vector<Url>({node}));
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Down);
}

TEST(NodeHealthStore, ConsecutiveServerErrorsMarkNodeDown) {
    Url node("http", "node1.local", 8080);
    NodeHealthStoreConfig cfg;
    cfg.consecutive_server_error_threshold = 2;
    NodeHealthStore store(cfg, {node});

    store.ReportNodeResult(node, NodeHealthObservation::ServerError);
    EXPECT_EQ(store.GetActiveNodes(), std::vector<Url>({node}));
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->consecutive_server_errors, 1U);

    store.ReportNodeResult(node, NodeHealthObservation::ServerError);
    EXPECT_TRUE(store.GetActiveNodes().empty());
    EXPECT_EQ(store.GetDownNodes(), std::vector<Url>({node}));
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Down);
    EXPECT_EQ(store.GetNodeStatus(node)->consecutive_server_errors, 2U);
}

TEST(NodeHealthStore, DownNodeSuccessMovesToQuarantine) {
    Url node("http", "node1.local", 8080);
    NodeHealthStoreConfig cfg;
    cfg.quarantine_success_threshold = 2;
    NodeHealthStore store(cfg, {node});

    store.ReportNodeResult(node, NodeHealthObservation::ConnectionFailure);
    ASSERT_EQ(store.GetDownNodes(), std::vector<Url>({node}));

    auto responsive = store.ProbeDownNodes([](const Url&, const NodeHealthStatus& status) {
        return status.state == NodeHealthState::Down
                   ? NodeHealthObservation::Success
                   : NodeHealthObservation::ConnectionFailure;
    });

    EXPECT_EQ(responsive, std::vector<Url>({node}));
    EXPECT_TRUE(store.GetActiveNodes().empty());
    EXPECT_EQ(store.GetQuarantinedNodes(), std::vector<Url>({node}));
    EXPECT_TRUE(store.GetDownNodes().empty());
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Quarantined);
    EXPECT_EQ(store.GetNodeStatus(node)->consecutive_successes, 1U);
}

TEST(NodeHealthStore, QuarantinePromotesToActiveAfterConfiguredSuccesses) {
    Url node("http", "node1.local", 8080);
    NodeHealthStoreConfig cfg;
    cfg.quarantine_success_threshold = 2;
    NodeHealthStore store(cfg, {node});

    store.ReportNodeResult(node, NodeHealthObservation::ConnectionFailure);
    store.ReportNodeResult(node, NodeHealthObservation::Success);
    ASSERT_EQ(store.GetQuarantinedNodes(), std::vector<Url>({node}));

    store.ReportNodeResult(node, NodeHealthObservation::Success);

    EXPECT_EQ(store.GetActiveNodes(), std::vector<Url>({node}));
    EXPECT_TRUE(store.GetQuarantinedNodes().empty());
    EXPECT_TRUE(store.GetDownNodes().empty());
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Active);
    EXPECT_EQ(store.GetNodeStatus(node)->consecutive_successes, 2U);
}

TEST(NodeHealthStore, QuarantinePromotionRequiresConsecutiveSuccesses) {
    Url node("http", "node1.local", 8080);
    NodeHealthStoreConfig cfg;
    cfg.consecutive_server_error_threshold = 3;
    cfg.quarantine_success_threshold = 2;
    NodeHealthStore store(cfg, {node});

    store.ReportNodeResult(node, NodeHealthObservation::ConnectionFailure);
    store.ReportNodeResult(node, NodeHealthObservation::Success);
    ASSERT_EQ(store.GetQuarantinedNodes(), std::vector<Url>({node}));

    store.ReportNodeResult(node, NodeHealthObservation::ServerError);
    store.ReportNodeResult(node, NodeHealthObservation::Success);

    EXPECT_TRUE(store.GetActiveNodes().empty());
    EXPECT_EQ(store.GetQuarantinedNodes(), std::vector<Url>({node}));
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Quarantined);
    EXPECT_EQ(store.GetNodeStatus(node)->consecutive_successes, 1U);

    store.ReportNodeResult(node, NodeHealthObservation::Success);

    EXPECT_EQ(store.GetActiveNodes(), std::vector<Url>({node}));
    EXPECT_TRUE(store.GetQuarantinedNodes().empty());
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Active);
}

TEST(NodeHealthStore, QuarantineFailureReturnsNodeToDown) {
    Url node("http", "node1.local", 8080);
    NodeHealthStoreConfig cfg;
    cfg.consecutive_server_error_threshold = 1;
    cfg.quarantine_success_threshold = 2;
    NodeHealthStore store(cfg, {node});

    store.ReportNodeResult(node, NodeHealthObservation::ConnectionFailure);
    store.ReportNodeResult(node, NodeHealthObservation::Success);
    ASSERT_EQ(store.GetQuarantinedNodes(), std::vector<Url>({node}));

    store.ReportNodeResult(node, NodeHealthObservation::ServerError);

    EXPECT_TRUE(store.GetQuarantinedNodes().empty());
    EXPECT_EQ(store.GetDownNodes(), std::vector<Url>({node}));
    ASSERT_TRUE(store.GetNodeStatus(node).has_value());
    EXPECT_EQ(store.GetNodeStatus(node)->state, NodeHealthState::Down);
}

TEST(NodeHealthStore, DisabledKeepsAllNodesActive) {
    Url node("http", "node1.local", 8080);
    NodeHealthStoreConfig cfg;
    cfg.disabled = true;

    NodeHealthStore store(cfg, {node});
    store.ReportNodeResult(node, NodeHealthObservation::ConnectionFailure);

    EXPECT_EQ(store.GetActiveNodes(), std::vector<Url>({node}));
    EXPECT_TRUE(store.GetQuarantinedNodes().empty());
    EXPECT_TRUE(store.GetDownNodes().empty());
}
