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

#include "testinfra/test_cluster_internal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace scylladb::alternator::testinfra;

namespace {

class FakeProvisioner final : public ClusterProvisioner {
public:
    std::filesystem::path RunDirectory() const override {
        return "/tmp/alternator-cpp-fake-run";
    }

    ProvisionedClusterData Provision(
        const ClusterSpec& spec,
        const std::string&,
        int ccm_id) override {
        ++provision_count;
        if (fail_provision) {
            throw ClusterProvisioningError(
                "provision failed",
                provision_cleanup_proven,
                provision_recovery_required);
        }
        ProvisionedClusterData data;
        data.ccm_id = ccm_id;
        data.ccm_directory = RunDirectory() / "cluster";
        int index = 0;
        for (std::size_t dc = 0; dc < spec.Topology().Datacenters().size(); ++dc) {
            const auto& datacenter = spec.Topology().Datacenters()[dc];
            for (std::size_t rack = 0; rack < datacenter.Racks().size(); ++rack) {
                for (int count = 0; count < datacenter.Racks()[rack].NodeCount(); ++count) {
                    ++index;
                    data.nodes.push_back(TestClusterNode{
                        "node" + std::to_string(index),
                        "127.0." + std::to_string(ccm_id) + "." + std::to_string(index),
                        "dc" + std::to_string(dc + 1),
                        "RAC" + std::to_string(rack + 1),
                    });
                }
            }
        }
        return data;
    }

    void Start(const PhysicalTestCluster&) override {
        ++start_count;
        if (fail_start_recovery) {
            throw RecoveryRequiredError("start process group remains alive");
        }
    }

    void Stop(const PhysicalTestCluster&) override {
        ++stop_count;
        {
            std::unique_lock<std::mutex> lock(stop_mutex);
            if (block_stop) {
                stop_entered = true;
                stop_condition.notify_all();
                stop_condition.wait(lock, [&] { return release_stop; });
            }
        }
        if (fail_stop) {
            throw std::runtime_error("ambiguous stop");
        }
    }

    void StartNode(const PhysicalTestCluster&, const TestClusterNode&) override {
        ++start_node_count;
    }

    void StopNode(const PhysicalTestCluster&, const TestClusterNode&) override {
        ++stop_node_count;
    }

    TestClusterNode AddNode(
        const PhysicalTestCluster& cluster,
        const std::string& datacenter,
        const std::string& rack) override {
        ++add_node_count;
        if (fail_add) {
            throw NodeProvisioningError(
                "add failed",
                add_cleanup_proven,
                add_recovery_required);
        }
        int index = 1;
        const auto nodes = cluster.Nodes();
        while (std::any_of(nodes.begin(), nodes.end(), [&](const TestClusterNode& node) {
            return node.name == "node" + std::to_string(index);
        })) {
            ++index;
        }
        return TestClusterNode{
            "node" + std::to_string(index),
            "127.0." + std::to_string(cluster.CcmId()) + "." + std::to_string(index),
            datacenter,
            rack,
        };
    }

    void DecommissionNode(const PhysicalTestCluster&, const TestClusterNode&) override {
        ++decommission_count;
    }

    void DeleteNodeState(const PhysicalTestCluster&, const TestClusterNode&) override {
        ++delete_node_count;
    }

    bool IsHealthy(const PhysicalTestCluster&) override {
        ++health_count;
        return healthy;
    }

    bool IsNodeRunning(const PhysicalTestCluster&, const TestClusterNode&) override {
        return true;
    }

    void Remove(const PhysicalTestCluster&) override {
        ++remove_count;
        if (fail_remove) {
            throw std::runtime_error("remove failed");
        }
    }

    void BlockStop() {
        std::lock_guard<std::mutex> lock(stop_mutex);
        block_stop = true;
        stop_entered = false;
        release_stop = false;
    }

