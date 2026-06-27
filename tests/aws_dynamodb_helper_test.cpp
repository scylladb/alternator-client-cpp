#include <scylladb/alternator/aws/dynamodb_helper.h>

#include <aws/core/Aws.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/dynamodb/model/ListTablesRequest.h>
#include <aws/dynamodb/model/PutItemRequest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>
#include <thread>

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

class RetryingHttpServer {
public:
    explicit RetryingHttpServer(int attempts)
        : attempts_(attempts) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket failed");
        }

        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind failed");
        }
        if (listen(fd_, attempts_) != 0) {
            throw std::runtime_error("listen failed");
        }

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);

        worker_ = std::thread([this] {
            for (int i = 0; i < attempts_; ++i) {
                int client = accept(fd_, nullptr, nullptr);
                if (client < 0) {
                    return;
                }

                char buffer[4096];
                const auto n = recv(client, buffer, sizeof(buffer), 0);
                if (n > 0) {
                    requests_.emplace_back(buffer, static_cast<std::size_t>(n));
                }

                const std::string body = R"({"__type":"InternalServerError","message":"boom"})";
                const std::string response =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/x-amz-json-1.0\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Connection: close\r\n"
                    "\r\n" +
                    body;
                send(client, response.data(), response.size(), 0);
                close(client);
            }
        });
    }

    ~RetryingHttpServer() {
        if (fd_ >= 0) {
            close(fd_);
        }
        Wait();
    }

    void Wait() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

    [[nodiscard]] const std::vector<std::string>& Requests() const {
        return requests_;
    }

private:
    int fd_ = -1;
    int attempts_ = 0;
    std::uint16_t port_ = 0;
    std::vector<std::string> requests_;
    std::thread worker_;
};

std::string HostHeader(const std::string& request) {
    const std::string key = "\r\nHost: ";
    auto start = request.find(key);
    if (start == std::string::npos) {
        return {};
    }
    start += key.size();
    const auto end = request.find("\r\n", start);
    if (end == std::string::npos) {
        return {};
    }
    return request.substr(start, end - start);
}

} // namespace

TEST(AwsDynamoDBHelper, ConfiguresClientAndInstantiatesRequestHooks) {
    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    Config cfg;
    cfg.port = 8043;
    cfg.scheme = "http";
    cfg.aws_region = "us-east-1";
    cfg.credentials = {"alternator", "secret"};
    cfg.user_agent = "test-client/1.0";
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.key_route_affinity.partition_key_by_table = {{"orders", "id"}};

    aws::DynamoDBHelper helper({"node1.example.com", "node2.example.com"}, cfg);

    auto client_config = helper.NewClientConfiguration();
    EXPECT_EQ(client_config.region, "us-east-1");
    EXPECT_EQ(client_config.endpointOverride, "http://dynamodb.fake.alternator.cluster.node:8043");
    EXPECT_EQ(client_config.userAgent, "test-client/1.0");

    EXPECT_EQ(helper.GetPartitionKeyName("orders"), "id");
    EXPECT_TRUE(helper.ShouldUseKeyRouteAffinityForWrite(WriteOperationKind::PutItem, {}));

    auto plan = helper.NewPartitionKeyQueryPlan({{"id", AttributeValue::String("order-123")}}, "orders");
    EXPECT_FALSE(plan.Next().Empty());

    Aws::DynamoDB::Model::PutItemRequest request;
    helper.AttachNodeErrorReporter(request);
    EXPECT_TRUE(static_cast<bool>(request.GetRequestSignedHandler()));
    EXPECT_TRUE(static_cast<bool>(request.GetRequestRetryHandler()));
}

TEST(AwsDynamoDBHelper, PropagatesTransportConfigurationToAwsClientConfig) {
    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    Config cfg;
    cfg.port = 9999;
    cfg.scheme = "https";
    cfg.aws_region = "us-west-2";
    cfg.verify_ssl = false;
    cfg.ca_file = "/tmp/test-ca.pem";
    cfg.max_connections = 17;
    cfg.connect_timeout = std::chrono::milliseconds{1234};
    cfg.http_client_timeout = std::chrono::milliseconds{5678};
    cfg.user_agent = "transport-test/1.0";

    aws::DynamoDBHelper helper({"node1.example.com"}, cfg);

    auto client_config = helper.NewClientConfiguration();
    EXPECT_EQ(client_config.region, "us-west-2");
    EXPECT_EQ(client_config.endpointOverride, "https://dynamodb.fake.alternator.cluster.node:9999");
    EXPECT_FALSE(client_config.verifySSL);
    EXPECT_EQ(client_config.caFile, "/tmp/test-ca.pem");
    EXPECT_EQ(client_config.maxConnections, 17U);
    EXPECT_EQ(client_config.connectTimeoutMs, 1234);
    EXPECT_EQ(client_config.httpRequestTimeoutMs, 5678);
    EXPECT_EQ(client_config.requestTimeoutMs, 5678);
    EXPECT_EQ(client_config.userAgent, "transport-test/1.0");
}

TEST(AwsDynamoDBHelper, DefaultsToConfiguredSdkConnectionPoolLimit) {
    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    Config cfg;
    aws::DynamoDBHelper helper({"node1.example.com"}, cfg);

    auto client_config = helper.NewClientConfiguration();
    EXPECT_EQ(client_config.maxConnections, cfg.max_connections);
    EXPECT_EQ(client_config.maxConnections, 100U);
}

TEST(AwsDynamoDBHelper, HttpClientFactoryRotatesNodesAcrossRetries) {
    RetryingHttpServer server(2);

    Config cfg;
    cfg.port = server.Port();
    cfg.scheme = "http";
    cfg.aws_region = "us-east-1";
    cfg.credentials = {"alternator", "secret"};
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.http_client_timeout = std::chrono::milliseconds{2000};
    cfg.connect_timeout = std::chrono::milliseconds{1000};

    aws::DynamoDBHelper helper({"127.0.0.1", "localhost"}, cfg);

    Aws::SDKOptions sdk_options;
    helper.ApplyToSDKOptions(sdk_options);
    AwsApiGuard api(sdk_options);

    auto client_config = helper.NewClientConfiguration();
    client_config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(
        "AlternatorClientCppTestRetryStrategy",
        1,
        0);
    client_config.version = Aws::Http::Version::HTTP_VERSION_1_1;

    Aws::Auth::AWSCredentials credentials("alternator", "secret");
    Aws::DynamoDB::DynamoDBClient client(credentials, helper.NewEndpointProvider(), client_config);

    Aws::DynamoDB::Model::ListTablesRequest request;
    auto outcome = client.ListTables(request);
    EXPECT_FALSE(outcome.IsSuccess());

    server.Wait();

    ASSERT_EQ(server.Requests().size(), 2U);
    const auto first_host = HostHeader(server.Requests()[0]);
    const auto second_host = HostHeader(server.Requests()[1]);
    EXPECT_NE(first_host, "");
    EXPECT_NE(second_host, "");
    EXPECT_NE(first_host, second_host);
}
