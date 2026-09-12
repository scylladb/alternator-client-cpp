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
#include "testinfra/test_cluster.h"

#include <scylladb/alternator/aws/dynamodb_helper.h>
#include <scylladb/alternator/http_client.h>

#include <aws/core/Aws.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/dynamodb/model/AttributeDefinition.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/KeySchemaElement.h>
#include <aws/dynamodb/model/ProvisionedThroughput.h>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace scylladb::alternator;
using namespace scylladb::alternator::testinfra;

namespace {

class AwsApiGuard {
public:
    explicit AwsApiGuard(Aws::SDKOptions& options)
        : options_(options) {
        Aws::InitAPI(options_);
    }

    ~AwsApiGuard() {
        Aws::ShutdownAPI(options_);
    }

    AwsApiGuard(const AwsApiGuard&) = delete;
    AwsApiGuard& operator=(const AwsApiGuard&) = delete;

private:
    Aws::SDKOptions& options_;
};

std::vector<std::string> NodeAddresses(const TestClusterInfo& cluster) {
    std::vector<std::string> addresses;
    for (const auto& node : cluster.Nodes()) {
        addresses.push_back(node.address);
    }
    return addresses;
}

void CreateTable(Aws::DynamoDB::DynamoDBClient& client, const std::string& table_name) {
    Aws::DynamoDB::Model::AttributeDefinition attribute;
    attribute.SetAttributeName("pk");
    attribute.SetAttributeType(Aws::DynamoDB::Model::ScalarAttributeType::S);

    Aws::DynamoDB::Model::KeySchemaElement key;
    key.SetAttributeName("pk");
    key.SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);

    Aws::DynamoDB::Model::ProvisionedThroughput throughput;
    throughput.SetReadCapacityUnits(1);
    throughput.SetWriteCapacityUnits(1);

    Aws::DynamoDB::Model::CreateTableRequest request;
    request.SetTableName(table_name);
    request.AddAttributeDefinitions(std::move(attribute));
    request.AddKeySchema(std::move(key));
    request.SetProvisionedThroughput(std::move(throughput));
    const auto outcome = client.CreateTable(request);
    ASSERT_TRUE(outcome.IsSuccess()) << outcome.GetError().GetMessage();
}

bool TableExists(Aws::DynamoDB::DynamoDBClient& client, const std::string& table_name) {
    Aws::DynamoDB::Model::DescribeTableRequest request;
    request.SetTableName(table_name);
    return client.DescribeTable(request).IsSuccess();
}

bool CanConnect(const Url& endpoint) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed while checking CCM endpoint");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    const int parsed = inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr);
    if (parsed != 1) {
        close(fd);
        throw std::runtime_error("invalid CCM IPv4 endpoint " + endpoint.host);
    }
    const bool connected =
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    close(fd);
    return connected;
}

void AwaitEndpointClosed(const Url& endpoint) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!CanConnect(endpoint)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("CCM endpoint remained reachable after stop: " + endpoint.ToString());
}

void ExpectHttpsWorks(const Url& endpoint, const std::filesystem::path& ca_certificate) {
    Config config;
    config.scheme = "https";
    config.port = endpoint.port;
    config.ca_file = ca_certificate.string();
    config.connect_timeout = std::chrono::seconds(1);
    config.http_client_timeout = std::chrono::seconds(2);
    const auto response = NewDefaultHttpClient(config)->Get(endpoint);
    EXPECT_GE(response.status_code, 200);
    EXPECT_LT(response.status_code, 300);
}