    bool WaitForBlockedStop() {
        std::unique_lock<std::mutex> lock(stop_mutex);
        return stop_condition.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return stop_entered; });
    }

    void ReleaseBlockedStop() {
        std::lock_guard<std::mutex> lock(stop_mutex);
        release_stop = true;
        stop_condition.notify_all();
    }

    int provision_count = 0;
    int health_count = 0;
    int remove_count = 0;
    int start_count = 0;
    int stop_count = 0;
    int start_node_count = 0;
    int stop_node_count = 0;
    int add_node_count = 0;
    int decommission_count = 0;
    int delete_node_count = 0;
    bool healthy = true;
    bool fail_stop = false;
    bool fail_start_recovery = false;
    bool fail_add = false;
    bool add_cleanup_proven = false;
    bool add_recovery_required = false;
    bool fail_provision = false;
    bool provision_cleanup_proven = false;
    bool provision_recovery_required = false;
    bool fail_remove = false;
    std::mutex stop_mutex;
    std::condition_variable stop_condition;
    bool block_stop = false;
    bool stop_entered = false;
    bool release_stop = false;
};

struct PoolFixture {
    PoolFixture()
        : provisioner(std::make_shared<FakeProvisioner>())
        , pool(std::make_shared<TestClusterPool>(
              provisioner,
              ClusterSpec::kMaximumNodeCount,
              [&](TestResourceScope& scope) { cleaned_prefixes.push_back(scope.Prefix()); })) {}

    std::shared_ptr<FakeProvisioner> provisioner;
    std::vector<std::string> cleaned_prefixes;
    std::shared_ptr<TestClusterPool> pool;
};

} // namespace

TEST(TestClusterPoolTest, MatchingReusableLeasesShareClusterAndOwnResourceScopes) {
    PoolFixture fixture;
    auto first = fixture.pool->AcquireReusable(ClusterSpec{});
    auto second = fixture.pool->AcquireReusable(ClusterSpec{});

    EXPECT_EQ(fixture.provisioner->provision_count, 1);
    EXPECT_EQ(first.Cluster().InstanceId(), second.Cluster().InstanceId());
    EXPECT_NE(first.Resources().Prefix(), second.Resources().Prefix());
    const auto table = first.Resources().NewTableName("Mixed punctuation !/\xE2\x98\x83");
    EXPECT_LE(table.size(), 192U);
    EXPECT_TRUE(std::all_of(table.begin(), table.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '_' ||
               character == '-' || character == '.';
    }));
    EXPECT_EQ(first.Resources().NewTableName(std::string(512, 'A')).size(), 192U);

    first.Close();
    EXPECT_EQ(fixture.cleaned_prefixes.size(), 1U);
    second.Close();
    EXPECT_EQ(fixture.cleaned_prefixes.size(), 2U);
}

TEST(TestClusterPoolTest, ActiveIncompatibleAndPrivateRequestsFailImmediately) {
    PoolFixture fixture;
    auto lease = fixture.pool->AcquireReusable(ClusterSpec{});
    const auto incompatible =
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1));

    EXPECT_THROW((void)fixture.pool->AcquireReusable(incompatible), std::logic_error);
    EXPECT_THROW((void)fixture.pool->ProvisionPrivate(ClusterSpec{}), std::logic_error);
    lease.Close();
}

