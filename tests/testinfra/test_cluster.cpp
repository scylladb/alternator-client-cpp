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

#include "test_cluster_internal.h"

#include "dynamodb_test_client.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace scylladb::alternator::testinfra {

class LeaseValidity {
public:
    explicit LeaseValidity(std::string closed_message)
        : closed_message_(std::move(closed_message)) {}

    [[nodiscard]] bool IsValid() const noexcept {
        return valid_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_lock<std::shared_mutex> Acquire() const {
        if (!IsValid()) {
            throw std::logic_error(closed_message_);
        }
        std::shared_lock<std::shared_mutex> operation(mutex_);
        if (!IsValid()) {
            throw std::logic_error(closed_message_);
        }
        return operation;
    }

    void Invalidate() {
        MarkInvalid();
        WaitForOperations();
    }

    void MarkInvalid() noexcept {
        valid_.store(false, std::memory_order_release);
    }

    void WaitForOperations() const {
        std::unique_lock<std::shared_mutex> barrier(mutex_);
    }

    void Revalidate() {
        std::unique_lock<std::shared_mutex> barrier(mutex_);
        valid_.store(true, std::memory_order_release);
    }

private:
    std::string closed_message_;
    mutable std::shared_mutex mutex_;
    std::atomic<bool> valid_{true};
};

namespace {

constexpr std::uint16_t kHttpPort = 8080;
constexpr std::uint16_t kHttpsPort = 8043;
constexpr std::size_t kMaximumTableNameLength = 192;
constexpr std::size_t kUniqueSuffixLength = 33;

std::uint64_t NextNodeIdentity() {
    static std::atomic<std::uint64_t> identity{0};
    const auto next = identity.fetch_add(1, std::memory_order_relaxed) + 1;
    if (next == 0) {
        throw std::overflow_error("CCM test-cluster node identity space is exhausted");
    }
    return next;
}

std::string RandomHex(std::size_t bytes) {
    std::random_device device;
    std::uniform_int_distribution<unsigned> distribution(0, 255);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes; ++index) {
        output << std::setw(2) << distribution(device);
    }
    return output.str();
}

std::string SanitizeResourceName(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '-' || character == '.') {
            result.push_back(static_cast<char>(character));
        } else if (character >= 'A' && character <= 'Z') {
            result.push_back(static_cast<char>(std::tolower(character)));
        } else {
            result.push_back('_');
        }
    }
    return result;
}

std::string Truncate(const std::string& value, std::size_t maximum_length) {
    return value.size() <= maximum_length ? value : value.substr(0, maximum_length);
}

int ParseMaximumNodes() {
    const char* configured = std::getenv("SCYLLA_CCM_MAX_NODES");
    if (configured == nullptr || *configured == '\0') {
        return ClusterSpec::kMaximumNodeCount;
    }
    const std::string value(configured);
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= '0' && character <= '9';
        })) {
        throw std::runtime_error("SCYLLA_CCM_MAX_NODES must be a positive integer");
    }
    std::size_t parsed_characters = 0;
    const auto parsed = std::stoul(value, &parsed_characters);
    if (parsed_characters != value.size() || parsed == 0 ||
        parsed > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("SCYLLA_CCM_MAX_NODES must be a positive integer");
    }
    return std::min(static_cast<int>(parsed), ClusterSpec::kMaximumNodeCount);
}

std::string CreateInstanceId() {
    static const auto process_token = RandomHex(8);
    static std::atomic<std::uint64_t> counter{0};
    return "alternator-cpp-" + std::to_string(getpid()) + "-" + process_token + "-" +
           std::to_string(++counter);
}

void ReportDestructorFailure(const char* owner, const std::exception& failure) noexcept {
    std::cerr << owner << " cleanup failed: " << failure.what() << '\n';
}

class ReadOnlyTestCluster final : public TestClusterInfo {
public:
    ReadOnlyTestCluster(
        std::shared_ptr<PhysicalTestCluster> cluster,
        std::shared_ptr<LeaseValidity> validity,
        bool snapshot_nodes = true)
        : cluster_(std::move(cluster))
        , validity_(std::move(validity))
        , snapshot_nodes_(snapshot_nodes)
        , nodes_(snapshot_nodes_ ? cluster_->Nodes() : std::vector<TestClusterNode>{}) {}

    std::string InstanceId() const override {
        auto operation = validity_->Acquire();
        return cluster_->InstanceId();
    }

    ClusterSpec Spec() const override {
        auto operation = validity_->Acquire();
        return cluster_->Spec();
    }

    std::vector<TestClusterNode> Nodes() const override {
        auto operation = validity_->Acquire();
        return snapshot_nodes_ ? nodes_ : cluster_->Nodes();
    }

    AlternatorConnection Connection(AlternatorTransport transport) const override {
        auto operation = validity_->Acquire();
        return cluster_->Connection(transport);
    }

