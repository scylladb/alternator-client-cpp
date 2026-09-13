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

#include "cluster_spec.h"

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace scylladb::alternator::testinfra {
namespace {

struct Utf8CodePoint {
    std::uint32_t value;
    std::size_t width;
};

bool IsContinuationByte(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

Utf8CodePoint DecodeUtf8(const std::string& value, std::size_t offset) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first < 0x80U) {
        return {first, 1};
    }

    std::size_t width = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
        width = 2;
        code_point = first & 0x1fU;
        minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
        width = 3;
        code_point = first & 0x0fU;
        minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
        width = 4;
        code_point = first & 0x07U;
        minimum = 0x10000U;
    } else {
        throw std::invalid_argument("text is not valid UTF-8");
    }
    if (offset + width > value.size()) {
        throw std::invalid_argument("text is not valid UTF-8");
    }
    for (std::size_t index = 1; index < width; ++index) {
        const auto byte = static_cast<unsigned char>(value[offset + index]);
        if (!IsContinuationByte(byte)) {
            throw std::invalid_argument("text is not valid UTF-8");
        }
        code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    if (code_point < minimum || code_point > 0x10ffffU ||
        (code_point >= 0xd800U && code_point <= 0xdfffU)) {
        throw std::invalid_argument("text is not valid UTF-8");
    }
    return {code_point, width};
}

bool IsUnicodeWhitespace(std::uint32_t code_point) {
    return (code_point >= 0x09U && code_point <= 0x0dU) ||
           (code_point >= 0x1cU && code_point <= 0x20U) ||
           code_point == 0x85U ||
           code_point == 0xa0U ||
           code_point == 0x1680U ||
           (code_point >= 0x2000U && code_point <= 0x200aU) ||
           code_point == 0x2028U ||
           code_point == 0x2029U ||
           code_point == 0x202fU ||
           code_point == 0x205fU ||
           code_point == 0x3000U;
}

std::string TrimUnicodeWhitespace(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto decoded = DecodeUtf8(value, begin);
        if (!IsUnicodeWhitespace(decoded.value)) {
            break;
        }
        begin += decoded.width;
    }

    std::size_t end = value.size();
    while (end > begin) {
        std::size_t start = end - 1;
        while (start > begin && IsContinuationByte(static_cast<unsigned char>(value[start]))) {
            --start;
        }
        const auto decoded = DecodeUtf8(value, start);
        if (start + decoded.width != end) {
            throw std::invalid_argument("text is not valid UTF-8");
        }
        if (!IsUnicodeWhitespace(decoded.value)) {
            break;
        }
        end = start;
    }
    return value.substr(begin, end - begin);
}

bool IsAsciiIdentifierStart(char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') ||
           character == '_';
}

bool IsAsciiIdentifierContinuation(char character) {
    return IsAsciiIdentifierStart(character) || (character >= '0' && character <= '9');
}

bool IsYamlKeyShapeValid(const std::string& key) {
    bool expecting_start = true;
    int segments = 1;
    for (char character : key) {
        if (character == '.') {
            if (expecting_start || ++segments > 2) {
                return false;
            }
            expecting_start = true;
            continue;
        }
        if (expecting_start) {
            if (!IsAsciiIdentifierStart(character)) {
                return false;
            }
            expecting_start = false;
        } else if (!IsAsciiIdentifierContinuation(character)) {
            return false;
        }
    }
    return !key.empty() && !expecting_start;
}

bool IsControlCodePoint(std::uint32_t code_point) {
    return code_point <= 0x1fU || (code_point >= 0x7fU && code_point <= 0x9fU);
}

