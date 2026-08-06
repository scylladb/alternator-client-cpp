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
