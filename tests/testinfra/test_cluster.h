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

#include "cluster_spec.h"

#include <scylladb/alternator/config.h>
#include <scylladb/alternator/uri.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scylladb::alternator::testinfra {

class PhysicalTestCluster;

struct TestClusterNode {
    TestClusterNode() = default;

    TestClusterNode(
        std::string node_name,
        std::string node_address,
        std::string node_datacenter,
        std::string node_rack)
        : name(std::move(node_name))
        , address(std::move(node_address))
        , datacenter(std::move(node_datacenter))
        , rack(std::move(node_rack)) {}

    std::string name;
    std::string address;
    std::string datacenter;
    std::string rack;

    friend bool operator==(const TestClusterNode& first, const TestClusterNode& second) {
        return first.name == second.name && first.address == second.address &&
               first.datacenter == second.datacenter && first.rack == second.rack &&
               first.identity_ == second.identity_;
    }

private:
    std::uint64_t identity_ = 0;

    friend class PhysicalTestCluster;
};

struct AlternatorConnection {
    Url seed_endpoint;
    std::vector<Url> node_endpoints;
    std::optional<Credentials> credentials;
    std::filesystem::path ca_certificate_path;
};

class TestClusterInfo {
public:
    virtual ~TestClusterInfo() = default;

    [[nodiscard]] virtual std::string InstanceId() const = 0;
    [[nodiscard]] virtual ClusterSpec Spec() const = 0;
    [[nodiscard]] virtual std::vector<TestClusterNode> Nodes() const = 0;
    [[nodiscard]] virtual AlternatorConnection Connection(AlternatorTransport transport) const = 0;
    [[nodiscard]] virtual Config ClientConfig(AlternatorTransport transport) const = 0;
};

class TestResourceScope {
public:
    TestResourceScope() = default;

    [[nodiscard]] std::string NewTableName(const std::string& hint) const;
    [[nodiscard]] std::string Prefix() const;

private:
    class Impl;
    explicit TestResourceScope(std::shared_ptr<Impl> impl);
    TestResourceScope(std::shared_ptr<Impl> impl, bool bypass_validity);

    std::shared_ptr<Impl> impl_;
    bool bypass_validity_ = false;

    friend class ReusableClusterLease;
    friend class PrivateClusterLease;
    friend class TestClusterPool;
};

class PrivateClusterControl {
public:
    virtual ~PrivateClusterControl() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual void StartNode(const TestClusterNode& node) = 0;
    virtual void StopNode(const TestClusterNode& node) = 0;
    [[nodiscard]] virtual TestClusterNode AddNode(
        const std::string& datacenter,
        const std::string& rack) = 0;
    virtual void RemoveNode(const TestClusterNode& node) = 0;
};

class ReusableClusterLease {
public:
    ReusableClusterLease() = default;
    ~ReusableClusterLease();

    ReusableClusterLease(ReusableClusterLease&& other) noexcept;
    ReusableClusterLease& operator=(ReusableClusterLease&& other) noexcept;
    ReusableClusterLease(const ReusableClusterLease&) = delete;
    ReusableClusterLease& operator=(const ReusableClusterLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const TestClusterInfo& Cluster() const;
    [[nodiscard]] TestResourceScope& Resources();
    [[nodiscard]] const TestResourceScope& Resources() const;
    void Close();

private:
    class Impl;
    explicit ReusableClusterLease(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class TestClusterPool;
};

class PrivateClusterLease {
public:
    PrivateClusterLease() = default;
    ~PrivateClusterLease();

    PrivateClusterLease(PrivateClusterLease&& other) noexcept;
    PrivateClusterLease& operator=(PrivateClusterLease&& other) noexcept;
    PrivateClusterLease(const PrivateClusterLease&) = delete;
    PrivateClusterLease& operator=(const PrivateClusterLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] const TestClusterInfo& Cluster() const;
    [[nodiscard]] PrivateClusterControl& Control();
    [[nodiscard]] TestResourceScope& Resources();
    [[nodiscard]] const TestResourceScope& Resources() const;
    void Close();

private:
    class Impl;
    explicit PrivateClusterLease(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;

    friend class TestClusterPool;
};

/** Process-wide entry point for CCM-backed integration-test clusters. */
class TestClusters {
public:
    [[nodiscard]] static ReusableClusterLease AcquireReusable(const ClusterSpec& spec);
    [[nodiscard]] static PrivateClusterLease ProvisionPrivate(const ClusterSpec& spec);
    static void CloseAll();
};

} // namespace scylladb::alternator::testinfra