const std::set<std::string>& ReservedYamlKeys() {
    static const std::set<std::string> keys = {
        "alternator_address",
        "alternator_encryption_options",
        "alternator_enforce_authorization",
        "alternator_https_port",
        "alternator_port",
        "alternator_write_isolation",
        "api_address",
        "api_port",
        "auth_superuser_name",
        "auth_superuser_salted_password",
        "authenticator",
        "auto_bootstrap",
        "authorizer",
        "blocked_reactor_notify_ms",
        "broadcast_address",
        "broadcast_rpc_address",
        "cluster_name",
        "commitlog_directory",
        "commitlog_use_o_dsync",
        "data_file_directories",
        "default_log_level",
        "developer_mode",
        "endpoint_snitch",
        "hints_directory",
        "ignore_dead_nodes_for_replace",
        "initial_token",
        "join_ring",
        "kernel_page_cache",
        "listen_address",
        "listen_interface",
        "listen_interface_prefer_ipv6",
        "listen_on_broadcast_address",
        "load_ring_state",
        "log_to_stdout",
        "maintenance_mode",
        "maintenance_socket",
        "maintenance_socket_group",
        "max_networking_io_control_blocks",
        "memory",
        "native_shard_aware_transport_port",
        "native_shard_aware_transport_port_proxy_protocol",
        "native_shard_aware_transport_port_ssl",
        "native_shard_aware_transport_port_ssl_proxy_protocol",
        "native_transport_port",
        "native_transport_port_ssl",
        "num_tokens",
        "overprovisioned",
        "partitioner",
        "prometheus_address",
        "prometheus_port",
        "redis_port",
        "redis_ssl_port",
        "replace_address",
        "replace_address_first_boot",
        "replace_node_first_boot",
        "role_manager",
        "rpc_address",
        "rpc_interface",
        "rpc_interface_prefer_ipv6",
        "rpc_port",
        "saved_caches_directory",
        "schema_commitlog_directory",
        "seeds",
        "seed_provider",
        "server_encryption_options",
        "smp",
        "ssl_storage_port",
        "start_native_transport",
        "storage_port",
        "unsafe_bypass_fsync",
        "view_hints_directory",
        "workdir",
    };
    return keys;
}

std::string CanonicalizeYamlKey(const std::string& key) {
    for (std::size_t offset = 0; offset < key.size();) {
        const auto decoded = DecodeUtf8(key, offset);
        if (IsControlCodePoint(decoded.value)) {
            throw std::invalid_argument("Scylla YAML override keys cannot contain controls");
        }
        offset += decoded.width;
    }

    std::string canonical = TrimUnicodeWhitespace(key);
    if (!IsYamlKeyShapeValid(canonical)) {
        throw std::invalid_argument(
            "a Scylla YAML override key must contain one or two ASCII identifier segments");
    }

    const auto separator = canonical.find('.');
    auto root = canonical.substr(0, separator);
    if (root == "cql_port") {
        root = "native_transport_port";
    } else if (root == "datadir") {
        root = "data_file_directories";
    }
    if (ReservedYamlKeys().count(root) != 0) {
        throw std::invalid_argument("Scylla YAML key '" + root + "' is owned by a typed cluster option");
    }
    return separator == std::string::npos ? root : root + canonical.substr(separator);
}

enum class YamlValueKind {
    Scalar,
    List,
    Map,
};

std::string YamlKeyIdentity(const YAML::Node& key) {
    if (key.IsScalar()) {
        return "scalar:" + key.Tag() + ':' + key.Scalar();
    }
    return "complex:" + YAML::Dump(key);
}

void ValidateYamlTree(const YAML::Node& node, std::size_t depth = 0) {
    constexpr std::size_t maximum_depth = 100;
    if (depth > maximum_depth) {
        throw std::invalid_argument("Scylla YAML override exceeds maximum nesting depth");
    }

    if (node.IsMap()) {
        std::set<std::string> keys;
        for (const auto& entry : node) {
            const auto identity = YamlKeyIdentity(entry.first);
            if (!keys.insert(identity).second) {
                throw std::invalid_argument("Scylla YAML override contains duplicate mapping keys");
            }
            ValidateYamlTree(entry.first, depth + 1);
            ValidateYamlTree(entry.second, depth + 1);
        }
    } else if (node.IsSequence()) {
        for (const auto& entry : node) {
            ValidateYamlTree(entry, depth + 1);
        }
    }
}