TEST(TestClusterPoolTest, IdleIncompatibleOrUnhealthyClusterIsReplaced) {
    PoolFixture fixture;
    {
        auto first = fixture.pool->AcquireReusable(ClusterSpec{});
        first.Close();
    }
    {
        auto second = fixture.pool->AcquireReusable(
            ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
        EXPECT_EQ(fixture.provisioner->remove_count, 1);
        EXPECT_EQ(fixture.provisioner->provision_count, 2);
        second.Close();
    }
    fixture.provisioner->healthy = false;
    auto third = fixture.pool->AcquireReusable(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    EXPECT_EQ(fixture.provisioner->health_count, 1);
    EXPECT_EQ(fixture.provisioner->remove_count, 2);
    EXPECT_EQ(fixture.provisioner->provision_count, 3);
    third.Close();
}

TEST(TestClusterPoolTest, PrivateLeaseExposesSerializedLifecycleAndTopologyControls) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    const auto original = lease.Cluster().Nodes().front();
    lease.Control().Stop();
    lease.Control().Start();
    const auto added = lease.Control().AddNode("dc1", "RAC2");
    EXPECT_EQ(lease.Cluster().Nodes().size(), 2U);
    lease.Control().StopNode(added);
    lease.Control().StartNode(added);
    lease.Control().RemoveNode(original);
    ASSERT_EQ(lease.Cluster().Nodes().size(), 1U);
    EXPECT_EQ(lease.Cluster().Nodes().front(), added);

    EXPECT_EQ(fixture.provisioner->stop_count, 1);
    EXPECT_EQ(fixture.provisioner->start_count, 1);
    EXPECT_EQ(fixture.provisioner->add_node_count, 1);
    EXPECT_EQ(fixture.provisioner->stop_node_count, 1);
    EXPECT_EQ(fixture.provisioner->start_node_count, 1);
    EXPECT_EQ(fixture.provisioner->decommission_count, 1);
    EXPECT_EQ(fixture.provisioner->delete_node_count, 1);
    lease.Close();
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, RemoveNodeRejectsMissingVoterQuorumBeforeMutation) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(2)));
    const auto original = lease.Cluster().Nodes().front();

    lease.Control().Stop();
    EXPECT_THROW(lease.Control().RemoveNode(original), std::logic_error);
    EXPECT_EQ(fixture.provisioner->start_node_count, 0);
    EXPECT_EQ(fixture.provisioner->decommission_count, 0);
    EXPECT_EQ(fixture.provisioner->delete_node_count, 0);

    EXPECT_NO_THROW(lease.Control().Start());
    EXPECT_NO_THROW(lease.Control().RemoveNode(original));
    EXPECT_EQ(fixture.provisioner->decommission_count, 1);
    EXPECT_EQ(fixture.provisioner->delete_node_count, 1);
    lease.Close();
}

TEST(TestClusterPoolTest, RemoveNodeRejectsMissingProjectedVoterQuorumBeforeMutation) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(3)));
    const auto nodes = lease.Cluster().Nodes();
    ASSERT_EQ(nodes.size(), 3U);

    lease.Control().StopNode(nodes[2]);
    EXPECT_THROW(lease.Control().RemoveNode(nodes[0]), std::logic_error);
    EXPECT_EQ(fixture.provisioner->start_node_count, 0);
    EXPECT_EQ(fixture.provisioner->decommission_count, 0);
    EXPECT_EQ(fixture.provisioner->delete_node_count, 0);

    EXPECT_NO_THROW(lease.Control().StartNode(nodes[2]));
    EXPECT_NO_THROW(lease.Control().RemoveNode(nodes[0]));
    EXPECT_EQ(fixture.provisioner->decommission_count, 1);
    EXPECT_EQ(fixture.provisioner->delete_node_count, 1);
    lease.Close();
}

