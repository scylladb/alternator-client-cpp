#include <scylladb/alternator/aws/dynamodb_helper.h>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/http/curl/CurlHttpClient.h>
#include <aws/core/http/standard/StandardHttpRequest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace scylladb::alternator::aws {
namespace {

constexpr char kAllocationTag[] = "ScyllaDBAlternatorAwsAdapter";
constexpr char kEndpointOverrideHost[] = "dynamodb.fake.alternator.cluster.node";

Aws::Http::Scheme AwsScheme(const std::string& scheme) {
    return scheme == "https" ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
}

std::string BracketIPv6Host(const std::string& host) {
    if (host.find(':') != std::string::npos && (host.empty() || host.front() != '[')) {
        return "[" + host + "]";
    }
    return host;
}

std::string UnbracketHost(std::string host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host.erase(host.begin());
        host.pop_back();
    }
    return host;
}

bool SameEndpointHost(const std::string& lhs, const std::string& rhs) {
    return UnbracketHost(lhs) == UnbracketHost(rhs);
}

bool IsAlternatorEndpoint(const Aws::Http::URI& uri,
                          const AlternatorLiveNodes& nodes,
                          std::uint16_t endpoint_override_port) {
    const auto host = std::string(uri.GetHost().c_str());
    const auto port = uri.GetPort();
    if (SameEndpointHost(host, kEndpointOverrideHost) && port == endpoint_override_port) {
        return true;
    }

    const auto known_nodes = nodes.GetNodes();
    return std::any_of(known_nodes.begin(), known_nodes.end(), [&](const Url& node) {
        return SameEndpointHost(host, node.host) && port == node.port;
    });
}

Url UrlFromAwsUri(const Aws::Http::URI& uri) {
    const auto scheme = uri.GetScheme() == Aws::Http::Scheme::HTTPS ? std::string("https") : std::string("http");
    return Url(scheme, std::string(uri.GetHost().c_str()), uri.GetPort());
}

void RewriteRequestEndpoint(const std::shared_ptr<Aws::Http::HttpRequest>& request, const Url& node) {
    auto& uri = request->GetUri();
    uri.SetScheme(AwsScheme(node.scheme));
    uri.SetAuthority(Aws::String(BracketIPv6Host(node.host).c_str()));
    uri.SetPort(node.port);
    request->SetHeaderValue(Aws::Http::HOST_HEADER, Aws::String(node.Authority().c_str()));
}

NodeHealthObservation ObservationFromHttpResult(const std::shared_ptr<Aws::Http::HttpResponse>& response) {
    if (!response) {
        return NodeHealthObservation::ConnectionFailure;
    }
    if (response->HasClientError()) {
        return NodeHealthObservation::ConnectionFailure;
    }

    const auto code = response->GetResponseCode();
    if (code == Aws::Http::HttpResponseCode::REQUEST_NOT_MADE ||
        code == Aws::Http::HttpResponseCode::NETWORK_CONNECT_TIMEOUT ||
        code == Aws::Http::HttpResponseCode::NETWORK_READ_TIMEOUT) {
        return NodeHealthObservation::ConnectionFailure;
    }
    if (static_cast<int>(code) >= 500) {
        return NodeHealthObservation::ServerError;
    }
    return NodeHealthObservation::Success;
}

bool IsFirstSdkAttempt(const Aws::Http::HttpRequest& request) {
    if (!request.HasHeader(Aws::Http::SDK_REQUEST_HEADER)) {
        return false;
    }

    const auto header = request.GetHeaderValue(Aws::Http::SDK_REQUEST_HEADER);
    const Aws::String marker = "attempt=";
    const auto marker_pos = header.find(marker);
    if (marker_pos == Aws::String::npos) {
        return false;
    }

    auto value_pos = marker_pos + marker.size();
    while (value_pos < header.size() && header[value_pos] == ' ') {
        ++value_pos;
    }

    return value_pos < header.size() && header[value_pos] == '1';
}