YamlValueKind ValidateYamlValue(const std::string& value) {
    const auto yaml = TrimUnicodeWhitespace(value);
    if (yaml.empty()) {
        throw std::invalid_argument("Scylla YAML override values cannot be empty");
    }
    if (yaml.find('\0') != std::string::npos) {
        throw std::invalid_argument("Scylla YAML override values cannot contain NUL bytes");
    }

    try {
        const auto documents = YAML::LoadAll(yaml);
        if (documents.size() != 1) {
            throw std::invalid_argument("Scylla YAML override must contain exactly one document");
        }
        const auto& parsed = documents.front();
        ValidateYamlTree(parsed);
        if (parsed.IsMap()) {
            return YamlValueKind::Map;
        }
        if (parsed.IsSequence()) {
            return YamlValueKind::List;
        }
        return YamlValueKind::Scalar;
    } catch (const YAML::Exception& error) {
        throw std::invalid_argument(
            "Scylla YAML override has an invalid YAML value: " + std::string(error.what()));
    }
}

std::map<std::string, std::string> CanonicalizeYamlOverrides(
    const std::map<std::string, std::string>& overrides) {
    std::map<std::string, std::string> canonical;
    for (const auto& option : overrides) {
        canonical[CanonicalizeYamlKey(option.first)] = option.second;
    }
    return canonical;
}

std::set<AlternatorTransport> ToTransportSet(const std::vector<AlternatorTransport>& values) {
    return {values.begin(), values.end()};
}

const char* TransportName(AlternatorTransport transport) {
    switch (transport) {
    case AlternatorTransport::Http:
        return "HTTP";
    case AlternatorTransport::Https:
        return "HTTPS";
    }
    throw std::invalid_argument("unknown Alternator transport");
}

const char* AuthenticationName(AuthenticationMode mode) {
    switch (mode) {
    case AuthenticationMode::AllowAll:
        return "ALLOW_ALL";
    case AuthenticationMode::Password:
        return "PASSWORD";
    case AuthenticationMode::Transitional:
        return "TRANSITIONAL";
    }
    throw std::invalid_argument("unknown authentication mode");
}

const char* AuthorizationName(AuthorizationMode mode) {
    switch (mode) {
    case AuthorizationMode::AllowAll:
        return "ALLOW_ALL";
    case AuthorizationMode::Cassandra:
        return "CASSANDRA";
    case AuthorizationMode::Transitional:
        return "TRANSITIONAL";
    }
    throw std::invalid_argument("unknown authorization mode");
}

void AppendLengthPrefixed(std::ostringstream& output, const std::string& value) {
    output << value.size() << ':' << value;
}

} // namespace

RackSpec::RackSpec(int node_count)
    : node_count_(node_count) {
    if (node_count_ < 1) {
        throw std::invalid_argument("a rack must contain at least one node");
    }
}

int RackSpec::NodeCount() const noexcept {
    return node_count_;
}

bool operator==(const RackSpec& lhs, const RackSpec& rhs) noexcept {
    return lhs.NodeCount() == rhs.NodeCount();
}

bool operator!=(const RackSpec& lhs, const RackSpec& rhs) noexcept {
    return !(lhs == rhs);
}

DatacenterSpec::DatacenterSpec(std::vector<RackSpec> racks)
    : racks_(std::move(racks)) {
    if (racks_.empty()) {
        throw std::invalid_argument("a datacenter must contain at least one rack");
    }
}

DatacenterSpec DatacenterSpec::Create(std::initializer_list<int> nodes_per_rack) {
    return Create(std::vector<int>(nodes_per_rack));
}

