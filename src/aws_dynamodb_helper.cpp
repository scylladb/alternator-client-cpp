#include <scylladb/alternator/aws/dynamodb_helper.h>

#include "http_compression.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/http/curl/CurlHttpClient.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/dynamodb/model/AttributeAction.h>
#include <aws/dynamodb/model/AttributeValueUpdate.h>
#include <aws/dynamodb/model/BatchWriteItemRequest.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>
#include <aws/dynamodb/model/DeleteRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/KeySchemaElement.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/PutRequest.h>
#include <aws/dynamodb/model/ReturnValue.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>
#include <aws/dynamodb/model/WriteRequest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ios>
#include <istream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace scylladb::alternator::aws {
namespace {

constexpr char kAllocationTag[] = "ScyllaDBAlternatorAwsAdapter";
constexpr char kEndpointOverrideHost[] = "dynamodb.fake.alternator.cluster.node";

struct HeaderOptimizationPolicy {
    bool enabled = false;
    std::set<std::string> allowed_headers;
};

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

std::string ReadResponseBody(Aws::IOStream& body) {
    body.clear();
    body.seekg(0, std::ios::beg);
    return std::string(std::istreambuf_iterator<char>(body), std::istreambuf_iterator<char>());
}

void WriteResponseBody(Aws::IOStream& body, const std::string& value) {
    if (!value.empty()) {
        body.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    body.flush();
    body.clear();
    body.seekg(0, std::ios::beg);
}

std::shared_ptr<Aws::Http::HttpResponse> DecodeCompressedAwsResponse(
    const std::shared_ptr<Aws::Http::HttpRequest>& request,
    const std::shared_ptr<Aws::Http::HttpResponse>& response,
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders) {
    if (!request || !response || response->HasClientError() ||
        !response->HasHeader(Aws::Http::CONTENT_ENCODING_HEADER)) {
        return response;
    }

    const auto content_encoding = std::string(response->GetHeader(Aws::Http::CONTENT_ENCODING_HEADER).c_str());
    auto decoded_body = detail::DecodeHttpResponseBody(
        ReadResponseBody(response->GetResponseBody()),
        content_encoding,
        content_encoding_decoders);

    auto decoded_response = Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(
        kAllocationTag,
        request);
    decoded_response->SetResponseCode(response->GetResponseCode());

    for (const auto& header : response->GetHeaders()) {
        const auto header_name = detail::ToLowerAscii(std::string(header.first.c_str()));
        if (header_name == Aws::Http::CONTENT_ENCODING_HEADER ||
            header_name == Aws::Http::CONTENT_LENGTH_HEADER) {
            continue;
        }
        decoded_response->AddHeader(header.first, header.second);
    }
    decoded_response->AddHeader(
        Aws::Http::CONTENT_LENGTH_HEADER,
        Aws::String(std::to_string(decoded_body.size()).c_str()));
    WriteResponseBody(decoded_response->GetResponseBody(), decoded_body);
    return decoded_response;
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

std::set<std::string> NormalizeHeaderAllowlist(const std::vector<std::string>& headers) {
    std::set<std::string> allowlist;
    for (const auto& header : headers) {
        allowlist.insert(detail::ToLowerAscii(header));
    }
    return allowlist;
}

HeaderOptimizationContext HeaderOptimizationContextFromConfig(const Config& config) {
    return {
        !config.credentials.access_key_id.empty() || !config.credentials.secret_access_key.empty(),
        !config.user_agent.empty(),
    };
}

HeaderOptimizationPolicy BuildHeaderOptimizationPolicy(const Config& config) {
    if (!config.header_optimization) {
        return {};
    }

    const auto headers = config.header_optimization->AllowedHeaders(
        HeaderOptimizationContextFromConfig(config));
    return {true, NormalizeHeaderAllowlist(headers)};
}

void OptimizeRequestHeaders(const std::shared_ptr<Aws::Http::HttpRequest>& request,
                            const HeaderOptimizationPolicy& policy) {
    if (!request || !policy.enabled) {
        return;
    }

    const auto headers = request->GetHeaders();
    for (const auto& header : headers) {
        const auto header_name = detail::ToLowerAscii(std::string(header.first.c_str()));
        if (policy.allowed_headers.find(header_name) == policy.allowed_headers.end()) {
            request->DeleteHeader(header.first.c_str());
        }
    }
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
                         std::vector<std::shared_ptr<HttpContentEncodingDecoder>> content_encoding_decoders,
                         HeaderOptimizationPolicy header_optimization,
                         std::shared_ptr<Aws::Http::HttpClient> delegate)
        : nodes_(std::move(nodes))
        , endpoint_override_port_(endpoint_override_port)
        , content_encoding_decoders_(std::move(content_encoding_decoders))
        , accept_encoding_value_(detail::BuildAcceptEncodingValue(content_encoding_decoders_))
        , header_optimization_(std::move(header_optimization))
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
        if (!node.Empty() && !accept_encoding_value_.empty()) {
            request->SetAcceptEncoding(Aws::String(accept_encoding_value_.c_str()));
        }
        if (!node.Empty()) {
            OptimizeRequestHeaders(request, header_optimization_);
        }

        auto response = delegate_->MakeRequest(request, read_limiter, write_limiter);
        if (!node.Empty()) {
            response = DecodeCompressedAwsResponse(
                request,
                response,
                content_encoding_decoders_);
        }
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
    std::vector<std::shared_ptr<HttpContentEncodingDecoder>> content_encoding_decoders_;
    std::string accept_encoding_value_;
    HeaderOptimizationPolicy header_optimization_;
    std::shared_ptr<Aws::Http::HttpClient> delegate_;
};

class AlternatorHttpClientFactory final : public Aws::Http::HttpClientFactory {
public:
    AlternatorHttpClientFactory(std::shared_ptr<AlternatorLiveNodes> nodes,
                                std::uint16_t endpoint_override_port,
                                std::vector<std::shared_ptr<HttpContentEncodingDecoder>> content_encoding_decoders,
                                HeaderOptimizationPolicy header_optimization)
        : nodes_(std::move(nodes))
        , endpoint_override_port_(endpoint_override_port)
        , content_encoding_decoders_(std::move(content_encoding_decoders))
        , header_optimization_(std::move(header_optimization)) {
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
            content_encoding_decoders_,
            header_optimization_,
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
    std::vector<std::shared_ptr<HttpContentEncodingDecoder>> content_encoding_decoders_;
    HeaderOptimizationPolicy header_optimization_;
};

std::string EndpointOverride(std::uint16_t port, const std::string& scheme) {
    return scheme + "://" + kEndpointOverrideHost + ":" + std::to_string(port);
}

std::shared_ptr<Aws::Http::HttpClientFactory> NewAlternatorHttpClientFactory(
    std::shared_ptr<AlternatorLiveNodes> nodes,
    std::uint16_t endpoint_override_port,
    std::vector<std::shared_ptr<HttpContentEncodingDecoder>> content_encoding_decoders,
    HeaderOptimizationPolicy header_optimization) {
    return Aws::MakeShared<AlternatorHttpClientFactory>(
        kAllocationTag,
        std::move(nodes),
        endpoint_override_port,
        std::move(content_encoding_decoders),
        std::move(header_optimization));
}

class FixedEndpointProvider final : public Aws::DynamoDB::Endpoint::DynamoDBEndpointProviderBase {
public:
    explicit FixedEndpointProvider(Url node)
        : node_(std::move(node)) {}

    void InitBuiltInParameters(const Aws::DynamoDB::Endpoint::DynamoDBClientConfiguration& config) override {
        default_provider_.InitBuiltInParameters(config);
    }

    void InitBuiltInParameters(const Aws::DynamoDB::Endpoint::DynamoDBClientConfiguration& config,
                               const Aws::String& service_name) override {
        default_provider_.InitBuiltInParameters(config, service_name);
    }

    void OverrideEndpoint(const Aws::String& endpoint) override {
        default_provider_.OverrideEndpoint(endpoint);
    }

    Aws::DynamoDB::Endpoint::DynamoDBClientContextParameters& AccessClientContextParameters() override {
        return default_provider_.AccessClientContextParameters();
    }

    const Aws::DynamoDB::Endpoint::DynamoDBClientContextParameters& GetClientContextParameters() const override {
        return default_provider_.GetClientContextParameters();
    }

    Aws::Endpoint::ResolveEndpointOutcome ResolveEndpoint(
        const Aws::DynamoDB::Endpoint::EndpointParameters&) const override {
        if (node_.Empty()) {
            return Aws::Endpoint::ResolveEndpointOutcome(
                Aws::Client::AWSError<Aws::Client::CoreErrors>(
                    Aws::Client::CoreErrors::ENDPOINT_RESOLUTION_FAILURE,
                    "NoAlternatorNodes",
                    "no Alternator nodes are available",
                    false));
        }

        Aws::Endpoint::AWSEndpoint endpoint;
        endpoint.SetURL(Aws::String(node_.ToString().c_str()));
        return Aws::Endpoint::ResolveEndpointOutcome(std::move(endpoint));
    }

private:
    Url node_;
    Aws::DynamoDB::Endpoint::DynamoDBEndpointProvider default_provider_;
};

ReturnValues ToReturnValues(Aws::DynamoDB::Model::ReturnValue value) {
    switch (value) {
    case Aws::DynamoDB::Model::ReturnValue::NONE:
        return ReturnValues::None;
    case Aws::DynamoDB::Model::ReturnValue::ALL_OLD:
        return ReturnValues::AllOld;
    case Aws::DynamoDB::Model::ReturnValue::UPDATED_OLD:
        return ReturnValues::UpdatedOld;
    case Aws::DynamoDB::Model::ReturnValue::ALL_NEW:
        return ReturnValues::AllNew;
    case Aws::DynamoDB::Model::ReturnValue::UPDATED_NEW:
        return ReturnValues::UpdatedNew;
    case Aws::DynamoDB::Model::ReturnValue::NOT_SET:
        return ReturnValues::NotSet;
    }
    return ReturnValues::NotSet;
}

AttributeAction ToAttributeAction(Aws::DynamoDB::Model::AttributeAction action) {
    switch (action) {
    case Aws::DynamoDB::Model::AttributeAction::ADD:
        return AttributeAction::Add;
    case Aws::DynamoDB::Model::AttributeAction::DELETE_:
        return AttributeAction::Delete;
    case Aws::DynamoDB::Model::AttributeAction::PUT:
    case Aws::DynamoDB::Model::AttributeAction::NOT_SET:
        return AttributeAction::Put;
    }
    return AttributeAction::Put;
}

AttributeValue ToAttributeValue(const Aws::DynamoDB::Model::AttributeValue& value) {
    switch (value.GetType()) {
    case Aws::DynamoDB::Model::ValueType::STRING:
        return AttributeValue::String(std::string(value.GetS().c_str()));
    case Aws::DynamoDB::Model::ValueType::NUMBER:
        return AttributeValue::Number(std::string(value.GetN().c_str()));
    case Aws::DynamoDB::Model::ValueType::BYTEBUFFER: {
        const auto& buffer = value.AccessB();
        std::vector<std::uint8_t> bytes;
        if (buffer.GetLength() > 0) {
            const auto* data = buffer.GetUnderlyingData();
            bytes.assign(data, data + buffer.GetLength());
        }
        return AttributeValue::Binary(std::move(bytes));
    }
    case Aws::DynamoDB::Model::ValueType::STRING_SET:
    case Aws::DynamoDB::Model::ValueType::NUMBER_SET:
    case Aws::DynamoDB::Model::ValueType::BYTEBUFFER_SET:
    case Aws::DynamoDB::Model::ValueType::ATTRIBUTE_MAP:
    case Aws::DynamoDB::Model::ValueType::ATTRIBUTE_LIST:
    case Aws::DynamoDB::Model::ValueType::BOOL:
    case Aws::DynamoDB::Model::ValueType::NULLVALUE:
        break;
    }
    throw std::invalid_argument("unsupported DynamoDB partition-key attribute type");
}

AttributeMap PartitionKeyOnlyAttributeMap(
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>& values,
    const std::string& key_name) {
    const auto it = values.find(Aws::String(key_name.c_str()));
    if (it == values.end()) {
        return {};
    }
    return {{key_name, ToAttributeValue(it->second)}};
}

WriteOperationOptions PutItemOptions(const Aws::DynamoDB::Model::PutItemRequest& request) {
    WriteOperationOptions options;
    options.has_condition_expression = !request.GetConditionExpression().empty();
    options.has_expected = !request.GetExpected().empty();
    options.return_values = ToReturnValues(request.GetReturnValues());
    return options;
}

WriteOperationOptions DeleteItemOptions(const Aws::DynamoDB::Model::DeleteItemRequest& request) {
    WriteOperationOptions options;
    options.has_condition_expression = !request.GetConditionExpression().empty();
    options.has_expected = !request.GetExpected().empty();
    options.return_values = ToReturnValues(request.GetReturnValues());
    return options;
}

std::vector<AttributeUpdate> AttributeUpdates(
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValueUpdate>& updates) {
    std::vector<AttributeUpdate> out;
    out.reserve(updates.size());
    for (const auto& [_, update] : updates) {
        out.push_back(AttributeUpdate{
            ToAttributeAction(update.GetAction()),
            update.ValueHasBeenSet(),
        });
    }
    return out;
}

WriteOperationOptions UpdateItemOptions(const Aws::DynamoDB::Model::UpdateItemRequest& request) {
    WriteOperationOptions options;
    options.has_update_expression = !request.GetUpdateExpression().empty();
    options.has_condition_expression = !request.GetConditionExpression().empty();
    options.has_expected = !request.GetExpected().empty();
    options.return_values = ToReturnValues(request.GetReturnValues());
    options.attribute_updates = AttributeUpdates(request.GetAttributeUpdates());
    return options;
}

QueryPlan DefaultQueryPlanFor(const DynamoDBHelper& helper) {
    return QueryPlan::FromNodesSource(*helper.Nodes());
}

QueryPlan PartitionKeyQueryPlanFor(
    const DynamoDBHelper& helper,
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>& values,
    const Aws::String& table_name) {
    const auto table = std::string(table_name.c_str());
    const auto key_name = helper.GetPartitionKeyName(table);
    if (key_name.empty()) {
        return helper.NewPartitionKeyQueryPlan({}, table);
    }

    try {
        return helper.NewPartitionKeyQueryPlan(PartitionKeyOnlyAttributeMap(values, key_name), table);
    } catch (const std::invalid_argument&) {
        return helper.NewPartitionKeyQueryPlan({}, table);
    }
}

QueryPlan PutItemQueryPlanFor(const DynamoDBHelper& helper,
                              const Aws::DynamoDB::Model::PutItemRequest& request) {
    if (!helper.ShouldUseKeyRouteAffinityForWrite(WriteOperationKind::PutItem, PutItemOptions(request))) {
        return DefaultQueryPlanFor(helper);
    }
    return PartitionKeyQueryPlanFor(helper, request.GetItem(), request.GetTableName());
}

QueryPlan UpdateItemQueryPlanFor(const DynamoDBHelper& helper,
                                 const Aws::DynamoDB::Model::UpdateItemRequest& request) {
    if (!helper.ShouldUseKeyRouteAffinityForWrite(WriteOperationKind::UpdateItem, UpdateItemOptions(request))) {
        return DefaultQueryPlanFor(helper);
    }
    return PartitionKeyQueryPlanFor(helper, request.GetKey(), request.GetTableName());
}

QueryPlan DeleteItemQueryPlanFor(const DynamoDBHelper& helper,
                                 const Aws::DynamoDB::Model::DeleteItemRequest& request) {
    if (!helper.ShouldUseKeyRouteAffinityForWrite(WriteOperationKind::DeleteItem, DeleteItemOptions(request))) {
        return DefaultQueryPlanFor(helper);
    }
    return PartitionKeyQueryPlanFor(helper, request.GetKey(), request.GetTableName());
}

void AddBatchWriteOperationForMissingMetadata(std::vector<BatchWriteOperation>& operations,
                                              const std::string& table_name) {
    operations.push_back(BatchWriteOperation::Put(table_name, {}));
}

void AddBatchWriteOperation(std::vector<BatchWriteOperation>& operations,
                            const std::string& table_name,
                            const std::string& key_name,
                            const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>& values) {
    try {
        auto key_values = PartitionKeyOnlyAttributeMap(values, key_name);
        if (!key_values.empty()) {
            operations.push_back(BatchWriteOperation::Put(table_name, std::move(key_values)));
        }
    } catch (const std::invalid_argument&) {
    }
}

std::vector<BatchWriteOperation> BatchWriteOperationsFor(const DynamoDBHelper& helper,
                                                         const Aws::DynamoDB::Model::BatchWriteItemRequest& request) {
    std::vector<BatchWriteOperation> operations;
    for (const auto& [table, writes] : request.GetRequestItems()) {
        const auto table_name = std::string(table.c_str());
        const auto key_name = helper.GetPartitionKeyName(table_name);
        if (key_name.empty()) {
            if (!writes.empty()) {
                AddBatchWriteOperationForMissingMetadata(operations, table_name);
            }
            continue;
        }

        for (const auto& write : writes) {
            if (write.PutRequestHasBeenSet() && !write.DeleteRequestHasBeenSet()) {
                AddBatchWriteOperation(operations, table_name, key_name, write.GetPutRequest().GetItem());
            } else if (write.DeleteRequestHasBeenSet() && !write.PutRequestHasBeenSet()) {
                AddBatchWriteOperation(operations, table_name, key_name, write.GetDeleteRequest().GetKey());
            }
        }
    }
    return operations;
}

QueryPlan BatchWriteItemQueryPlanFor(const DynamoDBHelper& helper,
                                     const Aws::DynamoDB::Model::BatchWriteItemRequest& request) {
    // A default PutItem is affinity-routed only in AnyWrite mode; batch writes follow the same rule.
    if (!helper.ShouldUseKeyRouteAffinityForWrite(WriteOperationKind::PutItem, {})) {
        return DefaultQueryPlanFor(helper);
    }

    auto operations = BatchWriteOperationsFor(helper, request);
    if (operations.empty()) {
        return DefaultQueryPlanFor(helper);
    }
    return helper.NewBatchWriteQueryPlan(operations);
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
    , partition_keys_(std::make_shared<PartitionKeyMetadata>(config_.key_route_affinity.partition_key_by_table))
    , partition_key_discovery_(std::make_shared<PartitionKeyDiscoveryState>()) {
    nodes_->Start();
}

DynamoDBHelper::~DynamoDBHelper() {
    WaitForPartitionKeyDiscovery();
    Stop();
}

std::shared_ptr<Aws::DynamoDB::DynamoDBClient> DynamoDBHelper::NewDynamoDB() const {
    return NewDynamoDBWithEndpointProvider(NewEndpointProvider());
}

Aws::DynamoDB::Model::PutItemOutcome DynamoDBHelper::PutItem(
    const Aws::DynamoDB::Model::PutItemRequest& request) const {
    auto routed_request = request;
    AttachNodeErrorReporter(routed_request);
    return NewDynamoDBForQueryPlan(PutItemQueryPlanFor(*this, request))->PutItem(routed_request);
}

Aws::DynamoDB::Model::UpdateItemOutcome DynamoDBHelper::UpdateItem(
    const Aws::DynamoDB::Model::UpdateItemRequest& request) const {
    auto routed_request = request;
    AttachNodeErrorReporter(routed_request);
    return NewDynamoDBForQueryPlan(UpdateItemQueryPlanFor(*this, request))->UpdateItem(routed_request);
}

Aws::DynamoDB::Model::DeleteItemOutcome DynamoDBHelper::DeleteItem(
    const Aws::DynamoDB::Model::DeleteItemRequest& request) const {
    auto routed_request = request;
    AttachNodeErrorReporter(routed_request);
    return NewDynamoDBForQueryPlan(DeleteItemQueryPlanFor(*this, request))->DeleteItem(routed_request);
}

Aws::DynamoDB::Model::BatchWriteItemOutcome DynamoDBHelper::BatchWriteItem(
    const Aws::DynamoDB::Model::BatchWriteItemRequest& request) const {
    auto routed_request = request;
    AttachNodeErrorReporter(routed_request);
    return NewDynamoDBForQueryPlan(BatchWriteItemQueryPlanFor(*this, request))->BatchWriteItem(routed_request);
}

std::shared_ptr<Aws::DynamoDB::DynamoDBClient> DynamoDBHelper::NewDynamoDBWithEndpointProvider(
    std::shared_ptr<Aws::DynamoDB::Endpoint::DynamoDBEndpointProviderBase> endpoint_provider) const {
    auto client_config = NewClientConfiguration();

    if (!config_.credentials.access_key_id.empty() || !config_.credentials.secret_access_key.empty()) {
        Aws::Auth::AWSCredentials credentials(
            Aws::String(config_.credentials.access_key_id.c_str()),
            Aws::String(config_.credentials.secret_access_key.c_str()));
        return Aws::MakeShared<Aws::DynamoDB::DynamoDBClient>(
            "ScyllaDBAlternatorDynamoDBClient",
            credentials,
            std::move(endpoint_provider),
            client_config);
    }

    return Aws::MakeShared<Aws::DynamoDB::DynamoDBClient>(
        "ScyllaDBAlternatorDynamoDBClient",
        client_config,
        std::move(endpoint_provider));
}

std::shared_ptr<Aws::DynamoDB::DynamoDBClient> DynamoDBHelper::NewDynamoDBForNode(const Url& node) const {
    std::lock_guard<std::mutex> lock(fixed_clients_mutex_);
    const auto existing = fixed_clients_.find(node);
    if (existing != fixed_clients_.end()) {
        return existing->second;
    }

    auto endpoint_provider = Aws::MakeShared<FixedEndpointProvider>(
        "ScyllaDBAlternatorFixedEndpointProvider",
        node);
    auto client = NewDynamoDBWithEndpointProvider(std::move(endpoint_provider));
    fixed_clients_.emplace(node, client);
    return client;
}

std::shared_ptr<Aws::DynamoDB::DynamoDBClient> DynamoDBHelper::NewDynamoDBForQueryPlan(QueryPlan plan) const {
    const auto node = plan.Next();
    if (node.Empty()) {
        return NewDynamoDB();
    }
    return NewDynamoDBForNode(node);
}

std::shared_ptr<AlternatorEndpointProvider> DynamoDBHelper::NewEndpointProvider() const {
    return Aws::MakeShared<AlternatorEndpointProvider>(
        "ScyllaDBAlternatorEndpointProvider",
        nodes_);
}

std::shared_ptr<Aws::Http::HttpClientFactory> DynamoDBHelper::NewHttpClientFactory() const {
    return NewAlternatorHttpClientFactory(
        nodes_,
        config_.port,
        config_.content_encoding_decoders,
        BuildHeaderOptimizationPolicy(config_));
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
    const auto content_encoding_decoders = config_.content_encoding_decoders;
    const auto header_optimization = BuildHeaderOptimizationPolicy(config_);
    options.httpOptions.httpClientFactory_create_fn = [nodes, port, content_encoding_decoders, header_optimization] {
        return NewAlternatorHttpClientFactory(nodes, port, content_encoding_decoders, header_optimization);
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
    partition_keys_->SetPartitionKeyName(table_name, std::move(partition_key_name));
}

std::string DynamoDBHelper::GetPartitionKeyName(const std::string& table_name) const {
    return partition_keys_->GetPartitionKeyName(table_name);
}

bool DynamoDBHelper::UpdatePartitionKeyName(const std::string& table_name) const {
    if (table_name.empty()) {
        return false;
    }

    auto backoff = config_.key_route_affinity.partition_key_discovery_initial_backoff;
    for (std::uint32_t attempt = 1; attempt <= config_.key_route_affinity.partition_key_discovery_attempts; ++attempt) {
        const auto key_name = DiscoverPartitionKeyName(table_name);
        if (!key_name.empty()) {
            partition_keys_->SetPartitionKeyName(table_name, key_name);
            return true;
        }
        if (attempt == config_.key_route_affinity.partition_key_discovery_attempts) {
            break;
        }
        if (backoff > std::chrono::milliseconds::zero()) {
            std::this_thread::sleep_for(backoff);
            backoff *= 2;
        }
    }
    return false;
}

bool DynamoDBHelper::ShouldUseKeyRouteAffinityForWrite(WriteOperationKind kind,
                                                       const WriteOperationOptions& options) const {
    return ShouldUseKeyRouteAffinity(config_.key_route_affinity.mode, kind, options);
}

QueryPlan DynamoDBHelper::NewPartitionKeyQueryPlan(const AttributeMap& values,
                                                   const std::string& table_name) const {
    if (partition_keys_->GetPartitionKeyName(table_name).empty()) {
        TriggerPartitionKeyDiscovery(table_name);
        return DefaultQueryPlan();
    }

    try {
        return QueryPlanForPartitionKey(*nodes_, values, table_name, *partition_keys_);
    } catch (const std::invalid_argument&) {
        return DefaultQueryPlan();
    }
}

QueryPlan DynamoDBHelper::NewBatchWriteQueryPlan(const std::vector<BatchWriteOperation>& operations) const {
    TriggerPartitionKeyDiscoveryForMissingMetadata(operations);
    try {
        return QueryPlanForBatchWrite(*nodes_, operations, *partition_keys_);
    } catch (const std::invalid_argument&) {
        return DefaultQueryPlan();
    }
}

void DynamoDBHelper::CheckIfRackAndDatacenterSetCorrectly() {
    nodes_->CheckIfRackAndDatacenterSetCorrectly();
}

bool DynamoDBHelper::CheckIfRackDatacenterFeatureIsSupported() {
    return nodes_->CheckIfRackDatacenterFeatureIsSupported();
}

void DynamoDBHelper::TriggerPartitionKeyDiscovery(const std::string& table_name) const {
    if (table_name.empty() || !partition_keys_->GetPartitionKeyName(table_name).empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(partition_key_discovery_->mutex);
        if (!partition_key_discovery_->in_progress.insert(table_name).second) {
            return;
        }
    }

    try {
        std::lock_guard<std::mutex> lock(partition_key_discovery_->mutex);
        partition_key_discovery_->workers.emplace_back([this, table_name] {
            try {
                (void)UpdatePartitionKeyName(table_name);
            } catch (const std::exception& error) {
                AWS_LOGSTREAM_ERROR(
                    kAllocationTag,
                    "partition-key discovery failed for table " << table_name << ": " << error.what());
            } catch (...) {
                AWS_LOGSTREAM_ERROR(
                    kAllocationTag,
                    "partition-key discovery failed for table " << table_name << ": unknown error");
            }

            std::lock_guard<std::mutex> lock(partition_key_discovery_->mutex);
            partition_key_discovery_->in_progress.erase(table_name);
        });
    } catch (...) {
        std::lock_guard<std::mutex> lock(partition_key_discovery_->mutex);
        partition_key_discovery_->in_progress.erase(table_name);
    }
}

void DynamoDBHelper::TriggerPartitionKeyDiscoveryForMissingMetadata(
    const std::vector<BatchWriteOperation>& operations) const {
    std::set<std::string> missing_tables;
    for (const auto& operation : operations) {
        if (partition_keys_->GetPartitionKeyName(operation.table_name).empty()) {
            missing_tables.insert(operation.table_name);
        }
    }
    for (const auto& table_name : missing_tables) {
        TriggerPartitionKeyDiscovery(table_name);
    }
}

void DynamoDBHelper::WaitForPartitionKeyDiscovery() const {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(partition_key_discovery_->mutex);
        workers.swap(partition_key_discovery_->workers);
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::string DynamoDBHelper::DiscoverPartitionKeyName(const std::string& table_name) const {
    auto client = NewDynamoDB();
    Aws::DynamoDB::Model::DescribeTableRequest request;
    request.SetTableName(Aws::String(table_name.c_str()));

    const auto outcome = client->DescribeTable(request);
    if (!outcome.IsSuccess()) {
        AWS_LOGSTREAM_ERROR(
            kAllocationTag,
            "DescribeTable failed during partition-key discovery for table "
                << table_name << ": " << outcome.GetError().GetMessage());
        return {};
    }

    for (const auto& key_schema : outcome.GetResult().GetTable().GetKeySchema()) {
        if (key_schema.GetKeyType() == Aws::DynamoDB::Model::KeyType::HASH) {
            return std::string(key_schema.GetAttributeName().c_str());
        }
    }
    AWS_LOGSTREAM_ERROR(
        kAllocationTag,
        "DescribeTable returned no HASH key during partition-key discovery for table " << table_name);
    return {};
}

QueryPlan DynamoDBHelper::DefaultQueryPlan() const {
    return QueryPlan::FromNodesSource(*nodes_);
}

} // namespace scylladb::alternator::aws
