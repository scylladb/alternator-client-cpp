#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/URI.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/DynamoDBEndpointProvider.h>

#include <scylladb/alternator/live_nodes.h>
#include <scylladb/alternator/key_route_affinity.h>

namespace Aws {
struct SDKOptions;
namespace Http {
class HttpClientFactory;
} // namespace Http
} // namespace Aws

namespace scylladb::alternator::aws {

template <typename RequestT>
void AttachNodeErrorReporter(RequestT& request, std::shared_ptr<AlternatorLiveNodes> nodes) {
    if (!nodes) {
        return;
    }

    auto last_node = std::make_shared<Url>();
    auto last_node_mutex = std::make_shared<std::mutex>();
    auto previous_signed_handler = request.GetRequestSignedHandler();
    auto previous_retry_handler = request.GetRequestRetryHandler();

    request.SetRequestSignedHandler(
        [last_node, last_node_mutex, previous_signed_handler](const Aws::Http::HttpRequest& http_request) {
            const auto& uri = http_request.GetUri();
            const auto scheme = uri.GetScheme() == Aws::Http::Scheme::HTTPS ? std::string("https") : std::string("http");
            auto port = uri.GetPort();
            if (port == 0) {
                port = uri.GetScheme() == Aws::Http::Scheme::HTTPS ? 443 : 80;
            }

            {
                std::lock_guard<std::mutex> lock(*last_node_mutex);
                *last_node = Url(scheme, std::string(uri.GetHost().c_str()), port);
            }

            if (previous_signed_handler) {
                previous_signed_handler(http_request);
            }
        });

    request.SetRequestRetryHandler(
        [nodes, last_node, last_node_mutex, previous_retry_handler](const Aws::AmazonWebServiceRequest& retry_request) {
            Url node;
            {
                std::lock_guard<std::mutex> lock(*last_node_mutex);
                node = *last_node;
            }
            if (!node.Empty()) {
                // The retry handler runs after a failed attempt. Treat it as an HTTP error observation.
                nodes->ReportNodeResult(node, NodeHealthObservation::ServerError);
            }
            if (previous_retry_handler) {
                previous_retry_handler(retry_request);
            }
        });
}

class AlternatorEndpointProvider final : public Aws::DynamoDB::Endpoint::DynamoDBEndpointProviderBase {
public:
    explicit AlternatorEndpointProvider(std::shared_ptr<AlternatorLiveNodes> nodes);

    void InitBuiltInParameters(const Aws::DynamoDB::Endpoint::DynamoDBClientConfiguration& config) override;
    void InitBuiltInParameters(const Aws::DynamoDB::Endpoint::DynamoDBClientConfiguration& config,
                               const Aws::String& service_name) override;
    void OverrideEndpoint(const Aws::String& endpoint) override;

    Aws::DynamoDB::Endpoint::DynamoDBClientContextParameters& AccessClientContextParameters() override;
    const Aws::DynamoDB::Endpoint::DynamoDBClientContextParameters& GetClientContextParameters() const override;
    Aws::Endpoint::ResolveEndpointOutcome ResolveEndpoint(
        const Aws::DynamoDB::Endpoint::EndpointParameters& endpoint_parameters) const override;

private:
    std::shared_ptr<AlternatorLiveNodes> nodes_;
    Aws::DynamoDB::Endpoint::DynamoDBEndpointProvider default_provider_;
};

class DynamoDBHelper {
public:
    DynamoDBHelper(std::vector<std::string> initial_nodes,
                   Config config = {},
                   std::shared_ptr<HttpClient> discovery_http_client = nullptr);

    [[nodiscard]] std::shared_ptr<Aws::DynamoDB::DynamoDBClient> NewDynamoDB() const;
    [[nodiscard]] Aws::DynamoDB::DynamoDBClientConfiguration NewClientConfiguration() const;
    [[nodiscard]] std::shared_ptr<AlternatorEndpointProvider> NewEndpointProvider() const;
    [[nodiscard]] std::shared_ptr<Aws::Http::HttpClientFactory> NewHttpClientFactory() const;
    [[nodiscard]] std::shared_ptr<AlternatorLiveNodes> Nodes() const;

    void ApplyToSDKOptions(Aws::SDKOptions& options) const;
    void Start();
    void Stop();
    void UpdateLiveNodes();
    void ReportNodeResult(const Url& node, NodeHealthObservation observation);
    void SetPartitionKeyName(const std::string& table_name, std::string partition_key_name);
    [[nodiscard]] std::string GetPartitionKeyName(const std::string& table_name) const;
    [[nodiscard]] bool ShouldUseKeyRouteAffinityForWrite(WriteOperationKind kind,
                                                         const WriteOperationOptions& options) const;
    [[nodiscard]] QueryPlan NewPartitionKeyQueryPlan(const AttributeMap& values,
                                                     const std::string& table_name) const;
    [[nodiscard]] QueryPlan NewBatchWriteQueryPlan(const std::vector<BatchWriteOperation>& operations) const;
    template <typename RequestT>
    void AttachNodeErrorReporter(RequestT& request) const {
        ::scylladb::alternator::aws::AttachNodeErrorReporter(request, nodes_);
    }
    void CheckIfRackAndDatacenterSetCorrectly();
    [[nodiscard]] bool CheckIfRackDatacenterFeatureIsSupported();

private:
    std::shared_ptr<AlternatorLiveNodes> nodes_;
    Config config_;
    PartitionKeyMetadata partition_keys_;
};

} // namespace scylladb::alternator::aws