DatacenterSpec DatacenterSpec::Create(const std::vector<int>& nodes_per_rack) {
    std::vector<RackSpec> racks;
    racks.reserve(nodes_per_rack.size());
    for (const auto node_count : nodes_per_rack) {
        racks.emplace_back(node_count);
    }
    return DatacenterSpec(std::move(racks));
}

const std::vector<RackSpec>& DatacenterSpec::Racks() const noexcept {
    return racks_;
}

bool operator==(const DatacenterSpec& lhs, const DatacenterSpec& rhs) noexcept {
    return lhs.Racks() == rhs.Racks();
}

bool operator!=(const DatacenterSpec& lhs, const DatacenterSpec& rhs) noexcept {
    return !(lhs == rhs);
}

ClusterTopology::ClusterTopology(std::vector<DatacenterSpec> datacenters)
    : datacenters_(std::move(datacenters)) {
    if (datacenters_.empty()) {
        throw std::invalid_argument("a cluster must contain at least one datacenter");
    }
}

ClusterTopology ClusterTopology::SingleDatacenter(int node_count) {
    return SingleDatacenter({node_count});
}

ClusterTopology ClusterTopology::SingleDatacenter(std::initializer_list<int> nodes_per_rack) {
    return SingleDatacenter(std::vector<int>(nodes_per_rack));
}

ClusterTopology ClusterTopology::SingleDatacenter(const std::vector<int>& nodes_per_rack) {
    return ClusterTopology({DatacenterSpec::Create(nodes_per_rack)});
}

const std::vector<DatacenterSpec>& ClusterTopology::Datacenters() const noexcept {
    return datacenters_;
}

int ClusterTopology::NodeCount() const {
    int count = 0;
    for (const auto& datacenter : datacenters_) {
        for (const auto& rack : datacenter.Racks()) {
            if (rack.NodeCount() > std::numeric_limits<int>::max() - count) {
                throw std::invalid_argument("a cluster topology has too many nodes");
            }
            count += rack.NodeCount();
        }
    }
    return count;
}

bool operator==(const ClusterTopology& lhs, const ClusterTopology& rhs) noexcept {
    return lhs.Datacenters() == rhs.Datacenters();
}

bool operator!=(const ClusterTopology& lhs, const ClusterTopology& rhs) noexcept {
    return !(lhs == rhs);
}

NodeResources::NodeResources(int smp, int memory_mib)
    : smp_(smp)
    , memory_mib_(memory_mib) {
    if (smp_ < 1 || memory_mib_ < 1) {
        throw std::invalid_argument("node SMP and memory must be positive");
    }
}

int NodeResources::Smp() const noexcept {
    return smp_;
}

int NodeResources::MemoryMiB() const noexcept {
    return memory_mib_;
}

bool operator==(const NodeResources& lhs, const NodeResources& rhs) noexcept {
    return lhs.Smp() == rhs.Smp() && lhs.MemoryMiB() == rhs.MemoryMiB();
}

bool operator!=(const NodeResources& lhs, const NodeResources& rhs) noexcept {
    return !(lhs == rhs);
}

ClusterSecuritySpec::ClusterSecuritySpec(AuthenticationMode authentication,
                                         AuthorizationMode authorization,
                                         bool enforce_alternator_authorization)
    : authentication_(authentication)
    , authorization_(authorization)
    , enforce_alternator_authorization_(enforce_alternator_authorization) {
    Validate();
}

ClusterSecuritySpec ClusterSecuritySpec::Disabled() {
    return ClusterSecuritySpec(AuthenticationMode::AllowAll, AuthorizationMode::AllowAll, false);
}

ClusterSecuritySpec ClusterSecuritySpec::Enforced() {
    return ClusterSecuritySpec(AuthenticationMode::Password, AuthorizationMode::Cassandra, true);
}

AuthenticationMode ClusterSecuritySpec::Authentication() const noexcept {
    return authentication_;
}