    Config ClientConfig(AlternatorTransport transport) const override {
        auto operation = validity_->Acquire();
        return cluster_->ClientConfig(transport);
    }

private:
    std::shared_ptr<PhysicalTestCluster> cluster_;
    std::shared_ptr<LeaseValidity> validity_;
    const bool snapshot_nodes_;
    const std::vector<TestClusterNode> nodes_;
};

class PrivateControlAdapter final : public PrivateClusterControl {
public:
    PrivateControlAdapter(
        std::shared_ptr<TestClusterPool> pool,
        std::shared_ptr<PhysicalTestCluster> cluster,
        std::shared_ptr<LeaseValidity> validity)
        : pool_(std::move(pool))
        , cluster_(std::move(cluster))
        , validity_(std::move(validity)) {}

    void Start() override {
        auto operation = validity_->Acquire();
        cluster_->Start();
    }

    void Stop() override {
        auto operation = validity_->Acquire();
        cluster_->Stop();
    }

    void StartNode(const TestClusterNode& node) override {
        auto operation = validity_->Acquire();
        cluster_->StartNode(node);
    }

    void StopNode(const TestClusterNode& node) override {
        auto operation = validity_->Acquire();
        cluster_->StopNode(node);
    }

    TestClusterNode AddNode(const std::string& datacenter, const std::string& rack) override {
        return pool_->AddPrivateNode(cluster_, validity_, datacenter, rack);
    }

    void RemoveNode(const TestClusterNode& node) override {
        auto operation = validity_->Acquire();
        cluster_->RemoveNode(node);
    }

private:
    std::shared_ptr<TestClusterPool> pool_;
    std::shared_ptr<PhysicalTestCluster> cluster_;
    std::shared_ptr<LeaseValidity> validity_;
};

struct SharedPoolHolder {
    std::mutex mutex;
    std::shared_ptr<TestClusterPool> pool;

    ~SharedPoolHolder() {
        try {
            if (pool != nullptr) {
                pool->Close();
            }
        } catch (const std::exception& failure) {
            ReportDestructorFailure("CCM pool", failure);
        }
    }
};

SharedPoolHolder& ProcessPool() {
    static SharedPoolHolder holder;
    return holder;
}

std::shared_ptr<TestClusterPool> GetProcessPool() {
    auto& holder = ProcessPool();
    std::lock_guard<std::mutex> lock(holder.mutex);
    if (holder.pool == nullptr) {
        holder.pool = TestClusterPool::CreateDefault();
    }
    return holder.pool;
}

} // namespace

bool ClusterProvisioner::RequiresJmxPortReservation(const ClusterSpec&) const {
    return false;
}

class TestResourceScope::Impl {
public:
    Impl(
        std::shared_ptr<PhysicalTestCluster> cluster,
        std::shared_ptr<LeaseValidity> validity,
        const std::string& run_id,
        std::uint64_t lease_id)
        : cluster_(std::move(cluster))
        , validity_(std::move(validity)) {
        const auto lease_component = "_" + std::to_string(lease_id) + "_";
        const auto fixed_length = std::string("cpp_it_").size() + lease_component.size() +
                                  kUniqueSuffixLength;
        if (fixed_length > kMaximumTableNameLength) {
            throw std::length_error("CCM resource prefix exceeds Alternator table-name limit");
        }
        prefix_ = "cpp_it_" +
                  Truncate(SanitizeResourceName(run_id), kMaximumTableNameLength - fixed_length) +
                  lease_component;
    }

    std::string NewTableName(const std::string& hint, bool bypass_validity) const {
        if (!bypass_validity) {
            auto operation = validity_->Acquire();
            return NewTableNameUnchecked(hint);
        }
        return NewTableNameUnchecked(hint);
    }

    std::string Prefix(bool bypass_validity) const {
        if (!bypass_validity) {
            auto operation = validity_->Acquire();
            return prefix_;
        }
        return prefix_;
    }

    void Cleanup() {
        auto cluster = cluster_.lock();
        if (cluster == nullptr) {
            return;
        }
        const auto spec = cluster->Spec();
        const auto transport = spec.Transports().count(AlternatorTransport::Http) != 0
                                   ? AlternatorTransport::Http
                                   : AlternatorTransport::Https;
        CleanupDynamoDbTables(cluster->Connection(transport), prefix_, std::chrono::minutes(2));
    }

private:
    std::string NewTableNameUnchecked(const std::string& hint) const {
        const auto suffix = "_" + RandomHex(16);
        const auto maximum_hint_length = kMaximumTableNameLength - prefix_.size() - suffix.size();
        return prefix_ + Truncate(SanitizeResourceName(hint), maximum_hint_length) + suffix;
    }

    std::weak_ptr<PhysicalTestCluster> cluster_;
    std::shared_ptr<LeaseValidity> validity_;
    std::string prefix_;
};

