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

#include <scylladb/alternator/aws/dynamodb_helper.h>

#include "integration_test_config.h"

#include <aws/core/Aws.h>
#include <aws/dynamodb/model/AttributeDefinition.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>
#include <aws/dynamodb/model/DeleteTableRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/KeySchemaElement.h>
#include <aws/dynamodb/model/ProvisionedThroughput.h>
#include <aws/dynamodb/model/PutItemRequest.h>

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace scylladb::alternator;

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

void DeleteTableIfExists(Aws::DynamoDB::DynamoDBClient& client, const std::string& table_name) {
    Aws::DynamoDB::Model::DeleteTableRequest request;
    request.SetTableName(table_name);
    (void)client.DeleteTable(request);
}

void CreateTable(Aws::DynamoDB::DynamoDBClient& client, const std::string& table_name) {
    Aws::DynamoDB::Model::AttributeDefinition attr;
    attr.SetAttributeName("ID");
    attr.SetAttributeType(Aws::DynamoDB::Model::ScalarAttributeType::S);

    Aws::DynamoDB::Model::KeySchemaElement key;
    key.SetAttributeName("ID");
    key.SetKeyType(Aws::DynamoDB::Model::KeyType::HASH);

    Aws::DynamoDB::Model::ProvisionedThroughput throughput;
    throughput.SetReadCapacityUnits(1);
    throughput.SetWriteCapacityUnits(1);

    Aws::DynamoDB::Model::CreateTableRequest request;
    request.SetTableName(table_name);
    request.AddAttributeDefinitions(std::move(attr));
    request.AddKeySchema(std::move(key));
    request.SetProvisionedThroughput(std::move(throughput));

    auto outcome = client.CreateTable(request);
    ASSERT_TRUE(outcome.IsSuccess()) << outcome.GetError().GetMessage();
}

void PutGetDeleteItem(Aws::DynamoDB::DynamoDBClient& client, const std::string& table_name) {
    Aws::DynamoDB::Model::AttributeValue id;
    id.SetS("123");
    Aws::DynamoDB::Model::AttributeValue name;
    name.SetS("value");

    Aws::DynamoDB::Model::PutItemRequest put;
    put.SetTableName(table_name);
    put.AddItem("ID", id);
    put.AddItem("Name", name);
    auto put_outcome = client.PutItem(put);
    ASSERT_TRUE(put_outcome.IsSuccess()) << put_outcome.GetError().GetMessage();

    Aws::DynamoDB::Model::GetItemRequest get;
    get.SetTableName(table_name);
    get.AddKey("ID", id);
    auto get_outcome = client.GetItem(get);
    ASSERT_TRUE(get_outcome.IsSuccess()) << get_outcome.GetError().GetMessage();
    EXPECT_FALSE(get_outcome.GetResult().GetItem().empty());

    Aws::DynamoDB::Model::DeleteItemRequest del;
    del.SetTableName(table_name);
    del.AddKey("ID", id);
    auto del_outcome = client.DeleteItem(del);
    ASSERT_TRUE(del_outcome.IsSuccess()) << del_outcome.GetError().GetMessage();
}

void RunDynamoDBOperations(Config cfg, const std::string& table_name) {
    Aws::SDKOptions sdk_options;
    std::unique_ptr<AwsApiGuard> api;
    aws::DynamoDBHelper helper(
        scylladb::alternator::testing::IntegrationNodes(), cfg);
    helper.ApplyToSDKOptions(sdk_options);
    api = std::make_unique<AwsApiGuard>(sdk_options);

    helper.UpdateLiveNodes();
    auto client = helper.NewDynamoDB();

    DeleteTableIfExists(*client, table_name);
    CreateTable(*client, table_name);
    PutGetDeleteItem(*client, table_name);
    DeleteTableIfExists(*client, table_name);
}

} // namespace

#define REQUIRE_INTEGRATION()                                                                    \
    do {                                                                                         \
        if (!scylladb::alternator::testing::IntegrationEnabled()) {                              \
            GTEST_SKIP() << "set ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 to run live Alternator integration tests"; \
        }                                                                                        \
    } while (false)

TEST(AwsDynamoDBIntegration, DynamoDBOperationsHttp) {
    REQUIRE_INTEGRATION();
    RunDynamoDBOperations(
        scylladb::alternator::testing::IntegrationConfig(testinfra::AlternatorTransport::Http),
        scylladb::alternator::testing::IntegrationTableName("http"));
}

TEST(AwsDynamoDBIntegration, DynamoDBOperationsHttpWithRequestCompressionAndHeaderOptimization) {
    REQUIRE_INTEGRATION();
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    GTEST_SKIP() << "zlib support is not enabled";
#endif

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Http);
    cfg.request_compressor = std::make_shared<GzipRequestCompressor>(0);
    cfg.content_encoding_decoders = {std::make_shared<ZlibContentEncodingDecoder>()};
    cfg.header_optimization = std::make_shared<HeaderAllowlistOptimization>(std::vector<std::string>{
        "Host",
        "X-Amz-Target",
        "Content-Length",
    });
    RunDynamoDBOperations(
        std::move(cfg),
        scylladb::alternator::testing::IntegrationTableName("http_gzip_request"));
}

TEST(AwsDynamoDBIntegration, DynamoDBOperationsHttpsWithoutCertificateVerification) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Https);
    cfg.verify_ssl = false;
    RunDynamoDBOperations(
        std::move(cfg),
        scylladb::alternator::testing::IntegrationTableName("https_noverify"));
}

TEST(AwsDynamoDBIntegration, HttpsDiscoveryTrustsConfiguredCAFile) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Https);

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_FALSE(nodes.GetNodes().empty());
}

TEST(AwsDynamoDBIntegration, HttpsDiscoveryRejectsUntrustedCertificate) {
    REQUIRE_INTEGRATION();

    auto cfg = scylladb::alternator::testing::IntegrationConfig(
        testinfra::AlternatorTransport::Https);
    cfg.verify_ssl = true;
    cfg.ca_file.clear();

    AlternatorLiveNodes nodes(scylladb::alternator::testing::IntegrationNodes(), cfg);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
}