TEST(TestClusterPoolTest, AmbiguousMutationDirtiesClusterButStillAllowsWholeRemoval) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    fixture.provisioner->fail_stop = true;
    EXPECT_THROW(lease.Control().Stop(), std::runtime_error);
    EXPECT_THROW(lease.Control().Start(), std::logic_error);
    EXPECT_NO_THROW(lease.Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, ProvenAddRollbackLeavesPrivateClusterUsable) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    fixture.provisioner->fail_add = true;
    fixture.provisioner->add_cleanup_proven = true;
    EXPECT_THROW(lease.Control().AddNode("dc1", "RAC1"), NodeProvisioningError);
    EXPECT_NO_THROW(lease.Control().Stop());
    fixture.provisioner->fail_add = false;
    EXPECT_NO_THROW(lease.Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, UnprovenAddRollbackDirtiesButAllowsWholeClusterCleanup) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    fixture.provisioner->fail_add = true;
    EXPECT_THROW(lease.Control().AddNode("dc1", "RAC1"), NodeProvisioningError);
    EXPECT_THROW(lease.Control().Stop(), std::logic_error);
    fixture.provisioner->fail_add = false;
    EXPECT_NO_THROW(lease.Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, FailedProvisioningWithUnprovenRollbackIsRetriedOnPoolClose) {
    PoolFixture fixture;
    fixture.provisioner->fail_provision = true;
    EXPECT_THROW(fixture.pool->AcquireReusable(ClusterSpec{}), ClusterProvisioningError);
    fixture.provisioner->fail_provision = false;
    EXPECT_NO_THROW(fixture.pool->Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, FailedPrivateCloseCanBeRetried) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    const auto& cluster = lease.Cluster();
    auto& resources = lease.Resources();
    fixture.provisioner->fail_remove = true;
    EXPECT_THROW(lease.Close(), std::runtime_error);
    EXPECT_TRUE(static_cast<bool>(lease));
    EXPECT_NO_THROW((void)cluster.InstanceId());
    EXPECT_NO_THROW((void)resources.Prefix());
    fixture.provisioner->fail_remove = false;
    EXPECT_NO_THROW(lease.Close());
    EXPECT_FALSE(static_cast<bool>(lease));
    EXPECT_EQ(fixture.provisioner->remove_count, 2);
}