TestResourceScope::TestResourceScope(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TestResourceScope::TestResourceScope(
    std::shared_ptr<Impl> impl,
    bool bypass_validity)
    : impl_(std::move(impl))
    , bypass_validity_(bypass_validity) {}

std::string TestResourceScope::NewTableName(const std::string& hint) const {
    if (impl_ == nullptr) {
        throw std::logic_error("CCM resource scope is empty");
    }
    return impl_->NewTableName(hint, bypass_validity_);
}

std::string TestResourceScope::Prefix() const {
    if (impl_ == nullptr) {
        throw std::logic_error("CCM resource scope is empty");
    }
    return impl_->Prefix(bypass_validity_);
}

PhysicalTestCluster::PhysicalTestCluster(
    std::shared_ptr<ClusterProvisioner> provisioner,
    std::string instance_id,
    ClusterSpec spec,
    ProvisionedClusterData data)
    : provisioner_(std::move(provisioner))
    , instance_id_(std::move(instance_id))
    , spec_(std::move(spec))
    , ccm_id_(data.ccm_id)
    , ccm_directory_(std::move(data.ccm_directory))
    , ca_certificate_path_(std::move(data.ca_certificate_path))
    , credentials_(std::move(data.credentials))
    , nodes_(std::move(data.nodes)) {
    for (auto& node : nodes_) {
        node.identity_ = NextNodeIdentity();
        node_states_[node.name] = NodeState::Running;
    }
}

std::string PhysicalTestCluster::InstanceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instance_id_;
}

ClusterSpec PhysicalTestCluster::Spec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spec_;
}

std::vector<TestClusterNode> PhysicalTestCluster::Nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_;
}

AlternatorConnection PhysicalTestCluster::Connection(AlternatorTransport transport) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spec_.Transports().count(transport) == 0) {
        throw std::logic_error(
            "cluster '" + instance_id_ + "' does not provide requested Alternator transport");
    }
    if (nodes_.empty()) {
        throw std::logic_error("cluster '" + instance_id_ + "' has no nodes");
    }
    const bool https = transport == AlternatorTransport::Https;
    const std::string scheme = https ? "https" : "http";
    const auto port = https ? kHttpsPort : kHttpPort;
    std::vector<Url> endpoints;
    endpoints.reserve(nodes_.size());
    for (const auto& node : nodes_) {
        endpoints.push_back(Url::FromHostPort(scheme, node.address, port));
    }
    return AlternatorConnection{
        endpoints.front(),
        endpoints,
        credentials_,
        https ? ca_certificate_path_ : std::filesystem::path{},
    };
}

Config PhysicalTestCluster::ClientConfig(AlternatorTransport transport) const {
    const auto connection = Connection(transport);
    Config config;
    config.scheme = connection.seed_endpoint.scheme;
    config.port = connection.seed_endpoint.port;
    config.aws_region = "us-east-1";
    config.credentials = connection.credentials.value_or(Credentials{"alternator", "secret"});
    config.nodes_list_update_period = std::chrono::milliseconds{0};
    config.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    config.node_health.disabled = true;
    config.http_client_timeout = std::chrono::seconds{5};
    config.connect_timeout = std::chrono::seconds{1};
    if (!connection.ca_certificate_path.empty()) {
        config.ca_file = connection.ca_certificate_path.string();
    }
    return config;
}

int PhysicalTestCluster::CcmId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ccm_id_;
}

std::filesystem::path PhysicalTestCluster::CcmDirectory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ccm_directory_;
}

std::filesystem::path PhysicalTestCluster::CaCertificatePath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ca_certificate_path_;
}

void PhysicalTestCluster::Start() {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureMutable();
    }
    try {
        provisioner_->Start(*this);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& state : node_states_) {
            if (state.second != NodeState::Decommissioned) {
                state.second = NodeState::Running;
            }
        }
    } catch (const RecoveryRequiredError&) {
        MarkRecoveryRequired();
        throw;
    } catch (...) {
        MarkDirty();
        throw;
    }
}

void PhysicalTestCluster::Stop() {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureMutable();
    }
    try {
        provisioner_->Stop(*this);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& state : node_states_) {
            if (state.second != NodeState::Decommissioned) {
                state.second = NodeState::Stopped;
            }
        }
    } catch (const RecoveryRequiredError&) {
        MarkRecoveryRequired();
        throw;
    } catch (...) {
        MarkDirty();
        throw;
    }
}

void PhysicalTestCluster::StartNode(const TestClusterNode& requested) {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    TestClusterNode node;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureMutable();
        node = FindNode(requested);
        if (node_states_.at(node.name) == NodeState::Decommissioned) {
            throw std::logic_error("cannot start decommissioned node " + node.name);
        }
    }
    try {
        provisioner_->StartNode(*this, node);
        std::lock_guard<std::mutex> lock(mutex_);
        node_states_[node.name] = NodeState::Running;
    } catch (const RecoveryRequiredError&) {
        MarkRecoveryRequired();
        throw;
    } catch (...) {
        MarkDirty();
        throw;
    }
}

