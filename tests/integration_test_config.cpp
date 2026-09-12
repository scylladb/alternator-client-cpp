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

#include "integration_test_config.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace scylladb::alternator::testing {
namespace {

class IntegrationContext {
public:
    testinfra::ReusableClusterLease& Lease() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lease_ == nullptr) {
            auto spec = testinfra::ClusterSpecs::DefaultSpec()
                            .WithYamlOverride("alternator_response_gzip_compression_level", "1")
                            .WithYamlOverride(
                                "alternator_response_compression_threshold_in_bytes", "1");
            lease_ = std::make_unique<testinfra::ReusableClusterLease>(
                testinfra::TestClusters::AcquireReusable(spec));
        }
        return *lease_;
    }

    void Close() {
        std::unique_ptr<testinfra::ReusableClusterLease> lease;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lease = std::move(lease_);
        }
        if (lease != nullptr) {
            lease->Close();
        }
    }

private:
    std::mutex mutex_;
    std::unique_ptr<testinfra::ReusableClusterLease> lease_;
};

IntegrationContext& Context() {
    static IntegrationContext context;
    return context;
}

} // namespace

bool IntegrationEnabled() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION");
    return value != nullptr && std::string(value) == "1";
}

const testinfra::TestClusterInfo& IntegrationCluster() {
    if (!IntegrationEnabled()) {
        throw std::logic_error(
            "set ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 before acquiring integration cluster");
    }
    return Context().Lease().Cluster();
}

std::vector<std::string> IntegrationNodes() {
    return {IntegrationCluster().Nodes().at(0).address};
}

std::string IntegrationDatacenter() {
    return IntegrationCluster().Nodes().at(0).datacenter;
}

std::string IntegrationRack() {
    return IntegrationCluster().Nodes().at(0).rack;
}

std::uint16_t IntegrationPort(testinfra::AlternatorTransport transport) {
    return IntegrationCluster().Connection(transport).seed_endpoint.port;
}

Config IntegrationConfig(testinfra::AlternatorTransport transport) {
    return IntegrationCluster().ClientConfig(transport);
}

std::filesystem::path IntegrationCaCertificate() {
    return IntegrationCluster()
        .Connection(testinfra::AlternatorTransport::Https)
        .ca_certificate_path;
}

std::string IntegrationTableName(const std::string& hint) {
    return Context().Lease().Resources().NewTableName(hint);
}

void CloseIntegrationCluster() {
    Context().Close();
}

} // namespace scylladb::alternator::testing
