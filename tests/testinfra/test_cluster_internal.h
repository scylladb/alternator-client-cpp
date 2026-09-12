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

#pragma once

#include "test_cluster.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scylladb::alternator::testinfra {

class PhysicalTestCluster;
class LeaseValidity;

struct ProvisionedClusterData {
    int ccm_id = 0;
    std::filesystem::path ccm_directory;
    std::vector<TestClusterNode> nodes;
    std::filesystem::path ca_certificate_path;
    std::optional<Credentials> credentials;
};

class ClusterProvisioningError : public std::runtime_error {
public:
    ClusterProvisioningError(
        std::string message,
        bool cleanup_proven,
        bool recovery_required = false)
        : std::runtime_error(std::move(message))
        , cleanup_proven_(cleanup_proven)
        , recovery_required_(recovery_required) {
        if (cleanup_proven_ && recovery_required_) {
            throw std::invalid_argument(
                "proven cluster cleanup cannot require next-process recovery");
        }
    }

    [[nodiscard]] bool CleanupProven() const noexcept {
        return cleanup_proven_;
    }

    [[nodiscard]] bool RecoveryRequired() const noexcept {
        return recovery_required_;
    }

private:
    bool cleanup_proven_;
    bool recovery_required_;
};

class NodeProvisioningError : public std::runtime_error {
public:
    NodeProvisioningError(
        std::string message,
        bool cleanup_proven,
        bool recovery_required = false)
        : std::runtime_error(std::move(message))
        , cleanup_proven_(cleanup_proven)
        , recovery_required_(recovery_required) {
        if (cleanup_proven_ && recovery_required_) {
            throw std::invalid_argument(
                "proven node cleanup cannot require next-process recovery");
        }
    }

    [[nodiscard]] bool CleanupProven() const noexcept {
        return cleanup_proven_;
    }

    [[nodiscard]] bool RecoveryRequired() const noexcept {
        return recovery_required_;
    }

private:
    bool cleanup_proven_;
    bool recovery_required_;
};

class RecoveryRequiredError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Deterministic fault injection for run-state tests.
void SetCcmRunStateMetadataPublishFsyncFailureCountdownForTest(std::uint32_t countdown);
void SetCcmRunStateMetadataRollbackFsyncFailureForTest(bool enabled);
void SetCcmRunStatePasswdLookupUnavailableForTest(bool unavailable);

class ClusterProvisioner {
public:
    virtual ~ClusterProvisioner() = default;

    [[nodiscard]] virtual std::filesystem::path RunDirectory() const = 0;
    [[nodiscard]] virtual bool RequiresJmxPortReservation(const ClusterSpec& spec) const;
    [[nodiscard]] virtual ProvisionedClusterData Provision(
        const ClusterSpec& spec,
        const std::string& instance_id,
        int ccm_id) = 0;
    virtual void Start(const PhysicalTestCluster& cluster) = 0;
    virtual void Stop(const PhysicalTestCluster& cluster) = 0;
    virtual void StartNode(const PhysicalTestCluster& cluster, const TestClusterNode& node) = 0;
    virtual void StopNode(const PhysicalTestCluster& cluster, const TestClusterNode& node) = 0;
    [[nodiscard]] virtual TestClusterNode AddNode(
        const PhysicalTestCluster& cluster,
        const std::string& datacenter,
        const std::string& rack) = 0;
    virtual void DecommissionNode(
        const PhysicalTestCluster& cluster,
        const TestClusterNode& node) = 0;
    virtual void DeleteNodeState(
        const PhysicalTestCluster& cluster,
        const TestClusterNode& node) = 0;
    [[nodiscard]] virtual bool IsHealthy(const PhysicalTestCluster& cluster) = 0;
    [[nodiscard]] virtual bool IsNodeRunning(
        const PhysicalTestCluster& cluster,
        const TestClusterNode& node) = 0;
    virtual void Remove(const PhysicalTestCluster& cluster) = 0;
};

class CcmProvisioner final : public ClusterProvisioner {
public:
    explicit CcmProvisioner(const std::filesystem::path& run_directory);
    CcmProvisioner(
        const std::filesystem::path& run_directory,
        std::filesystem::path diagnostics_directory,
        std::string ccm_executable,
        std::chrono::seconds command_timeout = std::chrono::minutes(10));