void PhysicalTestCluster::StopNode(const TestClusterNode& requested) {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    TestClusterNode node;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureMutable();
        node = FindNode(requested);
        if (node_states_.at(node.name) == NodeState::Decommissioned) {
            throw std::logic_error("cannot stop decommissioned node " + node.name);
        }
    }
    try {
        provisioner_->StopNode(*this, node);
        std::lock_guard<std::mutex> lock(mutex_);
        node_states_[node.name] = NodeState::Stopped;
    } catch (const RecoveryRequiredError&) {
        MarkRecoveryRequired();
        throw;
    } catch (...) {
        MarkDirty();
        throw;
    }
}

TestClusterNode PhysicalTestCluster::AddNode(
    const std::string& datacenter,
    const std::string& rack) {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureMutable();
        if (nodes_.size() >= static_cast<std::size_t>(ClusterSpec::kMaximumNodeCount)) {
            throw std::logic_error(
                "a cluster cannot exceed " + std::to_string(ClusterSpec::kMaximumNodeCount) +
                " nodes");
        }
    }
    try {
        auto node = provisioner_->AddNode(*this, datacenter, rack);
        std::lock_guard<std::mutex> lock(mutex_);
        node.identity_ = NextNodeIdentity();
        nodes_.push_back(node);
        node_states_[node.name] = NodeState::Running;
        return node;
    } catch (const NodeProvisioningError& failure) {
        if (failure.RecoveryRequired()) {
            MarkRecoveryRequired();
        } else if (!failure.CleanupProven()) {
            MarkDirty();
        }
        throw;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const RecoveryRequiredError&) {
        MarkRecoveryRequired();
        throw;
    } catch (...) {
        MarkDirty();
        throw;
    }
}

void PhysicalTestCluster::RemoveNode(const TestClusterNode& requested) {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    TestClusterNode node;
    NodeState state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EnsureMutable();
        node = FindNode(requested);
        state = node_states_.at(node.name);
        std::size_t active_nodes = 0;
        std::size_t running_nodes = 0;
        for (const auto& entry : node_states_) {
            if (entry.second != NodeState::Decommissioned) {
                ++active_nodes;
            }
            if (entry.second == NodeState::Running) {
                ++running_nodes;
            }
        }
        if (state != NodeState::Decommissioned && active_nodes == 1) {
            throw std::logic_error("cannot remove final node from cluster");
        }
        if (state != NodeState::Decommissioned) {
            const auto running_during_decommission =
                running_nodes + (state == NodeState::Stopped ? 1U : 0U);
            const auto decommission_voter_quorum = active_nodes / 2U + 1U;
            const auto running_after_decommission = running_during_decommission - 1U;
            const auto active_after_decommission = active_nodes - 1U;
            const auto remaining_voter_quorum = active_after_decommission / 2U + 1U;
            if (running_during_decommission < decommission_voter_quorum ||
                running_after_decommission < remaining_voter_quorum) {
                throw std::logic_error(
                    "cannot remove node " + node.name +
                    " without a live voter quorum during and after decommission; "
                    "start more nodes first");
            }
        }
    }
    try {
        if (state == NodeState::Stopped) {
            provisioner_->StartNode(*this, node);
        }
        if (state != NodeState::Decommissioned) {
            provisioner_->DecommissionNode(*this, node);
        }
        provisioner_->DeleteNodeState(*this, node);
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_.erase(
            std::remove_if(nodes_.begin(), nodes_.end(), [&](const TestClusterNode& candidate) {
                return candidate.name == node.name;
            }),
            nodes_.end());
        node_states_.erase(node.name);
    } catch (const RecoveryRequiredError&) {
        MarkRecoveryRequired();
        throw;
    } catch (...) {
        MarkDirty();
        throw;
    }
}

bool PhysicalTestCluster::IsDirty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dirty_;
}

void PhysicalTestCluster::MarkDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = true;
}

void PhysicalTestCluster::MarkRecoveryRequired() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = true;
    recovery_required_ = true;
}

void PhysicalTestCluster::RemovePhysical() {
    std::lock_guard<std::mutex> mutation_lock(mutation_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_state_ == LifecycleState::Closed) {
            return;
        }
        if (lifecycle_state_ == LifecycleState::Removing) {
            throw std::logic_error("cluster removal is already in progress");
        }
        if (recovery_required_) {
            throw std::logic_error(
                "cluster '" + instance_id_ +
                "' has unproven command-process cleanup and must be recovered by next test process");
        }
        lifecycle_state_ = LifecycleState::Removing;
    }
    try {
        provisioner_->Remove(*this);
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_state_ = LifecycleState::Closed;
    } catch (const RecoveryRequiredError&) {
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_state_ = LifecycleState::RemovalFailed;
        dirty_ = true;
        recovery_required_ = true;
        throw;
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_state_ = LifecycleState::RemovalFailed;
        dirty_ = true;
        throw;
    }
}

