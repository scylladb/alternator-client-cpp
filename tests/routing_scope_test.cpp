#include <scylladb/alternator/routing_scope.h>

#include <gtest/gtest.h>

using namespace scylladb::alternator;

TEST(RoutingScope, FormatsQueriesAndFallbacks) {
    auto cluster = NewClusterScope();
    auto dc = NewDCScope("dc1", cluster);
    auto rack = NewRackScope("dc1", "rack1", dc);

    EXPECT_EQ(cluster->Name(), "Cluster");
    EXPECT_EQ(cluster->ToString(), "Cluster()");
    EXPECT_EQ(cluster->LocalNodesQuery(), "");
    EXPECT_TRUE(cluster->IsCluster());

    EXPECT_EQ(dc->Name(), "Datacenter");
    EXPECT_EQ(dc->ToString(), "Datacenter(dc=dc1)");
    EXPECT_EQ(dc->LocalNodesQuery(), "dc=dc1");
    EXPECT_EQ(dc->Fallback(), cluster);

    EXPECT_EQ(rack->Name(), "Rack");
    EXPECT_EQ(rack->ToString(), "Rack(dc=dc1, rack=rack1)");
    EXPECT_EQ(rack->LocalNodesQuery(), "dc=dc1&rack=rack1");
    EXPECT_EQ(rack->Fallback(), dc);
}