    [[nodiscard]] std::filesystem::path RunDirectory() const override;
    [[nodiscard]] bool RequiresJmxPortReservation(const ClusterSpec& spec) const override;
    [[nodiscard]] ProvisionedClusterData Provision(
        const ClusterSpec& spec,
        const std::string& instance_id,
        int ccm_id) override;
    void Start(const PhysicalTestCluster& cluster) override;
    void Stop(const PhysicalTestCluster& cluster) override;
    void StartNode(const PhysicalTestCluster& cluster, const TestClusterNode& node) override;
    void StopNode(const PhysicalTestCluster& cluster, const TestClusterNode& node) override;
    [[nodiscard]] TestClusterNode AddNode(
        const PhysicalTestCluster& cluster,
        const std::string& datacenter,
        const std::string& rack) override;
    void DecommissionNode(
        const PhysicalTestCluster& cluster,
        const TestClusterNode& node) override;
    void DeleteNodeState(
        const PhysicalTestCluster& cluster,
        const TestClusterNode& node) override;
    [[nodiscard]] bool IsHealthy(const PhysicalTestCluster& cluster) override;
    [[nodiscard]] bool IsNodeRunning(
        const PhysicalTestCluster& cluster,
        const TestClusterNode& node) override;
    void Remove(const PhysicalTestCluster& cluster) override;

    void CleanupStaleCluster(
        const std::string& instance_id,
        int ccm_id,
        const std::filesystem::path& ccm_directory);

private:
    std::filesystem::path run_directory_;
    std::filesystem::path clusters_directory_;
    std::filesystem::path diagnostics_directory_;
    std::string ccm_executable_;
    std::chrono::seconds command_timeout_;
    mutable std::atomic<std::uint64_t> command_counter_{0};

    void RunCcm(
        const std::filesystem::path& ccm_directory,
        const std::vector<std::string>& arguments) const;
    void RunCommand(
        const std::filesystem::path& ccm_directory,
        const std::vector<std::string>& command) const;
    void WaitForAlternator(
        const PhysicalTestCluster& cluster,
        const std::vector<TestClusterNode>& nodes,
        std::chrono::seconds timeout) const;
    void ConfigureNodeCertificate(
        const TestClusterNode& node,
        const std::filesystem::path& ccm_directory,
        const std::filesystem::path& ca_certificate_path) const;
    void CollectDiagnostics(
        const std::string& instance_id,
        const std::filesystem::path& ccm_directory) const;
    void RemoveByName(
        const std::string& instance_id,
        const std::filesystem::path& ccm_directory) const;
};

class CcmRunState {
public:
    struct ClusterHandle {
        ClusterHandle();
        ~ClusterHandle();
        ClusterHandle(ClusterHandle&& other) noexcept;
        ClusterHandle& operator=(ClusterHandle&& other) noexcept;
        ClusterHandle(const ClusterHandle&) = delete;
        ClusterHandle& operator=(const ClusterHandle&) = delete;

        int ccm_id = 0;
        std::string instance_id;

    private:
        int lock_fd = -1;
        std::filesystem::path reservation_path;
        std::filesystem::path manifest_path;
        bool completed = false;
        friend class CcmRunState;
    };

    using StaleClusterCleanup = std::function<void(
        const std::filesystem::path&,
        const std::string&,
        int)>;

    static std::unique_ptr<CcmRunState> OpenDefault(StaleClusterCleanup cleanup);
    static std::unique_ptr<CcmRunState> Open(
        const std::filesystem::path& root,
        StaleClusterCleanup cleanup);
    ~CcmRunState();

    CcmRunState(CcmRunState&&) = delete;
    CcmRunState& operator=(CcmRunState&&) = delete;
    CcmRunState(const CcmRunState&) = delete;
    CcmRunState& operator=(const CcmRunState&) = delete;

    [[nodiscard]] std::filesystem::path RunDirectory() const;
    [[nodiscard]] ClusterHandle BeginCluster(
        const std::string& instance_id,
        const ClusterSpec& spec,
        bool include_jmx_port);
    void CompleteCluster(ClusterHandle& handle);
    void Close();

    [[nodiscard]] static bool IsAddressRangeAvailable(
        const ClusterSpec& spec,
        int ccm_id,
        bool include_jmx_port);

private:
    CcmRunState(
        std::filesystem::path root,
        std::filesystem::path run_directory,
        StaleClusterCleanup cleanup);

    std::filesystem::path root_;
    std::filesystem::path runs_directory_;
    std::filesystem::path locks_directory_;
    std::filesystem::path run_directory_;
    StaleClusterCleanup cleanup_;
    std::mutex mutex_;
    int incomplete_reservation_fd_ = -1;
    bool incomplete_ownership_ = false;
    bool closed_ = false;
};

