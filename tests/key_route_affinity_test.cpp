#include <scylladb/alternator/key_route_affinity.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

using namespace scylladb::alternator;

namespace {

class StaticNodes final : public NodesSource {
public:
    StaticNodes(std::vector<Url> active_nodes,
                std::vector<Url> down_nodes = {},
                std::vector<Url> query_plan_nodes = {})
        : active_nodes_(std::move(active_nodes))
        , down_nodes_(std::move(down_nodes))
        , query_plan_nodes_(std::move(query_plan_nodes)) {}

    std::vector<Url> GetActiveNodes() const override {
        return active_nodes_;
    }

    std::vector<Url> GetQueryPlanNodes() const override {
        if (!query_plan_nodes_.empty()) {
            return query_plan_nodes_;
        }
        return active_nodes_;
    }

    std::vector<Url> GetDownNodes() const override {
        return down_nodes_;
    }

private:
    std::vector<Url> active_nodes_;
    std::vector<Url> down_nodes_;
    std::vector<Url> query_plan_nodes_;
};

std::vector<Url> BatchWriteTestNodes() {
    return {
        {"http", "node2.example.com", 8000},
        {"http", "node10.example.com", 8000},
        {"http", "node1.example.com", 8000},
        {"http", "node3.example.com", 8000},
    };
}

std::vector<Url> BatchWriteSortedTestNodes() {
    auto nodes = BatchWriteTestNodes();
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

Url NodeForValue(const AttributeValue& value) {
    return FirstNodeWithSeed(BatchWriteTestNodes(), value.Hash());
}

std::vector<std::string> StringKeysForNode(const Url& target, int count) {
    std::vector<std::string> keys;
    for (int i = 0; i < 10000 && static_cast<int>(keys.size()) < count; ++i) {
        auto key = target.host + "-key-" + std::to_string(i);
        if (NodeForValue(AttributeValue::String(key)) == target) {
            keys.push_back(std::move(key));
        }
    }
    EXPECT_EQ(keys.size(), static_cast<std::size_t>(count));
    return keys;
}

AttributeMap KeyWithId(const std::string& id) {
    return {{"id", AttributeValue::String(id)}};
}

AttributeMap ItemWithId(const std::string& id, const std::string& payload) {
    return {
        {"id", AttributeValue::String(id)},
        {"data", AttributeValue::String("payload")},
        {"payload", AttributeValue::String(payload)},
    };
}

std::vector<std::string> PlanHosts(QueryPlan plan) {
    std::vector<std::string> hosts;
    for (auto node = plan.Next(); !node.Empty(); node = plan.Next()) {
        hosts.push_back(node.host);
    }
    return hosts;
}

std::vector<std::string> SortedHostsExcept(const std::set<std::string>& excluded) {
    std::vector<std::string> hosts;
    for (const auto& node : BatchWriteSortedTestNodes()) {
        if (excluded.find(node.host) == excluded.end()) {
            hosts.push_back(node.host);
        }
    }
    return hosts;
}

PartitionKeyMetadata Metadata(std::map<std::string, std::string> partition_key_by_table) {
    return PartitionKeyMetadata(std::move(partition_key_by_table));
}

} // namespace

TEST(QueryPlan, PreferredNodesComeFirstThenSortedRemaining) {
    const auto nodes = BatchWriteTestNodes();
    const auto preferred = std::vector<Url>{BatchWriteSortedTestNodes()[3], BatchWriteSortedTestNodes()[1]};
    QueryPlan plan = QueryPlan::WithPreferredNodes(nodes, preferred);

    auto hosts = PlanHosts(std::move(plan));
    std::vector<std::string> want{preferred[0].host, preferred[1].host};
    auto rest = SortedHostsExcept({preferred[0].host, preferred[1].host});
    want.insert(want.end(), rest.begin(), rest.end());
    EXPECT_EQ(hosts, want);
}

TEST(KeyRouteAffinity, ReadBeforeWriteMatchesGoHeuristics) {
    WriteOperationOptions no_options;
    EXPECT_FALSE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                           WriteOperationKind::PutItem,
                                           no_options));
    EXPECT_FALSE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                           WriteOperationKind::DeleteItem,
                                           no_options));
    EXPECT_FALSE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                           WriteOperationKind::UpdateItem,
                                           no_options));

    WriteOperationOptions conditional;
    conditional.has_condition_expression = true;
    EXPECT_TRUE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                          WriteOperationKind::PutItem,
                                          conditional));
    EXPECT_TRUE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                          WriteOperationKind::DeleteItem,
                                          conditional));
    EXPECT_TRUE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                          WriteOperationKind::UpdateItem,
                                          conditional));

    WriteOperationOptions add_update;
    add_update.attribute_updates.push_back({AttributeAction::Add, true});
    EXPECT_TRUE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                          WriteOperationKind::UpdateItem,
                                          add_update));

    WriteOperationOptions delete_without_value;
    delete_without_value.attribute_updates.push_back({AttributeAction::Delete, false});
    EXPECT_FALSE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                           WriteOperationKind::UpdateItem,
                                           delete_without_value));

    WriteOperationOptions delete_with_value;
    delete_with_value.attribute_updates.push_back({AttributeAction::Delete, true});
    EXPECT_TRUE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::ReadBeforeWrite,
                                          WriteOperationKind::UpdateItem,
                                          delete_with_value));

    EXPECT_TRUE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::AnyWrite,
                                          WriteOperationKind::PutItem,
                                          no_options));
    EXPECT_FALSE(ShouldUseKeyRouteAffinity(KeyRouteAffinityMode::None,
                                           WriteOperationKind::PutItem,
                                           conditional));
}

