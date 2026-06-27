#include <scylladb/alternator/query_plan.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace scylladb::alternator;

TEST(QueryPlan, ReturnsNodesWithoutDuplicates) {
    std::vector<Url> nodes{
        {"http", "a.local", 8080},
        {"http", "b.local", 8080},
        {"http", "extra.local", 8080},
    };
    QueryPlan plan(nodes);

    std::set<std::string> seen;
    for (int i = 0; i < 3; ++i) {
        auto node = plan.Next();
        ASSERT_FALSE(node.Empty());
        EXPECT_TRUE(seen.insert(node.host).second);
    }
    EXPECT_TRUE(plan.Next().Empty());
}

TEST(QueryPlan, PreferredNodeComesFirstThenSortedRemaining) {
    Url preferred("http", "b.local", 8080);
    QueryPlan plan = QueryPlan::WithPreferredNode(
        {{"http", "c.local", 8080},
         preferred,
         {"http", "a.local", 8080},
         {"http", "e.local", 8080},
         {"http", "d.local", 8080}},
        preferred);

    EXPECT_EQ(plan.Next().host, "b.local");
    EXPECT_EQ(plan.Next().host, "a.local");
    EXPECT_EQ(plan.Next().host, "c.local");
    EXPECT_EQ(plan.Next().host, "d.local");
    EXPECT_EQ(plan.Next().host, "e.local");
    EXPECT_TRUE(plan.Next().Empty());
}

TEST(QueryPlan, FirstNodeWithSeedUsesSortedNodes) {
    std::vector<Url> nodes{
        {"http", "node2.example.com", 8043},
        {"http", "node10.example.com", 8043},
        {"http", "node1.example.com", 8043},
    };

    auto first = FirstNodeWithSeed(nodes, 42);
    auto sorted = SortAndDedupeNodes(nodes);
    EXPECT_NE(std::find(sorted.begin(), sorted.end(), first), sorted.end());
    EXPECT_EQ(nodes[0].host, "node2.example.com");
}

static std::vector<Url> MakeTestNodes(const std::string& prefix, int count) {
    std::vector<Url> nodes;
    nodes.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        nodes.emplace_back("", prefix + std::to_string(i + 1) + ".example.com", 8043);
    }
    return nodes;
}

TEST(QueryPlan, SeededPlanMatchesGoCrossLanguageVectors) {
    struct Case {
        const char* name;
        std::int64_t seed;
        int node_count;
        std::vector<std::string> want_first6;
    };

    const std::vector<Case> cases{
        {"seed=42, 10 nodes", 42, 10, {
            "node6.example.com:8043",
            "node9.example.com:8043",
            "node5.example.com:8043",
            "node2.example.com:8043",
            "node7.example.com:8043",
            "node1.example.com:8043",
        }},
        {"seed=123, 10 nodes", 123, 10, {
            "node6.example.com:8043",
            "node1.example.com:8043",
            "node4.example.com:8043",
            "node3.example.com:8043",
            "node10.example.com:8043",
            "node5.example.com:8043",
        }},
        {"seed=999, 10 nodes", 999, 10, {
            "node5.example.com:8043",
            "node10.example.com:8043",
            "node4.example.com:8043",
            "node1.example.com:8043",
            "node2.example.com:8043",
            "node3.example.com:8043",
        }},
        {"seed=0, 10 nodes", 0, 10, {
            "node5.example.com:8043",
            "node1.example.com:8043",
            "node2.example.com:8043",
            "node10.example.com:8043",
            "node6.example.com:8043",
            "node8.example.com:8043",
        }},
        {"seed=-1, 10 nodes", -1, 10, {
            "node2.example.com:8043",
            "node5.example.com:8043",
            "node1.example.com:8043",
            "node3.example.com:8043",
            "node6.example.com:8043",
            "node10.example.com:8043",
        }},
        {"seed=42, 6 nodes", 42, 6, {
            "node6.example.com:8043",
            "node3.example.com:8043",
            "node1.example.com:8043",
            "node4.example.com:8043",
            "node2.example.com:8043",
            "node5.example.com:8043",
        }},
        {"seed=12345, 10 nodes", 12345, 10, {
            "node4.example.com:8043",
            "node5.example.com:8043",
            "node1.example.com:8043",
            "node7.example.com:8043",
            "node6.example.com:8043",
            "node8.example.com:8043",
        }},
        {"seed=MaxInt64, 10 nodes", 9223372036854775807LL, 10, {
            "node2.example.com:8043",
            "node7.example.com:8043",
            "node8.example.com:8043",
            "node1.example.com:8043",
            "node10.example.com:8043",
            "node4.example.com:8043",
        }},
    };

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        auto nodes = MakeTestNodes("node", tc.node_count);
        QueryPlan plan = QueryPlan::WithSeed(std::move(nodes), tc.seed);
        for (const auto& want : tc.want_first6) {
            EXPECT_EQ(plan.Next().Authority(), want);
        }
    }
}