class PhysicalTestCluster final : public TestClusterInfo,
                                  public std::enable_shared_from_this<PhysicalTestCluster> {
public:
    PhysicalTestCluster(
        std::shared_ptr<ClusterProvisioner> provisioner,
        std::string instance_id,
        ClusterSpec spec,
        ProvisionedClusterData data);

    [[nodiscard]] std::string InstanceId() const override;
    [[nodiscard]] ClusterSpec Spec() const override;
    [[nodiscard]] std::vector<TestClusterNode> Nodes() const override;
    [[nodiscard]] AlternatorConnection Connection(AlternatorTransport transport) const override;
    [[nodiscard]] Config ClientConfig(AlternatorTransport transport) const override;

    [[nodiscard]] int CcmId() const;
    [[nodiscard]] std::filesystem::path CcmDirectory() const;
    [[nodiscard]] std::filesystem::path CaCertificatePath() const;

    void Start();
    void Stop();
    void StartNode(const TestClusterNode& node);
    void StopNode(const TestClusterNode& node);
    [[nodiscard]] TestClusterNode AddNode(
        const std::string& datacenter,
        const std::string& rack);
    void RemoveNode(const TestClusterNode& node);
    [[nodiscard]] bool IsDirty() const;
    void MarkDirty();
    void MarkRecoveryRequired();
    void RemovePhysical();

private:
    enum class LifecycleState { Open, Removing, RemovalFailed, Closed };
    enum class NodeState { Running, Stopped, Decommissioned };

    std::shared_ptr<ClusterProvisioner> provisioner_;
    std::string instance_id_;
    ClusterSpec spec_;
    int ccm_id_;
    std::filesystem::path ccm_directory_;
    std::filesystem::path ca_certificate_path_;
    std::optional<Credentials> credentials_;
    mutable std::mutex mutation_mutex_;
    mutable std::mutex mutex_;
    std::vector<TestClusterNode> nodes_;
    std::map<std::string, NodeState> node_states_;
    LifecycleState lifecycle_state_ = LifecycleState::Open;
    bool dirty_ = false;
    bool recovery_required_ = false;

    [[nodiscard]] TestClusterNode FindNode(const TestClusterNode& requested) const;
    void EnsureMutable() const;
};

class TestClusterPool final : public std::enable_shared_from_this<TestClusterPool> {
public:
    using ResourceCleanup = std::function<void(TestResourceScope&)>;

    TestClusterPool(
        std::shared_ptr<ClusterProvisioner> provisioner,
        int maximum_nodes,
        ResourceCleanup resource_cleanup = {},
        std::unique_ptr<CcmRunState> run_state = {});
    ~TestClusterPool();

    static std::shared_ptr<TestClusterPool> CreateDefault();

    [[nodiscard]] ReusableClusterLease AcquireReusable(const ClusterSpec& spec);
    [[nodiscard]] PrivateClusterLease ProvisionPrivate(const ClusterSpec& spec);
    [[nodiscard]] TestClusterNode AddPrivateNode(
        const std::shared_ptr<PhysicalTestCluster>& cluster,
        const std::shared_ptr<LeaseValidity>& validity,
        const std::string& datacenter,
        const std::string& rack);
    void Close();

private:
    struct Slot;
    struct Ownership;

    std::shared_ptr<ClusterProvisioner> provisioner_;
    int maximum_nodes_;
    ResourceCleanup resource_cleanup_;
    std::unique_ptr<CcmRunState> run_state_;
    std::mutex mutex_;
    std::unique_ptr<Slot> current_;
    std::uint64_t generation_ = 0;
    std::uint64_t lease_counter_ = 0;
    std::exception_ptr terminal_failure_;
    bool closed_ = false;

    [[nodiscard]] std::unique_ptr<Slot> Provision(const ClusterSpec& spec, bool private_cluster);
    [[nodiscard]] Ownership ReserveOwnership(
        const ClusterSpec& spec,
        const std::string& instance_id);
    void CompleteOwnership(Ownership& ownership);
    [[nodiscard]] TestResourceScope CreateResourceScope(
        const std::shared_ptr<PhysicalTestCluster>& cluster,
        const std::shared_ptr<LeaseValidity>& validity);
    void ReleaseReusable(
        std::uint64_t generation,
        const std::shared_ptr<TestResourceScope::Impl>& resources);
    void ReleasePrivate(
        const std::shared_ptr<PhysicalTestCluster>& cluster,
        const std::shared_ptr<LeaseValidity>& validity);
    void RetireCurrent();
    void ValidateDemand(const ClusterSpec& spec) const;
    void ThrowIfUnavailable() const;

    friend class ReusableClusterLease::Impl;
    friend class PrivateClusterLease::Impl;
};

} // namespace scylladb::alternator::testinfra