Url SelectAttemptNode(const std::shared_ptr<Aws::Http::HttpRequest>& request,
                      const std::shared_ptr<AlternatorLiveNodes>& nodes,
                      std::uint16_t endpoint_override_port) {
    if (!request || !IsAlternatorEndpoint(request->GetUri(), *nodes, endpoint_override_port)) {
        return {};
    }

    auto current_node = UrlFromAwsUri(request->GetUri());
    if (IsFirstSdkAttempt(*request) && !SameEndpointHost(current_node.host, kEndpointOverrideHost)) {
        return current_node;
    }

    auto node = nodes->NextNode();
    if (!node.Empty()) {
        RewriteRequestEndpoint(request, node);
    }
    return node;
}

class AlternatorHttpClient final : public Aws::Http::HttpClient {
public:
    AlternatorHttpClient(std::shared_ptr<AlternatorLiveNodes> nodes,
                         std::uint16_t endpoint_override_port,
                         std::shared_ptr<Aws::Http::HttpClient> delegate)
        : nodes_(std::move(nodes))
        , endpoint_override_port_(endpoint_override_port)
        , delegate_(std::move(delegate)) {
        if (!nodes_) {
            throw std::invalid_argument("nodes must not be null");
        }
        if (!delegate_) {
            throw std::invalid_argument("delegate must not be null");
        }
    }

    std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(
        const std::shared_ptr<Aws::Http::HttpRequest>& request,
        Aws::Utils::RateLimits::RateLimiterInterface* read_limiter = nullptr,
        Aws::Utils::RateLimits::RateLimiterInterface* write_limiter = nullptr) const override {
        auto node = SelectAttemptNode(request, nodes_, endpoint_override_port_);

        auto response = delegate_->MakeRequest(request, read_limiter, write_limiter);
        if (!node.Empty()) {
            nodes_->ReportNodeResult(node, ObservationFromHttpResult(response));
        }
        return response;
    }

    bool SupportsChunkedTransferEncoding() const override {
        return delegate_->SupportsChunkedTransferEncoding();
    }

private:
    std::shared_ptr<AlternatorLiveNodes> nodes_;
    std::uint16_t endpoint_override_port_ = 0;
    std::shared_ptr<Aws::Http::HttpClient> delegate_;
};

class AlternatorHttpClientFactory final : public Aws::Http::HttpClientFactory {
public:
    AlternatorHttpClientFactory(std::shared_ptr<AlternatorLiveNodes> nodes,
                                std::uint16_t endpoint_override_port)
        : nodes_(std::move(nodes))
        , endpoint_override_port_(endpoint_override_port) {
        if (!nodes_) {
            throw std::invalid_argument("nodes must not be null");
        }
    }

    std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(
        const Aws::Client::ClientConfiguration& client_configuration) const override {
        // Keep one SDK platform client behind each wrapper so the SDK-owned
        // connection pool can persist across DynamoDB requests.
        auto delegate = Aws::MakeShared<Aws::Http::PlatformHttpClient>(
            kAllocationTag,
            client_configuration);
        return Aws::MakeShared<AlternatorHttpClient>(
            kAllocationTag,
            nodes_,
            endpoint_override_port_,
            std::move(delegate));
    }

    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
        const Aws::String& uri,
        Aws::Http::HttpMethod method,
        const Aws::IOStreamFactory& stream_factory) const override {
        return CreateHttpRequest(Aws::Http::URI(uri), method, stream_factory);
    }

    std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(
        const Aws::Http::URI& uri,
        Aws::Http::HttpMethod method,
        const Aws::IOStreamFactory& stream_factory) const override {
        auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(
            kAllocationTag,
            uri,
            method);
        request->SetResponseStreamFactory(stream_factory);
        return request;
    }

    void InitStaticState() override {
        Aws::Http::PlatformHttpClient::InitGlobalState();
    }

    void CleanupStaticState() override {
        Aws::Http::PlatformHttpClient::CleanupGlobalState();
    }

private:
    std::shared_ptr<AlternatorLiveNodes> nodes_;
    std::uint16_t endpoint_override_port_ = 0;
};

