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

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

struct SequencedHttpResponse {
    int status_code = 200;
    std::string reason = "OK";
    std::string body;
    std::chrono::milliseconds delay{0};
};

class KeepAliveSequenceHttpServer {
public:
    explicit KeepAliveSequenceHttpServer(std::vector<SequencedHttpResponse> responses)
        : responses_(std::move(responses)) {
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
        if (listen(fd_, static_cast<int>(responses_.size())) != 0) {
            throw std::runtime_error("listen failed");
        }

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);

        worker_ = std::thread([this] {
            while (request_count_.load() < static_cast<int>(responses_.size())) {
                int client = accept(fd_, nullptr, nullptr);
                if (client < 0) {
                    return;
                }
                ++accept_count_;

                while (request_count_.load() < static_cast<int>(responses_.size())) {
                    auto request = ReadRequest(client);
                    if (request.empty()) {
                        break;
                    }
                    requests_.push_back(std::move(request));
                    const auto response_index = request_count_.fetch_add(1);
                    const bool keep_connection = request_count_.load() < static_cast<int>(responses_.size());
                    SendResponse(
                        client,
                        responses_.at(static_cast<std::size_t>(response_index)),
                        keep_connection);
                    if (!keep_connection) {
                        break;
                    }
                }

                close(client);
            }
        });
    }

    ~KeepAliveSequenceHttpServer() {
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

    [[nodiscard]] int AcceptCount() const {
        return accept_count_.load();
    }

    [[nodiscard]] const std::vector<std::string>& Requests() const {
        return requests_;
    }

private:
    static bool EqualsCaseInsensitive(const std::string& lhs, const std::string& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (std::size_t i = 0; i < lhs.size(); ++i) {
            const auto left = static_cast<unsigned char>(lhs[i]);
            const auto right = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(left) != std::tolower(right)) {
                return false;
            }
        }

        return true;
    }

    static std::size_t ContentLength(const std::string& request, std::size_t header_end) {
        std::istringstream lines(request.substr(0, header_end));
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            if (EqualsCaseInsensitive(line.substr(0, colon), "Content-Length")) {
                return static_cast<std::size_t>(std::stoul(line.substr(colon + 1)));
            }
        }

        return 0;
    }

    static std::string ReadRequest(int client) {
        std::string request;
        char buffer[4096];
        auto header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return {};
            }
            request.append(buffer, static_cast<std::size_t>(n));
            header_end = request.find("\r\n\r\n");
        }

        header_end += 4;
        const auto content_length = ContentLength(request, header_end);
        while (request.size() < header_end + content_length) {
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                return {};
            }
            request.append(buffer, static_cast<std::size_t>(n));
        }

        return request;
    }

    static void SendResponse(int client, const SequencedHttpResponse& response, bool keep_connection) {
        if (response.delay > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(response.delay);
        }
        std::ostringstream text;
        text << "HTTP/1.1 " << response.status_code << " " << response.reason << "\r\n"
             << "Content-Type: application/x-amz-json-1.0\r\n"
             << "Content-Length: " << response.body.size() << "\r\n"
             << "Connection: " << (keep_connection ? "keep-alive" : "close") << "\r\n"
             << "\r\n"
             << response.body;
        const auto response_text = text.str();
        send(client, response_text.data(), response_text.size(), 0);
    }

    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::vector<SequencedHttpResponse> responses_;
    std::vector<std::string> requests_;
    std::atomic<int> accept_count_{0};
    std::atomic<int> request_count_{0};
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

std::string DescribeTableResponse(const std::string& table_name,
                                  const std::string& partition_key_name) {
    return R"({"Table":{"TableName":")" + table_name + R"(","KeySchema":[{"AttributeName":")" +
           partition_key_name + R"(","KeyType":"HASH"},{"AttributeName":"sk","KeyType":"RANGE"}]}})";
}

std::string ResourceNotFoundResponse() {
    return R"({"__type":"com.amazonaws.dynamodb.v20120810#ResourceNotFoundException","message":"not ready"})";
}