TEST(TestClusterPoolTest, PoolCloseInvalidatesAllOutstandingReusableLeases) {
    PoolFixture fixture;
    auto first = fixture.pool->AcquireReusable(ClusterSpec{});
    auto second = fixture.pool->AcquireReusable(ClusterSpec{});
    const auto& first_cluster = first.Cluster();
    auto& first_resources = first.Resources();
    const auto& second_cluster = second.Cluster();
    auto& second_resources = second.Resources();

    EXPECT_NO_THROW(fixture.pool->Close());

    EXPECT_FALSE(static_cast<bool>(first));
    EXPECT_FALSE(static_cast<bool>(second));
    EXPECT_THROW((void)first.Cluster(), std::logic_error);
    EXPECT_THROW((void)first.Resources(), std::logic_error);
    EXPECT_THROW((void)second.Cluster(), std::logic_error);
    EXPECT_THROW((void)second.Resources(), std::logic_error);
    EXPECT_THROW((void)first_cluster.InstanceId(), std::logic_error);
    EXPECT_THROW((void)first_resources.Prefix(), std::logic_error);
    EXPECT_THROW((void)second_cluster.Nodes(), std::logic_error);
    EXPECT_THROW((void)second_resources.NewTableName("closed"), std::logic_error);
    EXPECT_EQ(fixture.provisioner->remove_count, 1);

    EXPECT_NO_THROW(first.Close());
    EXPECT_NO_THROW(second.Close());
    EXPECT_NO_THROW(fixture.pool->Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, PoolCloseInvalidatesOutstandingPrivateLeaseAndControl) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    const auto& cluster = lease.Cluster();
    auto& control = lease.Control();
    auto& resources = lease.Resources();

    EXPECT_NO_THROW(fixture.pool->Close());

    EXPECT_FALSE(static_cast<bool>(lease));
    EXPECT_THROW((void)lease.Cluster(), std::logic_error);
    EXPECT_THROW((void)lease.Control(), std::logic_error);
    EXPECT_THROW((void)lease.Resources(), std::logic_error);
    EXPECT_THROW((void)cluster.Connection(AlternatorTransport::Http), std::logic_error);
    EXPECT_THROW(control.Start(), std::logic_error);
    EXPECT_THROW((void)resources.Prefix(), std::logic_error);
    EXPECT_EQ(fixture.provisioner->remove_count, 1);

    EXPECT_NO_THROW(lease.Close());
    EXPECT_NO_THROW(fixture.pool->Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, ClosingReusableLeaseInvalidatesOnlyItsSavedViews) {
    PoolFixture fixture;
    auto first = fixture.pool->AcquireReusable(ClusterSpec{});
    auto second = fixture.pool->AcquireReusable(ClusterSpec{});
    const auto& first_cluster = first.Cluster();
    auto& first_resources = first.Resources();
    const auto& second_cluster = second.Cluster();
    auto& second_resources = second.Resources();

    EXPECT_NO_THROW(first.Close());

    EXPECT_THROW((void)first_cluster.InstanceId(), std::logic_error);
    EXPECT_THROW((void)first_resources.NewTableName("closed"), std::logic_error);
    EXPECT_NO_THROW((void)second_cluster.InstanceId());
    EXPECT_NO_THROW((void)second_resources.Prefix());

    second.Close();
    fixture.pool->Close();
}

TEST(TestClusterPoolTest, PoolCloseWaitsForControlOperationThenRejectsSavedControl) {
    PoolFixture fixture;
    auto lease = fixture.pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1)));
    auto& control = lease.Control();
    fixture.provisioner->BlockStop();

    std::exception_ptr stop_failure;
    std::exception_ptr close_failure;
    std::atomic<bool> close_finished{false};
    std::thread stop([&] {
        try {
            control.Stop();
        } catch (...) {
            stop_failure = std::current_exception();
        }
    });
    const bool stop_entered = fixture.provisioner->WaitForBlockedStop();
    std::thread close([&] {
        try {
            fixture.pool->Close();
        } catch (...) {
            close_failure = std::current_exception();
        }
        close_finished.store(true);
    });

    bool invalidated = false;
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (!static_cast<bool>(lease)) {
            invalidated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool saved_control_rejected = false;
    if (invalidated) {
        try {
            control.Start();
        } catch (const std::logic_error&) {
            saved_control_rejected = true;
        }
    }
    const bool close_waited = !close_finished.load();

    fixture.provisioner->ReleaseBlockedStop();
    stop.join();
    close.join();

    EXPECT_TRUE(stop_entered);
    EXPECT_TRUE(invalidated);
    EXPECT_TRUE(saved_control_rejected);
    EXPECT_TRUE(close_waited);
    EXPECT_EQ(stop_failure, nullptr);
    EXPECT_EQ(close_failure, nullptr);
    EXPECT_EQ(fixture.provisioner->stop_count, 1);
    EXPECT_EQ(fixture.provisioner->start_count, 0);
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}

TEST(TestClusterPoolTest, ProcessCleanupFailureRequiresNextProcessRecovery) {
    auto provisioner = std::make_shared<FakeProvisioner>();
    auto spec = ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1));
    auto data = provisioner->Provision(spec, "recovery-cluster", 1);
    auto cluster = std::make_shared<PhysicalTestCluster>(
        provisioner, "recovery-cluster", spec, std::move(data));
    provisioner->fail_start_recovery = true;

    EXPECT_THROW(cluster->Start(), RecoveryRequiredError);
    EXPECT_TRUE(cluster->IsDirty());
    EXPECT_THROW(cluster->RemovePhysical(), std::logic_error);
    EXPECT_EQ(provisioner->remove_count, 0);
}

TEST(TestClusterPoolTest, CleanupOutcomeTypesRejectContradictoryState) {
    EXPECT_THROW(
        (void)ClusterProvisioningError("invalid", true, true),
        std::invalid_argument);
    EXPECT_THROW(
        (void)NodeProvisioningError("invalid", true, true),
        std::invalid_argument);
}