AuthorizationMode ClusterSecuritySpec::Authorization() const noexcept {
    return authorization_;
}

bool ClusterSecuritySpec::EnforceAlternatorAuthorization() const noexcept {
    return enforce_alternator_authorization_;
}

void ClusterSecuritySpec::Validate() const {
    if (authentication_ == AuthenticationMode::AllowAll && authorization_ != AuthorizationMode::AllowAll) {
        throw std::invalid_argument("allow-all authentication can only be used with allow-all authorization");
    }
    if (enforce_alternator_authorization_ &&
        (authentication_ != AuthenticationMode::Password || authorization_ != AuthorizationMode::Cassandra)) {
        throw std::invalid_argument(
            "Alternator authorization enforcement requires password authentication and Cassandra authorization");
    }
}

bool operator==(const ClusterSecuritySpec& lhs, const ClusterSecuritySpec& rhs) noexcept {
    return lhs.Authentication() == rhs.Authentication() &&
           lhs.Authorization() == rhs.Authorization() &&
           lhs.EnforceAlternatorAuthorization() == rhs.EnforceAlternatorAuthorization();
}

bool operator!=(const ClusterSecuritySpec& lhs, const ClusterSecuritySpec& rhs) noexcept {
    return !(lhs == rhs);
}

ClusterSpec::ClusterSpec()
    : ClusterSpec(kDefaultScyllaVersion,
                  ClusterTopology::SingleDatacenter(3),
                  {AlternatorTransport::Http, AlternatorTransport::Https},
                  ClusterSecuritySpec::Disabled(),
                  NodeResources(2, 1024),
                  {}) {}

ClusterSpec::ClusterSpec(std::string scylla_version,
                         ClusterTopology topology,
                         std::set<AlternatorTransport> transports,
                         ClusterSecuritySpec security,
                         NodeResources resources,
                         std::map<std::string, std::string> scylla_yaml_overrides)
    : scylla_version_(std::move(scylla_version))
    , topology_(std::move(topology))
    , transports_(std::move(transports))
    , security_(std::move(security))
    , resources_(std::move(resources))
    , scylla_yaml_overrides_(CanonicalizeYamlOverrides(scylla_yaml_overrides)) {
    Validate();
}

const std::string& ClusterSpec::ScyllaVersion() const noexcept {
    return scylla_version_;
}

const ClusterTopology& ClusterSpec::Topology() const noexcept {
    return topology_;
}

const std::set<AlternatorTransport>& ClusterSpec::Transports() const noexcept {
    return transports_;
}

const ClusterSecuritySpec& ClusterSpec::Security() const noexcept {
    return security_;
}

const NodeResources& ClusterSpec::Resources() const noexcept {
    return resources_;
}

const std::map<std::string, std::string>& ClusterSpec::ScyllaYamlOverrides() const noexcept {
    return scylla_yaml_overrides_;
}

ClusterSpec ClusterSpec::WithScyllaVersion(std::string value) const {
    return ClusterSpec(std::move(value), topology_, transports_, security_, resources_, scylla_yaml_overrides_);
}

ClusterSpec ClusterSpec::WithTopology(ClusterTopology value) const {
    return ClusterSpec(scylla_version_, std::move(value), transports_, security_, resources_, scylla_yaml_overrides_);
}

ClusterSpec ClusterSpec::WithTransports(std::vector<AlternatorTransport> values) const {
    return ClusterSpec(
        scylla_version_, topology_, ToTransportSet(values), security_, resources_, scylla_yaml_overrides_);
}

ClusterSpec ClusterSpec::WithTransports(std::initializer_list<AlternatorTransport> values) const {
    return WithTransports(std::vector<AlternatorTransport>(values));
}

ClusterSpec ClusterSpec::WithSecurity(ClusterSecuritySpec value) const {
    return ClusterSpec(scylla_version_, topology_, transports_, std::move(value), resources_, scylla_yaml_overrides_);
}