TEST(KeyRouteAffinity, PartitionKeyPlanUsesSortedSeed) {
    StaticNodes nodes(BatchWriteTestNodes());
    auto metadata = Metadata({{"orders", "id"}});
    auto key = StringKeysForNode(BatchWriteSortedTestNodes()[0], 1)[0];

    auto plan = QueryPlanForPartitionKey(nodes, KeyWithId(key), "orders", metadata);
    EXPECT_EQ(plan.Next(), BatchWriteSortedTestNodes()[0]);
}

TEST(KeyRouteAffinity, PartitionKeyPlanKeepsActiveAffinityWhenQuarantineIsSampled) {
    const auto active_nodes = BatchWriteTestNodes();
    auto query_plan_nodes = active_nodes;
    const Url quarantined("http", "a-quarantine.example.com", 8000);
    query_plan_nodes.push_back(quarantined);

    StaticNodes nodes(active_nodes, {}, query_plan_nodes);
    auto metadata = Metadata({{"orders", "id"}});
    auto key = StringKeysForNode(BatchWriteSortedTestNodes()[0], 1)[0];
    const auto seed = HashPartitionKey(KeyWithId(key), "orders", metadata);

    std::vector<std::string> want{quarantined.host};
    auto active_order = PlanHosts(QueryPlan::WithSortedSeed(active_nodes, seed));
    want.insert(want.end(), active_order.begin(), active_order.end());

    auto hosts = PlanHosts(QueryPlanForPartitionKey(nodes, KeyWithId(key), "orders", metadata));
    EXPECT_EQ(hosts, want);
}

TEST(KeyRouteAffinity, BatchWriteVotingSelectsPreferredNode) {
    const auto sorted_nodes = BatchWriteSortedTestNodes();
    const auto target = sorted_nodes[0];
    const auto other = sorted_nodes[1];
    const auto target_keys = StringKeysForNode(target, 2);
    const auto other_key = StringKeysForNode(other, 1)[0];

    StaticNodes nodes(BatchWriteTestNodes());
    auto metadata = Metadata({{"audit", "id"}, {"orders", "id"}});
    const std::vector<BatchWriteOperation> operations{
        BatchWriteOperation::Put("orders", ItemWithId(target_keys[0], "orders-payload")),
        BatchWriteOperation::Delete("orders", KeyWithId(other_key)),
        BatchWriteOperation::Put("audit", ItemWithId(target_keys[1], "audit-payload")),
    };

    auto hosts = PlanHosts(QueryPlanForBatchWrite(nodes, operations, metadata));
    std::vector<std::string> want{target.host};
    auto rest = SortedHostsExcept({target.host});
    want.insert(want.end(), rest.begin(), rest.end());
    EXPECT_EQ(hosts, want);
}