std::filesystem::path DiagnosticsRoot() {
    const char* configured = std::getenv("SCYLLA_CCM_DIAGNOSTICS_DIR");
    return configured == nullptr || *configured == '\0'
               ? std::filesystem::absolute("build/ccm")
               : std::filesystem::path(configured);
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

#define REQUIRE_CCM_INTEGRATION()                                                                \
    do {                                                                                         \
        if (!scylladb::alternator::testing::IntegrationEnabled()) {                              \
            GTEST_SKIP() << "set ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 to run CCM tests";    \
        }                                                                                        \
    } while (false)

TEST(CcmProvisioningIntegration, SameSpecReusesClusterWithIndependentResourceScopes) {
    REQUIRE_CCM_INTEGRATION();

    auto first = TestClusters::AcquireReusable(ClusterSpecs::DefaultSpec());
    auto second = TestClusters::AcquireReusable(ClusterSpecs::DefaultSpec());
    EXPECT_EQ(first.Cluster().InstanceId(), second.Cluster().InstanceId());
    const auto first_table = first.Resources().NewTableName("first");
    const auto second_table = second.Resources().NewTableName("second");

    Aws::SDKOptions options;
    std::unique_ptr<AwsApiGuard> api;
    aws::DynamoDBHelper helper(
        NodeAddresses(second.Cluster()),
        second.Cluster().ClientConfig(AlternatorTransport::Http));
    helper.ApplyToSDKOptions(options);
    api = std::make_unique<AwsApiGuard>(options);
    helper.UpdateLiveNodes();
    auto client = helper.NewDynamoDB();
    CreateTable(*client, first_table);
    CreateTable(*client, second_table);
    ASSERT_TRUE(TableExists(*client, first_table));
    ASSERT_TRUE(TableExists(*client, second_table));

    first.Close();
    EXPECT_FALSE(TableExists(*client, first_table));
    EXPECT_TRUE(TableExists(*client, second_table));
    second.Close();
}

TEST(CcmProvisioningIntegration, AuthorizedClusterProvidesWorkingCredentials) {
    REQUIRE_CCM_INTEGRATION();

    auto lease = TestClusters::AcquireReusable(
        ClusterSpecs::DefaultSpec()
            .WithTopology(ClusterTopology::SingleDatacenter(1))
            .WithTransports({AlternatorTransport::Http})
            .WithSecurity(ClusterSecuritySpec::Enforced()));

    auto authorized_config = lease.Cluster().ClientConfig(AlternatorTransport::Http);
    auto unauthorized_config = authorized_config;
    unauthorized_config.credentials = {"wrong-user", "wrong-password"};
    Aws::SDKOptions options;
    std::unique_ptr<AwsApiGuard> api;
    auto authorized_helper = std::make_unique<aws::DynamoDBHelper>(
        NodeAddresses(lease.Cluster()), authorized_config);
    auto unauthorized_helper = std::make_unique<aws::DynamoDBHelper>(
        NodeAddresses(lease.Cluster()), unauthorized_config);
    api = std::make_unique<AwsApiGuard>(options);
    authorized_helper->UpdateLiveNodes();
    unauthorized_helper->UpdateLiveNodes();
    {
        auto authorized_client_config = authorized_helper->NewClientConfiguration();
        authorized_client_config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(
            "CcmAuthorizedRetryStrategy", 0, 0);
        authorized_client_config.version = Aws::Http::Version::HTTP_VERSION_1_1;
        auto unauthorized_client_config = unauthorized_helper->NewClientConfiguration();
        unauthorized_client_config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(
            "CcmUnauthorizedRetryStrategy", 0, 0);
        unauthorized_client_config.version = Aws::Http::Version::HTTP_VERSION_1_1;
        Aws::Auth::AWSCredentials authorized_credentials(
            authorized_config.credentials.access_key_id.c_str(),
            authorized_config.credentials.secret_access_key.c_str());
        Aws::Auth::AWSCredentials unauthorized_credentials(
            unauthorized_config.credentials.access_key_id.c_str(),
            unauthorized_config.credentials.secret_access_key.c_str());
        Aws::DynamoDB::DynamoDBClient authorized(
            authorized_credentials,
            authorized_helper->NewEndpointProvider(),
            authorized_client_config);
        Aws::DynamoDB::DynamoDBClient unauthorized(
            unauthorized_credentials,
            unauthorized_helper->NewEndpointProvider(),
            unauthorized_client_config);

        EXPECT_FALSE(unauthorized.ListTables().IsSuccess());
        EXPECT_TRUE(authorized.ListTables().IsSuccess());
    }
    lease.Close();
}

TEST(CcmProvisioningIntegration, PrivateHttpsClusterCanChangeNodeLifecycleAndTopology) {
    REQUIRE_CCM_INTEGRATION();

    auto lease = TestClusters::ProvisionPrivate(
        ClusterSpecs::DefaultSpec()
            .WithTopology(ClusterTopology::SingleDatacenter(1))
            .WithTransports({AlternatorTransport::Https}));
    const auto original = lease.Cluster().Nodes().at(0);
    const auto added = lease.Control().AddNode("dc1", "RAC1");
    ASSERT_EQ(lease.Cluster().Nodes().size(), 2U);
    const auto connection = lease.Cluster().Connection(AlternatorTransport::Https);
    const auto added_endpoint = Url::FromHostPort("https", added.address, 8043);
    ExpectHttpsWorks(added_endpoint, connection.ca_certificate_path);

    lease.Control().Stop();
    AwaitEndpointClosed(connection.seed_endpoint);
    AwaitEndpointClosed(added_endpoint);
    lease.Control().Start();
    ExpectHttpsWorks(connection.seed_endpoint, connection.ca_certificate_path);
    ExpectHttpsWorks(added_endpoint, connection.ca_certificate_path);

    lease.Control().StopNode(added);
    AwaitEndpointClosed(added_endpoint);
    lease.Control().StartNode(added);
    ExpectHttpsWorks(added_endpoint, connection.ca_certificate_path);
    lease.Control().RemoveNode(original);
    ASSERT_EQ(lease.Cluster().Nodes().size(), 1U);

    const auto replacement = lease.Control().AddNode("dc1", "RAC1");
    EXPECT_EQ(replacement.address, original.address);
    ExpectHttpsWorks(
        Url::FromHostPort("https", replacement.address, 8043),
        connection.ca_certificate_path);
    lease.Control().RemoveNode(replacement);
    lease.Close();
}

TEST(CcmProvisioningIntegration, YamlOverridesSurviveClusterAndNodeUpdates) {
    REQUIRE_CCM_INTEGRATION();

    std::string instance_id;
    {
        auto lease = TestClusters::ProvisionPrivate(
            ClusterSpecs::DefaultSpec()
                .WithTopology(ClusterTopology::SingleDatacenter(1))
                .WithTransports({AlternatorTransport::Https})
                .WithYamlOverride("hinted_handoff_enabled", "false")
                .WithYamlOverride("commitlog_sync", "batch")
                .WithYamlOverride("commitlog_sync_batch_window_in_ms", "17")
                .WithYamlOverride("commitlog_sync_period_in_ms", "null")
                .WithYamlOverride("experimental_features", "null")
                .WithYamlOverride("client_encryption_options", "{enabled: false}")
                .WithYamlOverride("client_encryption_options.require_client_auth", "false"));
        instance_id = lease.Cluster().InstanceId();
        lease.Control().AddNode("dc1", "RAC1");
        ASSERT_EQ(lease.Cluster().Nodes().size(), 2U);
        lease.Close();
    }

    for (const auto& node : {"node1", "node2"}) {
        const auto config = DiagnosticsRoot() / instance_id / instance_id / node / "conf" /
                            "scylla.yaml";
        const auto yaml = ReadFile(config);
        const auto parsed = YAML::Load(yaml);
        EXPECT_FALSE(parsed["hinted_handoff_enabled"].as<bool>());
        EXPECT_EQ(parsed["commitlog_sync"].as<std::string>(), "batch");
        EXPECT_EQ(parsed["commitlog_sync_batch_window_in_ms"].as<int>(), 17);
        EXPECT_TRUE(parsed["commitlog_sync_period_in_ms"].IsNull());
        EXPECT_TRUE(parsed["experimental_features"].IsNull());
        ASSERT_TRUE(parsed["client_encryption_options"].IsMap());
        EXPECT_FALSE(parsed["client_encryption_options"]["enabled"].as<bool>());
        EXPECT_FALSE(
            parsed["client_encryption_options"]["require_client_auth"].as<bool>());
    }
}