ClusterSpec ClusterSpec::WithResources(NodeResources value) const {
    return ClusterSpec(scylla_version_, topology_, transports_, security_, std::move(value), scylla_yaml_overrides_);
}

ClusterSpec ClusterSpec::WithYamlOverride(std::string key, std::string yaml_value) const {
    auto overrides = scylla_yaml_overrides_;
    overrides[CanonicalizeYamlKey(key)] = std::move(yaml_value);
    return ClusterSpec(scylla_version_, topology_, transports_, security_, resources_, std::move(overrides));
}

void ClusterSpec::Validate() const {
    if (TrimUnicodeWhitespace(scylla_version_).empty()) {
        throw std::invalid_argument("a Scylla version is required");
    }
    if (transports_.empty()) {
        throw std::invalid_argument("at least one Alternator transport is required");
    }
    if (topology_.NodeCount() > kMaximumNodeCount) {
        throw std::invalid_argument("a cluster cannot exceed " + std::to_string(kMaximumNodeCount) + " nodes");
    }
    security_.Validate();

    std::map<std::string, YamlValueKind> kinds;
    for (const auto& option : scylla_yaml_overrides_) {
        kinds.emplace(option.first, ValidateYamlValue(option.second));
    }
    for (const auto& option : kinds) {
        const auto separator = option.first.find('.');
        if (separator == std::string::npos) {
            continue;
        }
        const auto root = option.first.substr(0, separator);
        const auto root_value = kinds.find(root);
        if (root_value != kinds.end() && root_value->second != YamlValueKind::Map) {
            throw std::invalid_argument(
                "Scylla YAML override '" + root + "' must be a mapping when overriding '" + option.first + "'");
        }
    }
}

std::string ClusterSpec::ReuseKey() const {
    std::ostringstream key;
    key << "version:";
    AppendLengthPrefixed(key, scylla_version_);
    key << "|transports:" << transports_.size();
    for (const auto transport : transports_) {
        key << ':' << TransportName(transport);
    }
    key << "|security:" << AuthenticationName(security_.Authentication())
        << ':' << AuthorizationName(security_.Authorization())
        << ':' << (security_.EnforceAlternatorAuthorization() ? '1' : '0');
    key << "|resources:" << resources_.Smp() << ':' << resources_.MemoryMiB();
    key << "|topology:" << topology_.Datacenters().size();
    for (const auto& datacenter : topology_.Datacenters()) {
        key << ":dc:" << datacenter.Racks().size();
        for (const auto& rack : datacenter.Racks()) {
            key << ':' << rack.NodeCount();
        }
    }
    key << "|yaml:" << scylla_yaml_overrides_.size();
    for (const auto& option : scylla_yaml_overrides_) {
        key << ':';
        AppendLengthPrefixed(key, option.first);
        key << '=';
        AppendLengthPrefixed(key, option.second);
    }
    return key.str();
}

bool operator==(const ClusterSpec& lhs, const ClusterSpec& rhs) noexcept {
    return lhs.ScyllaVersion() == rhs.ScyllaVersion() &&
           lhs.Topology() == rhs.Topology() &&
           lhs.Transports() == rhs.Transports() &&
           lhs.Security() == rhs.Security() &&
           lhs.Resources() == rhs.Resources() &&
           lhs.ScyllaYamlOverrides() == rhs.ScyllaYamlOverrides();
}

bool operator!=(const ClusterSpec& lhs, const ClusterSpec& rhs) noexcept {
    return !(lhs == rhs);
}

ClusterSpec ClusterSpecs::DefaultSpec() {
    const char* configured = std::getenv("SCYLLA_VERSION");
    if (configured == nullptr) {
        return ClusterSpec();
    }
    return ClusterSpec().WithScyllaVersion(configured);
}

} // namespace scylladb::alternator::testinfra