bool WaitForPartitionKeyName(const aws::DynamoDBHelper& helper,
                             const std::string& table_name,
                             const std::string& partition_key_name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (helper.GetPartitionKeyName(table_name) == partition_key_name) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

Config DiscoveryTestConfig(std::uint16_t port) {
    Config cfg;
    cfg.port = port;
    cfg.scheme = "http";
    cfg.aws_region = "us-east-1";
    cfg.credentials = {"alternator", "secret"};
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};
    cfg.http_client_timeout = std::chrono::milliseconds{2000};
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    return cfg;
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

TEST(AwsDynamoDBHelper, AutoDiscoversPartitionKeyNameWithDescribeTable) {
    KeepAliveSequenceHttpServer server({
        {200, "OK", DescribeTableResponse("orders", "id")},
    });

    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    auto cfg = DiscoveryTestConfig(server.Port());
    aws::DynamoDBHelper helper({"127.0.0.1"}, cfg);

    EXPECT_EQ(helper.GetPartitionKeyName("orders"), "");

    auto plan = helper.NewPartitionKeyQueryPlan({{"id", AttributeValue::String("order-123")}}, "orders");
    EXPECT_FALSE(plan.Next().Empty());

    EXPECT_TRUE(WaitForPartitionKeyName(helper, "orders", "id"));
    server.Wait();

    ASSERT_EQ(server.Requests().size(), 1U);
    EXPECT_NE(server.Requests()[0].find("DescribeTable"), std::string::npos);
}

TEST(AwsDynamoDBHelper, UpdatesPartitionKeyNameOnDemand) {
    KeepAliveSequenceHttpServer server({
        {200, "OK", DescribeTableResponse("orders", "id")},
    });

    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    auto cfg = DiscoveryTestConfig(server.Port());
    aws::DynamoDBHelper helper({"127.0.0.1"}, cfg);

    EXPECT_TRUE(helper.UpdatePartitionKeyName("orders"));
    EXPECT_EQ(helper.GetPartitionKeyName("orders"), "id");
    server.Wait();

    ASSERT_EQ(server.Requests().size(), 1U);
    EXPECT_NE(server.Requests()[0].find("DescribeTable"), std::string::npos);
}

TEST(AwsDynamoDBHelper, RetriesPartitionKeyDiscoveryAfterTransientFailure) {
    KeepAliveSequenceHttpServer server({
        {400, "Bad Request", ResourceNotFoundResponse()},
        {200, "OK", DescribeTableResponse("orders", "id")},
    });

    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    auto cfg = DiscoveryTestConfig(server.Port());
    cfg.key_route_affinity.partition_key_discovery_attempts = 2;
    cfg.key_route_affinity.partition_key_discovery_initial_backoff = std::chrono::milliseconds{0};
    aws::DynamoDBHelper helper({"127.0.0.1"}, cfg);

    EXPECT_TRUE(helper.UpdatePartitionKeyName("orders"));
    EXPECT_EQ(helper.GetPartitionKeyName("orders"), "id");
    server.Wait();

    ASSERT_EQ(server.Requests().size(), 2U);
    EXPECT_NE(server.Requests()[0].find("DescribeTable"), std::string::npos);
    EXPECT_NE(server.Requests()[1].find("DescribeTable"), std::string::npos);
}

TEST(AwsDynamoDBHelper, SuppressesDuplicatePartitionKeyDiscoveryForTable) {
    KeepAliveSequenceHttpServer server({
        {200, "OK", DescribeTableResponse("orders", "id"), std::chrono::milliseconds{200}},
    });

    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    auto cfg = DiscoveryTestConfig(server.Port());
    aws::DynamoDBHelper helper({"127.0.0.1"}, cfg);

    for (int i = 0; i < 3; ++i) {
        auto plan = helper.NewPartitionKeyQueryPlan({{"id", AttributeValue::String("order-123")}}, "orders");
        EXPECT_FALSE(plan.Next().Empty());
    }

    EXPECT_TRUE(WaitForPartitionKeyName(helper, "orders", "id"));
    server.Wait();

    EXPECT_EQ(server.Requests().size(), 1U);
}

TEST(AwsDynamoDBHelper, BatchWriteTriggersPartitionKeyDiscovery) {
    KeepAliveSequenceHttpServer server({
        {200, "OK", DescribeTableResponse("orders", "id")},
    });

    Aws::SDKOptions sdk_options;
    AwsApiGuard api(sdk_options);

    auto cfg = DiscoveryTestConfig(server.Port());
    aws::DynamoDBHelper helper({"127.0.0.1"}, cfg);

    auto plan = helper.NewBatchWriteQueryPlan({
        BatchWriteOperation::Put("orders", {{"id", AttributeValue::String("order-123")}}),
    });
    EXPECT_FALSE(plan.Next().Empty());

    EXPECT_TRUE(WaitForPartitionKeyName(helper, "orders", "id"));
    server.Wait();

    ASSERT_EQ(server.Requests().size(), 1U);
    EXPECT_NE(server.Requests()[0].find("DescribeTable"), std::string::npos);
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

TEST(AwsDynamoDBHelper, ReusesConnectionAfterRepeatedNonSuccessDynamoDbResponses) {
    KeepAliveSequenceHttpServer server({
        {400, "Bad Request", R"({"__type":"ValidationException","message":"bad"})"},
        {400, "Bad Request", R"({"__type":"ValidationException","message":"bad again"})"},
        {200, "OK", R"({"TableNames":[]})"},
    });

    Config cfg;
    cfg.port = server.Port();
    cfg.scheme = "http";
    cfg.aws_region = "us-east-1";
    cfg.credentials = {"alternator", "secret"};
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.max_connections = 1;
    cfg.http_client_timeout = std::chrono::milliseconds{2000};
    cfg.connect_timeout = std::chrono::milliseconds{1000};

    aws::DynamoDBHelper helper({"127.0.0.1"}, cfg);

    Aws::SDKOptions sdk_options;
    helper.ApplyToSDKOptions(sdk_options);
    AwsApiGuard api(sdk_options);

    auto client_config = helper.NewClientConfiguration();
    client_config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(
        "AlternatorClientCppKeepAliveTestRetryStrategy",
        0,
        0);
    client_config.version = Aws::Http::Version::HTTP_VERSION_1_1;

    Aws::Auth::AWSCredentials credentials("alternator", "secret");
    Aws::DynamoDB::DynamoDBClient client(credentials, helper.NewEndpointProvider(), client_config);

    auto list_tables = [&client] {
        Aws::DynamoDB::Model::ListTablesRequest request;
        return client.ListTables(request);
    };
    EXPECT_FALSE(list_tables().IsSuccess());
    EXPECT_FALSE(list_tables().IsSuccess());
    EXPECT_TRUE(list_tables().IsSuccess());

    server.Wait();

    EXPECT_EQ(server.Requests().size(), 3U);
    EXPECT_EQ(server.AcceptCount(), 1);
}
