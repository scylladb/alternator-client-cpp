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

#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scylladb::alternator::testinfra {

enum class AlternatorTransport {
    Http,
    Https,
};

enum class AuthenticationMode {
    AllowAll,
    Password,
    Transitional,
};

enum class AuthorizationMode {
    AllowAll,
    Cassandra,
    Transitional,
};

class RackSpec final {
public:
    explicit RackSpec(int node_count);

    [[nodiscard]] int NodeCount() const noexcept;

private:
    int node_count_;
};

bool operator==(const RackSpec& lhs, const RackSpec& rhs) noexcept;
bool operator!=(const RackSpec& lhs, const RackSpec& rhs) noexcept;

class DatacenterSpec final {
public:
    explicit DatacenterSpec(std::vector<RackSpec> racks);

    static DatacenterSpec Create(std::initializer_list<int> nodes_per_rack);
    static DatacenterSpec Create(const std::vector<int>& nodes_per_rack);

    [[nodiscard]] const std::vector<RackSpec>& Racks() const noexcept;

private:
    std::vector<RackSpec> racks_;
};

bool operator==(const DatacenterSpec& lhs, const DatacenterSpec& rhs) noexcept;
bool operator!=(const DatacenterSpec& lhs, const DatacenterSpec& rhs) noexcept;

class ClusterTopology final {
public:
    explicit ClusterTopology(std::vector<DatacenterSpec> datacenters);

    static ClusterTopology SingleDatacenter(int node_count);
    static ClusterTopology SingleDatacenter(std::initializer_list<int> nodes_per_rack);
    static ClusterTopology SingleDatacenter(const std::vector<int>& nodes_per_rack);

    [[nodiscard]] const std::vector<DatacenterSpec>& Datacenters() const noexcept;
    [[nodiscard]] int NodeCount() const;

private:
    std::vector<DatacenterSpec> datacenters_;
};

bool operator==(const ClusterTopology& lhs, const ClusterTopology& rhs) noexcept;
bool operator!=(const ClusterTopology& lhs, const ClusterTopology& rhs) noexcept;

class NodeResources final {
public:
    NodeResources(int smp, int memory_mib);

    [[nodiscard]] int Smp() const noexcept;
    [[nodiscard]] int MemoryMiB() const noexcept;

private:
    int smp_;
    int memory_mib_;
};

bool operator==(const NodeResources& lhs, const NodeResources& rhs) noexcept;
bool operator!=(const NodeResources& lhs, const NodeResources& rhs) noexcept;

class ClusterSecuritySpec final {
public:
    ClusterSecuritySpec(AuthenticationMode authentication,
                        AuthorizationMode authorization,
                        bool enforce_alternator_authorization);

    static ClusterSecuritySpec Disabled();
    static ClusterSecuritySpec Enforced();

    [[nodiscard]] AuthenticationMode Authentication() const noexcept;
    [[nodiscard]] AuthorizationMode Authorization() const noexcept;
    [[nodiscard]] bool EnforceAlternatorAuthorization() const noexcept;
    void Validate() const;

private:
    AuthenticationMode authentication_;
    AuthorizationMode authorization_;
    bool enforce_alternator_authorization_;
};

bool operator==(const ClusterSecuritySpec& lhs, const ClusterSecuritySpec& rhs) noexcept;
bool operator!=(const ClusterSecuritySpec& lhs, const ClusterSecuritySpec& rhs) noexcept;

class ClusterSpec final {
public:
    inline static constexpr int kMaximumNodeCount = 9;
    inline static constexpr const char* kDefaultScyllaVersion = "release:2025.2.5";

    ClusterSpec();

    [[nodiscard]] const std::string& ScyllaVersion() const noexcept;
    [[nodiscard]] const ClusterTopology& Topology() const noexcept;
    [[nodiscard]] const std::set<AlternatorTransport>& Transports() const noexcept;
    [[nodiscard]] const ClusterSecuritySpec& Security() const noexcept;
    [[nodiscard]] const NodeResources& Resources() const noexcept;
    [[nodiscard]] const std::map<std::string, std::string>& ScyllaYamlOverrides() const noexcept;

    [[nodiscard]] ClusterSpec WithScyllaVersion(std::string value) const;
    [[nodiscard]] ClusterSpec WithTopology(ClusterTopology value) const;
    [[nodiscard]] ClusterSpec WithTransports(std::vector<AlternatorTransport> values) const;
    [[nodiscard]] ClusterSpec WithTransports(std::initializer_list<AlternatorTransport> values) const;
    [[nodiscard]] ClusterSpec WithSecurity(ClusterSecuritySpec value) const;
    [[nodiscard]] ClusterSpec WithResources(NodeResources value) const;
    [[nodiscard]] ClusterSpec WithYamlOverride(std::string key, std::string yaml_value) const;

    void Validate() const;
    [[nodiscard]] std::string ReuseKey() const;

private:
    ClusterSpec(std::string scylla_version,
                ClusterTopology topology,
                std::set<AlternatorTransport> transports,
                ClusterSecuritySpec security,
                NodeResources resources,
                std::map<std::string, std::string> scylla_yaml_overrides);

    std::string scylla_version_;
    ClusterTopology topology_;
    std::set<AlternatorTransport> transports_;
    ClusterSecuritySpec security_;
    NodeResources resources_;
    std::map<std::string, std::string> scylla_yaml_overrides_;
};

bool operator==(const ClusterSpec& lhs, const ClusterSpec& rhs) noexcept;
bool operator!=(const ClusterSpec& lhs, const ClusterSpec& rhs) noexcept;

class ClusterSpecs final {
public:
    [[nodiscard]] static ClusterSpec DefaultSpec();

private:
    ClusterSpecs() = delete;
};

} // namespace scylladb::alternator::testinfra