TEST(KeyRouteAffinity, BatchWriteVotingKeepsQuarantineOutOfAffinityDomain) {
    const auto active_nodes = BatchWriteTestNodes();
    auto query_plan_nodes = active_nodes;
    const Url quarantined("http", "a-quarantine.example.com", 8000);
    query_plan_nodes.push_back(quarantined);

    const auto target = BatchWriteSortedTestNodes()[0];
    const auto target_key = StringKeysForNode(target, 1)[0];

    StaticNodes nodes(active_nodes, {}, query_plan_nodes);
    auto metadata = Metadata({{"orders", "id"}});
    const std::vector<BatchWriteOperation> operations{
        BatchWriteOperation::Put("orders", ItemWithId(target_key, "orders-payload")),
    };

    auto preferred = SelectBatchWritePreferredNodes(nodes, operations, metadata);
    ASSERT_EQ(preferred.size(), 1U);
    EXPECT_EQ(preferred[0], target);

    auto hosts = PlanHosts(QueryPlanForBatchWrite(nodes, operations, metadata));
    std::vector<std::string> want{quarantined.host, target.host};
    auto rest = SortedHostsExcept({target.host});
    want.insert(want.end(), rest.begin(), rest.end());
    EXPECT_EQ(hosts, want);
}

TEST(KeyRouteAffinity, BatchWriteVotingOrdersAllVotedNodesBeforeUnvoted) {
    const auto sorted_nodes = BatchWriteSortedTestNodes();
    const auto target = sorted_nodes[3];
    const auto other = sorted_nodes[2];
    const auto target_keys = StringKeysForNode(target, 2);
    const auto other_key = StringKeysForNode(other, 1)[0];

    StaticNodes nodes(BatchWriteTestNodes());
    auto metadata = Metadata({{"orders", "id"}});
    const std::vector<BatchWriteOperation> operations{
        BatchWriteOperation::Put("orders", ItemWithId(target_keys[0], "target-a")),
        BatchWriteOperation::Delete("orders", KeyWithId(other_key)),
        BatchWriteOperation::Put("orders", ItemWithId(target_keys[1], "target-b")),
    };

    auto hosts = PlanHosts(QueryPlanForBatchWrite(nodes, operations, metadata));
    std::vector<std::string> want{target.host, other.host};
    auto rest = SortedHostsExcept({target.host, other.host});
    want.insert(want.end(), rest.begin(), rest.end());
    EXPECT_EQ(hosts, want);
}

TEST(KeyRouteAffinity, BatchWriteVotingUsesDeterministicTieBreak) {
    const auto sorted_nodes = BatchWriteSortedTestNodes();
    const auto left = sorted_nodes[3];
    const auto right = sorted_nodes[2];
    const auto left_key = StringKeysForNode(left, 1)[0];
    const auto right_key = StringKeysForNode(right, 1)[0];

    StaticNodes nodes(BatchWriteTestNodes());
    auto metadata = Metadata({{"orders", "id"}});
    const std::vector<BatchWriteOperation> operations{
        BatchWriteOperation::Put("orders", ItemWithId(left_key, "left")),
        BatchWriteOperation::Delete("orders", KeyWithId(right_key)),
    };

    auto hosts = PlanHosts(QueryPlanForBatchWrite(nodes, operations, metadata));
    std::vector<std::string> want{right.host, left.host};
    auto rest = SortedHostsExcept({right.host, left.host});
    want.insert(want.end(), rest.begin(), rest.end());
    EXPECT_EQ(hosts, want);
}

TEST(KeyRouteAffinity, BatchWriteVotingSkipsUnusableCandidates) {
    const auto target = BatchWriteSortedTestNodes()[0];
    const auto target_key = StringKeysForNode(target, 1)[0];

    StaticNodes nodes(BatchWriteTestNodes());
    auto metadata = Metadata({{"orders", "id"}});
    const std::vector<BatchWriteOperation> operations{
        BatchWriteOperation::Put("unknown", ItemWithId("missing-metadata", "ignored")),
        BatchWriteOperation::Put("orders", {{"not_id", AttributeValue::String("missing-key")}}),
        BatchWriteOperation::Delete("orders", KeyWithId(target_key)),
    };

    auto plan = QueryPlanForBatchWrite(nodes, operations, metadata);
    EXPECT_EQ(plan.Next(), target);
}

TEST(KeyRouteAffinity, BatchWriteSupportsPartitionKeyTypes) {
    StaticNodes nodes(BatchWriteTestNodes());
    auto metadata = Metadata({{"orders", "id"}});

    const std::vector<AttributeValue> values{
        AttributeValue::String("string-key"),
        AttributeValue::Number("42"),
        AttributeValue::Binary({0x00, 0xff}),
    };

    for (const auto& value : values) {
        const std::vector<BatchWriteOperation> operations{
            BatchWriteOperation::Put("orders", {{"id", value}}),
        };
        auto plan = QueryPlanForBatchWrite(nodes, operations, metadata);
        EXPECT_EQ(plan.Next(), NodeForValue(value));
    }
}