std::string EndpointOverride(std::uint16_t port, const std::string& scheme) {
    return scheme + "://" + kEndpointOverrideHost + ":" + std::to_string(port);
}

std::shared_ptr<Aws::Http::HttpClientFactory> NewAlternatorHttpClientFactory(
    std::shared_ptr<AlternatorLiveNodes> nodes,
    std::uint16_t endpoint_override_port) {
    return Aws::MakeShared<AlternatorHttpClientFactory>(
        kAllocationTag,
        std::move(nodes),
        endpoint_override_port);
}

} // namespace

AlternatorEndpointProvider::AlternatorEndpointProvider(std::shared_ptr<AlternatorLiveNodes> nodes)
    : nodes_(std::move(nodes)) {
    if (!nodes_) {
        throw std::invalid_argument("nodes must not be null");
    }
}

void AlternatorEndpointProvider::InitBuiltInParameters(
    const Aws::DynamoDB::Endpoint::DynamoDBClientConfiguration& config) {
    default_provider_.InitBuiltInParameters(config);
}

void AlternatorEndpointProvider::InitBuiltInParameters(
    const Aws::DynamoDB::Endpoint::DynamoDBClientConfiguration& config,
    const Aws::String& service_name) {
    default_provider_.InitBuiltInParameters(config, service_name);
}

void AlternatorEndpointProvider::OverrideEndpoint(const Aws::String& endpoint) {
    default_provider_.OverrideEndpoint(endpoint);
}

Aws::DynamoDB::Endpoint::DynamoDBClientContextParameters&
AlternatorEndpointProvider::AccessClientContextParameters() {
    return default_provider_.AccessClientContextParameters();
}

const Aws::DynamoDB::Endpoint::DynamoDBClientContextParameters&
AlternatorEndpointProvider::GetClientContextParameters() const {
    return default_provider_.GetClientContextParameters();
}

Aws::Endpoint::ResolveEndpointOutcome AlternatorEndpointProvider::ResolveEndpoint(
    const Aws::DynamoDB::Endpoint::EndpointParameters&) const {
    const auto node = nodes_->NextNode();
    if (node.Empty()) {
        return Aws::Endpoint::ResolveEndpointOutcome(
            Aws::Client::AWSError<Aws::Client::CoreErrors>(
                Aws::Client::CoreErrors::ENDPOINT_RESOLUTION_FAILURE,
                "NoAlternatorNodes",
                "no Alternator nodes are available",
                false));
    }

    Aws::Endpoint::AWSEndpoint endpoint;
    endpoint.SetURL(Aws::String(node.ToString().c_str()));
    return Aws::Endpoint::ResolveEndpointOutcome(std::move(endpoint));
}

DynamoDBHelper::DynamoDBHelper(std::vector<std::string> initial_nodes,
                               Config config,
                               std::shared_ptr<HttpClient> discovery_http_client)
    : nodes_(std::make_shared<AlternatorLiveNodes>(std::move(initial_nodes),
                                                   config,
                                                   std::move(discovery_http_client)))
    , config_(std::move(config))
    , partition_keys_(config_.key_route_affinity.partition_key_by_table) {
    nodes_->Start();
}

DynamoDBHelper::~DynamoDBHelper() {
    Stop();
}

std::shared_ptr<Aws::DynamoDB::DynamoDBClient> DynamoDBHelper::NewDynamoDB() const {
    auto client_config = NewClientConfiguration();
    auto endpoint_provider = NewEndpointProvider();

    if (!config_.credentials.access_key_id.empty() || !config_.credentials.secret_access_key.empty()) {
        Aws::Auth::AWSCredentials credentials(
            Aws::String(config_.credentials.access_key_id.c_str()),
            Aws::String(config_.credentials.secret_access_key.c_str()));
        return Aws::MakeShared<Aws::DynamoDB::DynamoDBClient>(
            "ScyllaDBAlternatorDynamoDBClient",
            credentials,
            endpoint_provider,
            client_config);
    }

    return Aws::MakeShared<Aws::DynamoDB::DynamoDBClient>(
        "ScyllaDBAlternatorDynamoDBClient",
        client_config,
        endpoint_provider);
}