TEST(TestClusterPoolTest, ConcurrentReusableCloseReleasesExactlyOnce) {
    auto provisioner = std::make_shared<FakeProvisioner>();
    std::atomic<int> cleanup_count{0};
    auto pool = std::make_shared<TestClusterPool>(
        provisioner,
        ClusterSpec::kMaximumNodeCount,
        [&](TestResourceScope&) {
            ++cleanup_count;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        });
    auto lease = pool->AcquireReusable(ClusterSpec{});
    std::thread first([&] { lease.Close(); });
    std::thread second([&] { lease.Close(); });
    first.join();
    second.join();
    EXPECT_EQ(cleanup_count.load(), 1);
    pool->Close();
}

TEST(TestClusterPoolTest, ResourceCleanupFailurePoisonsUntilLastLeaseRetiresCluster) {
    auto provisioner = std::make_shared<FakeProvisioner>();
    int cleanup_count = 0;
    auto pool = std::make_shared<TestClusterPool>(
        provisioner,
        ClusterSpec::kMaximumNodeCount,
        [&](TestResourceScope&) {
            if (++cleanup_count == 1) {
                throw std::runtime_error("resource cleanup failed");
            }
        });
    auto first = pool->AcquireReusable(ClusterSpec{});
    auto second = pool->AcquireReusable(ClusterSpec{});

    EXPECT_THROW(first.Close(), std::runtime_error);
    EXPECT_THROW(pool->AcquireReusable(ClusterSpec{}), std::runtime_error);
    EXPECT_NO_THROW(second.Close());
    EXPECT_EQ(provisioner->remove_count, 1);

    auto replacement = pool->AcquireReusable(ClusterSpec{});
    EXPECT_EQ(provisioner->provision_count, 2);
    replacement.Close();
    pool->Close();
}

TEST(TestClusterPoolTest, NodeControlsRejectForgedAndStaleHandles) {
    auto provisioner = std::make_shared<FakeProvisioner>();
    const auto spec = ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(1));

    ProvisionedClusterData first_data = provisioner->Provision(spec, "first-cluster", 42);
    auto first = std::make_shared<PhysicalTestCluster>(
        provisioner, "first-cluster", spec, std::move(first_data));
    const auto stale = first->Nodes().front();
    const TestClusterNode forged{
        stale.name,
        stale.address,
        stale.datacenter,
        stale.rack,
    };
    EXPECT_THROW(first->StopNode(forged), std::invalid_argument);

    ProvisionedClusterData second_data = provisioner->Provision(spec, "second-cluster", 42);
    auto second = std::make_shared<PhysicalTestCluster>(
        provisioner, "second-cluster", spec, std::move(second_data));
    ASSERT_EQ(second->Nodes().front().name, stale.name);
    ASSERT_EQ(second->Nodes().front().address, stale.address);
    EXPECT_THROW(second->StopNode(stale), std::invalid_argument);
    EXPECT_NO_THROW(second->StopNode(second->Nodes().front()));
}

TEST(TestClusterPoolTest, ConfiguredNodeCeilingAppliesToProvisionAndAddition) {
    auto provisioner = std::make_shared<FakeProvisioner>();
    auto pool = std::make_shared<TestClusterPool>(
        provisioner, 2, [](TestResourceScope&) {});
    EXPECT_THROW((void)pool->AcquireReusable(ClusterSpec{}), std::logic_error);

    auto lease = pool->ProvisionPrivate(
        ClusterSpec{}.WithTopology(ClusterTopology::SingleDatacenter(2)));
    EXPECT_THROW((void)lease.Control().AddNode("dc1", "RAC1"), std::logic_error);
    lease.Close();
}

TEST(TestClusterPoolTest, LeaseAndPoolCloseAreIdempotent) {
    PoolFixture fixture;
    auto lease = fixture.pool->AcquireReusable(ClusterSpec{});
    lease.Close();
    EXPECT_NO_THROW(lease.Close());
    EXPECT_NO_THROW(fixture.pool->Close());
    EXPECT_NO_THROW(fixture.pool->Close());
    EXPECT_EQ(fixture.provisioner->remove_count, 1);
}