TestClusterNode PhysicalTestCluster::FindNode(const TestClusterNode& requested) const {
    const auto found = std::find_if(
        nodes_.begin(), nodes_.end(), [&](const TestClusterNode& candidate) {
            return candidate.identity_ != 0 && candidate.identity_ == requested.identity_;
        });
    if (found == nodes_.end()) {
        throw std::invalid_argument("node is not part of cluster: " + requested.name);
    }
    return *found;
}

void PhysicalTestCluster::EnsureMutable() const {
    if (lifecycle_state_ != LifecycleState::Open) {
        throw std::logic_error("cluster '" + instance_id_ + "' is closing or removed");
    }
    if (dirty_) {
        throw std::logic_error(
            "cluster '" + instance_id_ + "' has ambiguous state and must be removed");
    }
}

class ReusableClusterLease::Impl {
public:
    Impl(
        std::shared_ptr<TestClusterPool> pool,
        std::uint64_t generation,
        std::shared_ptr<PhysicalTestCluster> cluster,
        std::shared_ptr<LeaseValidity> validity,
        TestResourceScope resources)
        : pool_(std::move(pool))
        , generation_(generation)
        , cluster_(std::make_unique<ReadOnlyTestCluster>(std::move(cluster), validity))
        , validity_(std::move(validity))
        , resources_(std::move(resources)) {}

    void Close() {
        std::lock_guard<std::mutex> lock(close_mutex_);
        if (closed_.load()) {
            return;
        }
        validity_->Invalidate();
        closed_.store(true);
        pool_->ReleaseReusable(generation_, resources_.impl_);
    }

    [[nodiscard]] bool IsClosed() const noexcept {
        return closed_.load() || !validity_->IsValid();
    }

    std::shared_ptr<TestClusterPool> pool_;
    std::uint64_t generation_;
    std::unique_ptr<TestClusterInfo> cluster_;
    std::shared_ptr<LeaseValidity> validity_;
    TestResourceScope resources_;
    std::mutex close_mutex_;
    std::atomic<bool> closed_{false};
};

ReusableClusterLease::ReusableClusterLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ReusableClusterLease::~ReusableClusterLease() {
    try {
        Close();
    } catch (const std::exception& failure) {
        ReportDestructorFailure("reusable CCM lease", failure);
    }
}

ReusableClusterLease::ReusableClusterLease(ReusableClusterLease&& other) noexcept = default;

