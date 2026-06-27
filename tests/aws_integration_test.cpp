#include <scylladb/alternator/aws/dynamodb_helper.h>

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

#include <cstdlib>
#include <cstdint>
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

bool IntegrationEnabled() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION");
    return value != nullptr && std::string(value) == "1";
}

std::vector<std::string> IntegrationNodes() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_NODE");
    return {value != nullptr && std::string(value).size() > 0 ? std::string(value) : "172.41.0.2"};
}

std::uint16_t IntegrationHttpPort() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_HTTP_PORT");
    return static_cast<std::uint16_t>(value != nullptr ? std::stoi(value) : 9998);
}

std::uint16_t IntegrationHttpsPort() {
    const char* value = std::getenv("ALTERNATOR_CLIENT_CPP_HTTPS_PORT");
    return static_cast<std::uint16_t>(value != nullptr ? std::stoi(value) : 9999);
}

Config IntegrationConfig(std::uint16_t port, std::string scheme = "http") {
    Config cfg;
    cfg.port = port;
    cfg.scheme = std::move(scheme);
    cfg.aws_region = "us-east-1";
    cfg.credentials = {"whatever", "secret"};
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.disabled = true;
    cfg.http_client_timeout = std::chrono::milliseconds{5000};
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    return cfg;
}

void DeleteTableIfExists(Aws::DynamoDB::DynamoDBClient& client, const char* table_name) {
    Aws::DynamoDB::Model::DeleteTableRequest request;
    request.SetTableName(table_name);
    (void)client.DeleteTable(request);
}

void CreateTable(Aws::DynamoDB::DynamoDBClient& client, const char* table_name) {
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

void PutGetDeleteItem(Aws::DynamoDB::DynamoDBClient& client, const char* table_name) {
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

void RunDynamoDBOperations(Config cfg, const char* table_name) {
    Aws::SDKOptions sdk_options;
    aws::DynamoDBHelper helper(IntegrationNodes(), cfg);
    helper.ApplyToSDKOptions(sdk_options);
    AwsApiGuard api(sdk_options);

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
        if (!IntegrationEnabled()) {                                                             \
            GTEST_SKIP() << "set ALTERNATOR_CLIENT_CPP_RUN_INTEGRATION=1 to run live Alternator integration tests"; \
        }                                                                                        \
    } while (false)

TEST(AwsDynamoDBIntegration, DynamoDBOperationsHttp) {
    REQUIRE_INTEGRATION();
    RunDynamoDBOperations(IntegrationConfig(IntegrationHttpPort()), "cpp_integration_http");
}

TEST(AwsDynamoDBIntegration, DynamoDBOperationsHttpsWithoutCertificateVerification) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpsPort(), "https");
    cfg.verify_ssl = false;
    RunDynamoDBOperations(std::move(cfg), "cpp_integration_https_noverify");
}

TEST(AwsDynamoDBIntegration, HttpsDiscoveryTrustsConfiguredCAFile) {
    REQUIRE_INTEGRATION();

    const char* ca_file = std::getenv("ALTERNATOR_CLIENT_CPP_CA_FILE");
    if (ca_file == nullptr || std::string(ca_file).empty()) {
        GTEST_SKIP() << "set ALTERNATOR_CLIENT_CPP_CA_FILE to test trusted HTTPS discovery";
    }

    auto cfg = IntegrationConfig(IntegrationHttpsPort(), "https");
    cfg.ca_file = ca_file;

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_FALSE(nodes.GetNodes().empty());
}

TEST(AwsDynamoDBIntegration, HttpsDiscoveryRejectsUntrustedCertificate) {
    REQUIRE_INTEGRATION();

    auto cfg = IntegrationConfig(IntegrationHttpsPort(), "https");
    cfg.verify_ssl = true;

    AlternatorLiveNodes nodes(IntegrationNodes(), cfg);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
}