std::shared_ptr<AlternatorEndpointProvider> DynamoDBHelper::NewEndpointProvider() const {
    return Aws::MakeShared<AlternatorEndpointProvider>(
        "ScyllaDBAlternatorEndpointProvider",
        nodes_);
}

std::shared_ptr<Aws::Http::HttpClientFactory> DynamoDBHelper::NewHttpClientFactory() const {
    return NewAlternatorHttpClientFactory(nodes_, config_.port);
}

Aws::DynamoDB::DynamoDBClientConfiguration DynamoDBHelper::NewClientConfiguration() const {
    Aws::DynamoDB::DynamoDBClientConfiguration client_config;
    client_config.region = Aws::String(config_.aws_region.c_str());
    client_config.scheme = AwsScheme(config_.scheme);
    client_config.verifySSL = config_.verify_ssl;
    client_config.maxConnections = config_.max_connections;
    client_config.connectTimeoutMs = static_cast<long>(config_.connect_timeout.count());
    client_config.httpRequestTimeoutMs = static_cast<long>(config_.http_client_timeout.count());
    client_config.requestTimeoutMs = static_cast<long>(config_.http_client_timeout.count());
    client_config.endpointOverride = Aws::String(EndpointOverride(config_.port, config_.scheme).c_str());

    if (!config_.ca_file.empty()) {
        client_config.caFile = Aws::String(config_.ca_file.c_str());
    }
    if (!config_.user_agent.empty()) {
        client_config.userAgent = Aws::String(config_.user_agent.c_str());
    }
    return client_config;
}

std::shared_ptr<AlternatorLiveNodes> DynamoDBHelper::Nodes() const {
    return nodes_;
}

void DynamoDBHelper::ApplyToSDKOptions(Aws::SDKOptions& options) const {
    auto nodes = nodes_;
    const auto port = config_.port;
    options.httpOptions.httpClientFactory_create_fn = [nodes, port] {
        return NewAlternatorHttpClientFactory(nodes, port);
    };
}

void DynamoDBHelper::Start() {
    nodes_->Start();
}

void DynamoDBHelper::Stop() {
    nodes_->Stop();
}

void DynamoDBHelper::UpdateLiveNodes() {
    nodes_->UpdateLiveNodes();
}

void DynamoDBHelper::ReportNodeResult(const Url& node, NodeHealthObservation observation) {
    nodes_->ReportNodeResult(node, observation);
}

void DynamoDBHelper::SetPartitionKeyName(const std::string& table_name, std::string partition_key_name) {
    partition_keys_.SetPartitionKeyName(table_name, std::move(partition_key_name));
}

std::string DynamoDBHelper::GetPartitionKeyName(const std::string& table_name) const {
    return partition_keys_.GetPartitionKeyName(table_name);
}

bool DynamoDBHelper::ShouldUseKeyRouteAffinityForWrite(WriteOperationKind kind,
                                                       const WriteOperationOptions& options) const {
    return ShouldUseKeyRouteAffinity(config_.key_route_affinity.mode, kind, options);
}

QueryPlan DynamoDBHelper::NewPartitionKeyQueryPlan(const AttributeMap& values,
                                                   const std::string& table_name) const {
    return QueryPlanForPartitionKey(*nodes_, values, table_name, partition_keys_);
}

QueryPlan DynamoDBHelper::NewBatchWriteQueryPlan(const std::vector<BatchWriteOperation>& operations) const {
    return QueryPlanForBatchWrite(*nodes_, operations, partition_keys_);
}

void DynamoDBHelper::CheckIfRackAndDatacenterSetCorrectly() {
    nodes_->CheckIfRackAndDatacenterSetCorrectly();
}

bool DynamoDBHelper::CheckIfRackDatacenterFeatureIsSupported() {
    return nodes_->CheckIfRackDatacenterFeatureIsSupported();
}

} // namespace scylladb::alternator::aws