ReusableClusterLease& ReusableClusterLease::operator=(ReusableClusterLease&& other) noexcept {
    if (this != &other) {
        try {
            Close();
        } catch (const std::exception& failure) {
            ReportDestructorFailure("reusable CCM lease", failure);
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

ReusableClusterLease::operator bool() const noexcept {
    return impl_ != nullptr && !impl_->IsClosed();
}

const TestClusterInfo& ReusableClusterLease::Cluster() const {
    if (!*this) {
        throw std::logic_error("reusable CCM lease is closed");
    }
    return *impl_->cluster_;
}

TestResourceScope& ReusableClusterLease::Resources() {
    if (!*this) {
        throw std::logic_error("reusable CCM lease is closed");
    }
    return impl_->resources_;
}

const TestResourceScope& ReusableClusterLease::Resources() const {
    if (!*this) {
        throw std::logic_error("reusable CCM lease is closed");
    }
    return impl_->resources_;
}

void ReusableClusterLease::Close() {
    if (impl_ != nullptr) {
        impl_->Close();
    }
}

class PrivateClusterLease::Impl {
public:
    Impl(
        std::shared_ptr<TestClusterPool> pool,
        std::shared_ptr<PhysicalTestCluster> cluster,
        std::shared_ptr<LeaseValidity> validity,
        TestResourceScope resources)
        : pool_(std::move(pool))
        , cluster_(std::move(cluster))
        , info_(std::make_unique<ReadOnlyTestCluster>(cluster_, validity, false))
        , control_(pool_, cluster_, validity)
        , validity_(std::move(validity))
        , resources_(std::move(resources)) {}

    void Close() {
        std::lock_guard<std::mutex> lock(close_mutex_);
        if (closed_.load()) {
            return;
        }
        pool_->ReleasePrivate(cluster_, validity_);
        closed_.store(true);
    }

    [[nodiscard]] bool IsClosed() const noexcept {
        return closed_.load() || !validity_->IsValid();
    }

    std::shared_ptr<TestClusterPool> pool_;
    std::shared_ptr<PhysicalTestCluster> cluster_;
    std::unique_ptr<TestClusterInfo> info_;
    PrivateControlAdapter control_;
    std::shared_ptr<LeaseValidity> validity_;
    TestResourceScope resources_;
    std::mutex close_mutex_;
    std::atomic<bool> closed_{false};
};

PrivateClusterLease::PrivateClusterLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

PrivateClusterLease::~PrivateClusterLease() {
    try {
        Close();
    } catch (const std::exception& failure) {
        ReportDestructorFailure("private CCM lease", failure);
    }
}

PrivateClusterLease::PrivateClusterLease(PrivateClusterLease&& other) noexcept = default;

PrivateClusterLease& PrivateClusterLease::operator=(PrivateClusterLease&& other) noexcept {
    if (this != &other) {
        try {
            Close();
        } catch (const std::exception& failure) {
            ReportDestructorFailure("private CCM lease", failure);
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

PrivateClusterLease::operator bool() const noexcept {
    return impl_ != nullptr && !impl_->IsClosed();
}

const TestClusterInfo& PrivateClusterLease::Cluster() const {
    if (!*this) {
        throw std::logic_error("private CCM lease is closed");
    }
    return *impl_->info_;
}

PrivateClusterControl& PrivateClusterLease::Control() {
    if (!*this) {
        throw std::logic_error("private CCM lease is closed");
    }
    return impl_->control_;
}

TestResourceScope& PrivateClusterLease::Resources() {
    if (!*this) {
        throw std::logic_error("private CCM lease is closed");
    }
    return impl_->resources_;
}

const TestResourceScope& PrivateClusterLease::Resources() const {
    if (!*this) {
        throw std::logic_error("private CCM lease is closed");
    }
    return impl_->resources_;
}

void PrivateClusterLease::Close() {
    if (impl_ != nullptr) {
        impl_->Close();
    }
}

struct TestClusterPool::Ownership {
    int ccm_id = 0;
    std::optional<CcmRunState::ClusterHandle> durable_handle;
};

struct TestClusterPool::Slot {
    std::uint64_t generation = 0;
    std::string reuse_key;
    bool private_cluster = false;
    std::shared_ptr<PhysicalTestCluster> cluster;
    std::vector<std::weak_ptr<LeaseValidity>> lease_validities;
    Ownership ownership;
    int references = 0;
    bool poisoned = false;

    std::shared_ptr<LeaseValidity> RegisterLease() {
        lease_validities.erase(
            std::remove_if(
                lease_validities.begin(),
                lease_validities.end(),
                [](const auto& validity) { return validity.expired(); }),
            lease_validities.end());
        auto validity = std::make_shared<LeaseValidity>(
            private_cluster ? "private CCM lease is closed" : "reusable CCM lease is closed");
        lease_validities.emplace_back(validity);
        return validity;
    }

    void InvalidateLeases() {
        std::vector<std::shared_ptr<LeaseValidity>> validities;
        validities.reserve(lease_validities.size());
        for (const auto& weak_validity : lease_validities) {
            if (auto validity = weak_validity.lock()) {
                validity->MarkInvalid();
                validities.push_back(std::move(validity));
            }
        }
        for (const auto& validity : validities) {
            validity->WaitForOperations();
        }
    }
};

TestClusterPool::TestClusterPool(
    std::shared_ptr<ClusterProvisioner> provisioner,
    int maximum_nodes,
    ResourceCleanup resource_cleanup,
    std::unique_ptr<CcmRunState> run_state)
    : provisioner_(std::move(provisioner))
    , maximum_nodes_(maximum_nodes)
    , resource_cleanup_(std::move(resource_cleanup))
    , run_state_(std::move(run_state)) {
    if (provisioner_ == nullptr) {
        throw std::invalid_argument("cluster provisioner is required");
    }
    if (maximum_nodes_ < 1 || maximum_nodes_ > ClusterSpec::kMaximumNodeCount) {
        throw std::invalid_argument(
            "maximum_nodes must be between 1 and " +
            std::to_string(ClusterSpec::kMaximumNodeCount));
    }
    if (!resource_cleanup_) {
        resource_cleanup_ = [](TestResourceScope& resources) {
            resources.impl_->Cleanup();
        };
    }
}

TestClusterPool::~TestClusterPool() {
    try {
        Close();
    } catch (const std::exception& failure) {
        ReportDestructorFailure("CCM pool", failure);
    }
}

std::shared_ptr<TestClusterPool> TestClusterPool::CreateDefault() {
    auto state = CcmRunState::OpenDefault(
        [](const std::filesystem::path& run_directory,
           const std::string& instance_id,
           int ccm_id) {
            CcmProvisioner provisioner(run_directory);
            provisioner.CleanupStaleCluster(
                instance_id,
                ccm_id,
                run_directory / "clusters" / instance_id);
        });
    try {
        auto provisioner = std::make_shared<CcmProvisioner>(state->RunDirectory());
        return std::make_shared<TestClusterPool>(
            std::move(provisioner), ParseMaximumNodes(), ResourceCleanup{}, std::move(state));
    } catch (...) {
        try {
            state->Close();
        } catch (...) {
        }
        throw;
    }
}

ReusableClusterLease TestClusterPool::AcquireReusable(const ClusterSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidateDemand(spec);
    ThrowIfUnavailable();

    if (current_ != nullptr) {
        if (current_->private_cluster) {
            throw std::logic_error("private CCM cluster is already active in this process");
        }
        if (current_->references > 0) {
            if (current_->poisoned || current_->reuse_key != spec.ReuseKey()) {
                throw std::logic_error(
                    "active reusable CCM cluster is incompatible with requested specification");
            }
            ++current_->references;
            auto validity = current_->RegisterLease();
            auto resources = CreateResourceScope(current_->cluster, validity);
            return ReusableClusterLease(std::make_unique<ReusableClusterLease::Impl>(
                shared_from_this(),
                current_->generation,
                current_->cluster,
                std::move(validity),
                std::move(resources)));
        }

        if (current_->reuse_key == spec.ReuseKey() && !current_->poisoned &&
            provisioner_->IsHealthy(*current_->cluster)) {
            current_->references = 1;
            auto validity = current_->RegisterLease();
            auto resources = CreateResourceScope(current_->cluster, validity);
            return ReusableClusterLease(std::make_unique<ReusableClusterLease::Impl>(
                shared_from_this(),
                current_->generation,
                current_->cluster,
                std::move(validity),
                std::move(resources)));
        }
        RetireCurrent();
    }

    current_ = Provision(spec, false);
    current_->references = 1;
    auto validity = current_->RegisterLease();
    auto resources = CreateResourceScope(current_->cluster, validity);
    return ReusableClusterLease(std::make_unique<ReusableClusterLease::Impl>(
        shared_from_this(),
        current_->generation,
        current_->cluster,
        std::move(validity),
        std::move(resources)));
}

PrivateClusterLease TestClusterPool::ProvisionPrivate(const ClusterSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    ValidateDemand(spec);
    ThrowIfUnavailable();
    if (current_ != nullptr) {
        if (current_->private_cluster || current_->references > 0) {
            throw std::logic_error("CCM cluster lease is already active in this process");
        }
        RetireCurrent();
    }
    current_ = Provision(spec, true);
    auto validity = current_->RegisterLease();
    auto resources = CreateResourceScope(current_->cluster, validity);
    return PrivateClusterLease(std::make_unique<PrivateClusterLease::Impl>(
        shared_from_this(), current_->cluster, std::move(validity), std::move(resources)));
}

TestClusterNode TestClusterPool::AddPrivateNode(
    const std::shared_ptr<PhysicalTestCluster>& cluster,
    const std::shared_ptr<LeaseValidity>& validity,
    const std::string& datacenter,
    const std::string& rack) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || current_ == nullptr || !current_->private_cluster ||
        current_->cluster != cluster) {
        throw std::logic_error("private cluster is no longer owned by this pool");
    }
    auto operation = validity->Acquire();
    if (cluster->Nodes().size() >= static_cast<std::size_t>(maximum_nodes_)) {
        throw std::logic_error(
            "adding node would exceed this run's " + std::to_string(maximum_nodes_) +
            "-node limit");
    }
    return cluster->AddNode(datacenter, rack);
}

void TestClusterPool::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ && current_ == nullptr && run_state_ == nullptr) {
        return;
    }
    closed_ = true;
    std::exception_ptr failure;
    if (current_ != nullptr) {
        current_->InvalidateLeases();
        try {
            RetireCurrent();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (failure == nullptr && run_state_ != nullptr) {
        try {
            run_state_->Close();
            run_state_.reset();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

std::unique_ptr<TestClusterPool::Slot> TestClusterPool::Provision(
    const ClusterSpec& spec,
    bool private_cluster) {
    const auto instance_id = CreateInstanceId();
    auto ownership = ReserveOwnership(spec, instance_id);
    try {
        auto data = provisioner_->Provision(spec, instance_id, ownership.ccm_id);
        auto slot = std::make_unique<Slot>();
        slot->generation = ++generation_;
        slot->reuse_key = private_cluster ? std::string{} : spec.ReuseKey();
        slot->private_cluster = private_cluster;
        slot->cluster = std::make_shared<PhysicalTestCluster>(
            provisioner_, instance_id, spec, std::move(data));
        slot->ownership = std::move(ownership);
        return slot;
    } catch (const ClusterProvisioningError& failure) {
        if (failure.CleanupProven()) {
            try {
                CompleteOwnership(ownership);
            } catch (...) {
                terminal_failure_ = std::current_exception();
                throw;
            }
        } else {
            terminal_failure_ = std::current_exception();
            if (!failure.RecoveryRequired()) {
                ProvisionedClusterData retained;
                retained.ccm_id = ownership.ccm_id;
                retained.ccm_directory =
                    provisioner_->RunDirectory() / "clusters" / instance_id;
                auto slot = std::make_unique<Slot>();
                slot->generation = ++generation_;
                slot->private_cluster = true;
                slot->poisoned = true;
                slot->cluster = std::make_shared<PhysicalTestCluster>(
                    provisioner_, instance_id, spec, std::move(retained));
                slot->ownership = std::move(ownership);
                current_ = std::move(slot);
            }
        }
        throw;
    } catch (...) {
        terminal_failure_ = std::current_exception();
        throw;
    }
}

TestClusterPool::Ownership TestClusterPool::ReserveOwnership(
    const ClusterSpec& spec,
    const std::string& instance_id) {
    const bool include_jmx = provisioner_->RequiresJmxPortReservation(spec);
    Ownership ownership;
    if (run_state_ != nullptr) {
        ownership.durable_handle = run_state_->BeginCluster(instance_id, spec, include_jmx);
        ownership.ccm_id = ownership.durable_handle->ccm_id;
        return ownership;
    }
    for (int id = 1; id < 100; ++id) {
        if (CcmRunState::IsAddressRangeAvailable(spec, id, include_jmx)) {
            ownership.ccm_id = id;
            return ownership;
        }
    }
    throw std::runtime_error("no CCM cluster IDs are available");
}

void TestClusterPool::CompleteOwnership(Ownership& ownership) {
    if (run_state_ != nullptr && ownership.durable_handle.has_value()) {
        run_state_->CompleteCluster(*ownership.durable_handle);
        ownership.durable_handle.reset();
    }
}

TestResourceScope TestClusterPool::CreateResourceScope(
    const std::shared_ptr<PhysicalTestCluster>& cluster,
    const std::shared_ptr<LeaseValidity>& validity) {
    const auto run_id = provisioner_->RunDirectory().filename().string();
    return TestResourceScope(std::make_shared<TestResourceScope::Impl>(
        cluster, validity, run_id, ++lease_counter_));
}

void TestClusterPool::ReleaseReusable(
    std::uint64_t generation,
    const std::shared_ptr<TestResourceScope::Impl>& resources) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_ == nullptr || current_->generation != generation) {
        return;
    }
    if (current_->references <= 0) {
        throw std::logic_error("reusable CCM cluster lease released more than once");
    }

    std::exception_ptr failure;
    if (!closed_) {
        try {
            TestResourceScope scope(resources, true);
            resource_cleanup_(scope);
        } catch (...) {
            failure = std::current_exception();
            current_->poisoned = true;
            terminal_failure_ = failure;
        }
    }
    --current_->references;
    if (current_->references == 0 && (current_->poisoned || closed_)) {
        try {
            RetireCurrent();
            if (current_ == nullptr) {
                terminal_failure_ = nullptr;
            }
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void TestClusterPool::ReleasePrivate(
    const std::shared_ptr<PhysicalTestCluster>& cluster,
    const std::shared_ptr<LeaseValidity>& validity) {
    std::lock_guard<std::mutex> lock(mutex_);
    validity->Invalidate();
    if (current_ == nullptr || current_->cluster != cluster) {
        return;
    }
    if (!current_->private_cluster) {
        throw std::logic_error("cluster is not privately owned by this pool");
    }
    try {
        RetireCurrent();
    } catch (...) {
        if (!closed_) {
            validity->Revalidate();
        }
        throw;
    }
}

void TestClusterPool::RetireCurrent() {
    if (current_ == nullptr) {
        return;
    }
    try {
        current_->cluster->RemovePhysical();
        CompleteOwnership(current_->ownership);
        current_.reset();
        terminal_failure_ = nullptr;
    } catch (...) {
        terminal_failure_ = std::current_exception();
        throw;
    }
}

void TestClusterPool::ValidateDemand(const ClusterSpec& spec) const {
    spec.Validate();
    if (spec.Topology().NodeCount() > maximum_nodes_) {
        throw std::logic_error(
            "requested cluster exceeds this run's " + std::to_string(maximum_nodes_) +
            "-node limit");
    }
}

void TestClusterPool::ThrowIfUnavailable() const {
    if (closed_) {
        throw std::logic_error("CCM cluster pool is closed");
    }
    if (terminal_failure_ != nullptr) {
        try {
            std::rethrow_exception(terminal_failure_);
        } catch (...) {
            std::throw_with_nested(std::runtime_error(
                "CCM pool retained failed cleanup state; close before provisioning again"));
        }
    }
}

ReusableClusterLease TestClusters::AcquireReusable(const ClusterSpec& spec) {
    return GetProcessPool()->AcquireReusable(spec);
}

PrivateClusterLease TestClusters::ProvisionPrivate(const ClusterSpec& spec) {
    return GetProcessPool()->ProvisionPrivate(spec);
}

void TestClusters::CloseAll() {
    auto& holder = ProcessPool();
    std::lock_guard<std::mutex> lock(holder.mutex);
    if (holder.pool != nullptr) {
        holder.pool->Close();
        holder.pool.reset();
    }
}

} // namespace scylladb::alternator::testinfra
