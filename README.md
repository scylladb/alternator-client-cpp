# C++ Alternator client

This repository provides a C++ Alternator client for ScyllaDB deployments that expose the DynamoDB API across multiple nodes.

The core library discovers Alternator nodes through `/localnodes`, keeps a live node set, supports rack/datacenter routing scopes with fallbacks, tracks active/quarantined/down node state, and feeds query plans from one candidate list. Quarantine sampling is handled by the live-node source before a query plan is built. An optional AWS SDK for C++ adapter creates a DynamoDB client with a dynamic endpoint provider.

## Build

```sh
make build
make test
```

The core target requires C++17. The default discovery HTTP client uses libcurl when CMake can find the libcurl development package; otherwise it falls back to a small POSIX plain-HTTP client. HTTPS discovery requires libcurl or a caller-provided `HttpClient`. Built-in gzip and deflate response decoding is available when CMake finds zlib; callers can provide a custom decoder for other content encodings or for builds without zlib. Tests use GoogleTest when it is installed. The AWS adapter target is built only when CMake can find `AWSSDK` with the `dynamodb` component. `make test-integration` requires the AWS adapter and its tests by configuring with `ALTERNATOR_CLIENT_CPP_REQUIRE_AWS=ON`.

## Core Usage

```cpp
#include <scylladb/alternator/live_nodes.h>

using namespace scylladb::alternator;

Config cfg;
cfg.port = 8080;
cfg.scheme = "http";
cfg.routing_scope = NewRackScope("dc1", "rack1", NewDCScope("dc1", NewClusterScope()));

AlternatorLiveNodes nodes({"node1.example.com", "node2.example.com"}, cfg);
nodes.UpdateLiveNodes();

auto next = nodes.NextNode();
```

For cluster-wide routing, the client queries bare `/localnodes` on every configured initial node and merges
the returned node lists. Some ScyllaDB versions return only the contacted node's datacenter from
`/localnodes`, even with cluster scope. In multi-datacenter deployments, configure at least one working
initial node from every datacenter that should receive traffic:

```cpp
AlternatorLiveNodes nodes({
    "dc1-node.example.com",
    "dc2-node.example.com",
    "dc3-node.example.com",
}, cfg);
```

## AWS SDK for C++ Usage

```cpp
#include <aws/core/Aws.h>
#include <scylladb/alternator/aws/dynamodb_helper.h>

int main() {
    scylladb::alternator::Config cfg;
    cfg.port = 8080;
    cfg.aws_region = "us-east-1";
    cfg.credentials = {"alternator", "secret"};

    scylladb::alternator::aws::DynamoDBHelper helper({"node1.example.com", "node2.example.com"}, cfg);

    Aws::SDKOptions options;
    helper.ApplyToSDKOptions(options); // Optional: rotate nodes for each HTTP attempt.
    Aws::InitAPI(options);
    {
        auto ddb = helper.NewDynamoDB();

        // Use ddb like a regular Aws::DynamoDB::DynamoDBClient.
    }
    Aws::ShutdownAPI(options);
}
```

`Config::aws_region` is required by the AWS SDK for C++ client configuration,
including request signing and diagnostic metadata. Alternator does not support
SigV4 request validation, and this client does not try to preserve a valid
SigV4 signature after Alternator-specific request rewriting. Alternator does
not use the region value to choose endpoints; `DynamoDBHelper` discovers nodes
through `/localnodes` and installs the endpoint provider or HTTP wrapper that
routes requests to live Alternator nodes. The default value is
`default-alb-region`, which prevents the SDK configuration from being empty but
can look confusing in logs, traces, or metrics.

Set `cfg.aws_region` to the deployment or Scylla Cloud region that makes sense
for your environment when AWS SDK observability should show a real region. The
value only affects SDK metadata and signing scope; it does not change
Alternator node discovery or load balancing.

The AWS adapter uses the SDK's per-client `DynamoDBEndpointProviderBase` hook by default. AWS SDK for C++ resolves that endpoint once for a retried operation, so `DynamoDBHelper::ApplyToSDKOptions()` can also install a process-wide HTTP client factory before `Aws::InitAPI()`. That factory delegates to the SDK HTTP client and rotates only requests already aimed at the helper's Alternator endpoints.

The HTTP client factory runs after request signing. This is the closest public
AWS SDK for C++ hook for retry-aware endpoint rewriting, header optimization,
and request or response compression, and it matches Alternator's SigV4 behavior:
Alternator does not validate SigV4 signatures. Do not use the factory through
a proxy, gateway, or deployment policy that requires valid SigV4 signatures
after the request leaves the SDK. Automatic request-content-based endpoint
rewriting is not available in the same form. The core library does provide
deterministic key-route affinity primitives, and `DynamoDBHelper` exposes them
for applications that integrate request-specific routing in their own AWS SDK
wrapper.

### Headers Optimization

Header optimization is disabled by default because `Config::header_optimization`
is null. Set it to a `HeaderOptimization` implementation to make
`DynamoDBHelper::ApplyToSDKOptions()` install an HTTP client factory that
removes DynamoDB request headers not used by Alternator before the request is
sent. The default allowlist keeps `Host`, `X-Amz-Target`, `Content-Length`,
`Accept-Encoding`, and `Content-Encoding`. When `Config::credentials` is set,
it also keeps `Authorization` and `X-Amz-Date`. When `Config::user_agent` is
set, it keeps `User-Agent`.

```cpp
scylladb::alternator::Config cfg;
cfg.header_optimization =
    std::make_shared<scylladb::alternator::DefaultHeaderOptimization>();
```

Use `HeaderAllowlistOptimization` to replace the default allowlist:

```cpp
cfg.header_optimization =
    std::make_shared<scylladb::alternator::HeaderAllowlistOptimization>(
        std::vector<std::string>{
            "Host",
            "X-Amz-Target",
            "Content-Length",
        });
```

Header names are matched case-insensitively. The override is exact; if it is
set, the helper does not add credential or user-agent headers automatically.
Applications can provide their own implementation by deriving from
`HeaderOptimization`:

```cpp
class MyHeaderOptimization final : public scylladb::alternator::HeaderOptimization {
public:
    std::vector<std::string> AllowedHeaders(
        const scylladb::alternator::HeaderOptimizationContext& context) const override {
        std::vector<std::string> headers = {"Host", "X-Amz-Target", "Content-Length"};
        if (context.credentials_configured) {
            headers.push_back("Authorization");
            headers.push_back("X-Amz-Date");
        }
        return headers;
    }
};
```

Header optimization applies only to requests routed to known Alternator
endpoints through the factory installed by `DynamoDBHelper::ApplyToSDKOptions()`
before `Aws::InitAPI()`.

### HTTP Request Compression

Request compression is disabled by default. Configure a request compressor
before constructing `DynamoDBHelper`, then install the Alternator HTTP client
factory with `DynamoDBHelper::ApplyToSDKOptions()` before `Aws::InitAPI()`.
The factory compresses request bodies only for requests routed to known
Alternator nodes and sets the matching `Content-Encoding` and `Content-Length`
headers before sending the request.

Request compression is applied by the Alternator HTTP client factory after the
AWS SDK signs the request. That is compatible with Alternator because Alternator
does not support SigV4 validation. It is not compatible with any proxy, gateway,
or DynamoDB-compatible service that validates SigV4 signatures.

```cpp
scylladb::alternator::Config cfg;
cfg.request_compressor =
    std::make_shared<scylladb::alternator::GzipRequestCompressor>();
```

Only gzip request compression is built in. `GzipRequestCompressor` requires
zlib at build time and skips bodies smaller than 1024 bytes by default. Pass a
different minimum size, such as `GzipRequestCompressor(0)`, to control that
policy. The `HttpRequestCompressor` interface owns the decision to compress and
compresses from an input stream to an output stream so implementations do not
need to copy the whole request body into an intermediate string.

### HTTP Response Compression

Response compression is disabled by default. When configured, the built-in
discovery client sends the configured `Accept-Encoding` value on `/localnodes`
requests. If zlib was found at build time, configuring
`ZlibContentEncodingDecoder` enables matching `Content-Encoding: gzip` and
`Content-Encoding: deflate` responses.

For DynamoDB operations, response compression is supported when
`DynamoDBHelper::ApplyToSDKOptions()` installs the Alternator HTTP client
factory before `Aws::InitAPI()`. The factory advertises the configured response
encodings only for requests routed to Alternator nodes and decodes compressed
responses before the AWS SDK parses the JSON body. A DynamoDB client that only
uses the endpoint provider does not get this factory-level response decoding.

Configure content-encoding decoders before constructing the helper or discovery
client. The default empty decoder list disables compression advertisement.
Each decoder advertises the encodings it supports, and each advertised encoding
may be owned by only one decoder. Responses using an encoding outside the
configured decoder list are rejected.

```cpp
scylladb::alternator::Config cfg;
cfg.content_encoding_decoders.push_back(
    std::make_shared<scylladb::alternator::ZlibContentEncodingDecoder>());
```

When zlib is available, `ZlibContentEncodingDecoder` implements gzip and deflate
decoding and advertises both encodings by default. Pass a subset to advertise
only one of them, for example `ZlibContentEncodingDecoder({"gzip"})`. zlib is
optional at build time; without it, applications can still provide their own
decoder implementations.

For generic content-encoding support, provide another decoder. Its
`AcceptedResponseEncodings()` method declares the encodings to advertise, and
`Decode()` receives the encoded response body and the specific encoding token to
decode:

```cpp
class BrotliDecoder final : public scylladb::alternator::HttpContentEncodingDecoder {
public:
    std::vector<std::string> AcceptedResponseEncodings() const override {
        return {"br"};
    }

    std::string Decode(std::string body, const std::string& content_encoding) const override {
        // Decode body according to content_encoding.
        return body;
    }
};

scylladb::alternator::Config cfg;
cfg.content_encoding_decoders.push_back(std::make_shared<BrotliDecoder>());
```

To request gzip and zstd, configure zlib for gzip only and add a zstd decoder:

```cpp
scylladb::alternator::Config cfg;
cfg.content_encoding_decoders.push_back(
    std::make_shared<scylladb::alternator::ZlibContentEncodingDecoder>(
        std::vector<std::string>{"gzip"}));
cfg.content_encoding_decoders.push_back(std::make_shared<ZstdDecoder>());
```

### DynamoDB HTTP Connections

DynamoDB requests use the AWS SDK for C++ HTTP transport. The helper passes
`Config::max_connections` to the SDK client configuration, so persistent HTTP/HTTPS
connection pooling is enabled by default and bounded at 100 connections per DynamoDB
client.

Increase the reusable connection pool before constructing the helper or DynamoDB client:

```cpp
scylladb::alternator::Config cfg;
cfg.max_connections = 400;
```

When `DynamoDBHelper::ApplyToSDKOptions()` installs the retry-aware HTTP client factory,
the Alternator wrapper still delegates requests to the SDK platform HTTP client. The SDK
transport owns the underlying connection pool and continues to reuse connections while
the wrapper rotates Alternator endpoints.

When integrating custom retry or logging code around AWS SDK outcomes, call `helper.ReportNodeResult(node, observation)`
or `helper.Nodes()->ReportNodeResult(node, observation)`. The node-health layer translates observations into rigid
active/quarantined/down state:

- connection failures mark a node down immediately;
- consecutive 5xx responses mark a node down;
- down nodes are probed in the background with `GET /localnodes`;
- a non-5xx probe response moves a down node into quarantine;
- quarantined nodes are sampled into routing once every configured number of attempts;
- enough consecutive non-5xx responses from a quarantined node promote it back to active.

Key-route affinity hashes and votes over active nodes only. When a quarantined node is selected for a partition-key hash,
that hash keeps seeing the same node until the node is promoted to active or sent back down. The sampled node is scheduled
first for that hash, then the plan falls back to the active-node affinity order.

Sticky quarantine sampling is intentionally narrow:

- a hash is remembered only when the quarantined node is returned first for that hash;
- unrelated hashes continue to use the active-node affinity plan unless they are sampled separately;
- mappings are removed when the mapped node leaves quarantine, either by promotion to active or by moving back down.

The thresholds and probe cadence are configurable:

```cpp
scylladb::alternator::Config cfg;
cfg.node_health.consecutive_server_error_threshold = 5;
cfg.node_health.down_node_probe_period = std::chrono::seconds(10);
cfg.node_health.quarantine_traffic_interval = 20;
cfg.node_health.quarantine_success_threshold = 3;
```

### Discovery Refresh

`AlternatorLiveNodes::Start()` runs background `/localnodes` refreshes. Recent request activity uses
`nodes_list_update_period`, while idle clients use `idle_nodes_list_update_period`:

```cpp
scylladb::alternator::Config cfg;
cfg.nodes_list_update_period = std::chrono::seconds(1);
cfg.idle_nodes_list_update_period = std::chrono::minutes(1);
```

The AWS helper starts and stops this background refresh automatically. The core `AlternatorLiveNodes`
type leaves lifecycle control to the caller.

When libcurl is available, the default discovery client reuses HTTP connections by default. It keeps
a libcurl connection cache bounded by `max_connections` and can be disabled for debugging or
compatibility:

```cpp
scylladb::alternator::Config cfg;
cfg.max_connections = 100;
cfg.reuse_discovery_connections = false;
```

The POSIX fallback discovery client supports plain HTTP only and closes each request.

TLS session caching is enabled by default for HTTPS discovery. The libcurl session cache can be
disabled, and OpenSSL-backed libcurl builds also honor the configured cache size and timeout:

```cpp
scylladb::alternator::Config cfg;
cfg.tls_session_cache_enabled = true;
cfg.tls_session_cache_size = 1024;
cfg.tls_session_timeout = std::chrono::hours(24);
```

The endpoint provider itself chooses nodes, but AWS SDK for C++ does not expose a public per-attempt hook before request
signing.

For individual requests, the helper can attach AWS SDK retry hooks that report the signed endpoint when the SDK retries:

```cpp
Aws::DynamoDB::Model::PutItemRequest request;
helper.AttachNodeErrorReporter(request);
auto outcome = ddb->PutItem(request);
```

### Key Route Affinity

Key-route affinity makes write operations with the same partition key prefer the same coordinator. This can improve read-before-write paths such as conditional writes. The C++ core uses deterministic Murmur3/AttributeValue hashing and seeded query-plan selection:

```cpp
using namespace scylladb::alternator;

Config cfg;
cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;

scylladb::alternator::aws::DynamoDBHelper helper({"node1.example.com", "node2.example.com"}, cfg);

auto plan = helper.NewPartitionKeyQueryPlan(
    {{"id", AttributeValue::String("order-123")}},
    "orders");

auto preferred = plan.Next();
```

The AWS helper can discover missing table partition-key names automatically. When an affinity plan needs metadata that is
not cached yet, the helper starts one background `DescribeTable` lookup for that table and returns a normal non-affinity
query plan for the current request. Later requests use the discovered HASH key name once the lookup completes. Configure
`cfg.key_route_affinity.partition_key_by_table` when you already know the table metadata and do not want to wait for
discovery:

```cpp
cfg.key_route_affinity.partition_key_by_table = {{"orders", "id"}};
```

You can also refresh one table explicitly during startup:

```cpp
(void)helper.UpdatePartitionKeyName("orders");
```

Discovery retries `DescribeTable` a few times with exponential backoff to tolerate transient metadata
unavailability, such as writes immediately after `CreateTable`:

```cpp
cfg.key_route_affinity.partition_key_discovery_attempts = 3;
cfg.key_route_affinity.partition_key_discovery_initial_backoff = std::chrono::milliseconds{100};
```

The AWS helper also exposes typed write methods that inspect SDK request objects before sending them. These methods route
`PutItem`, `UpdateItem`, and `DeleteItem` according to `KeyRouteAffinityMode::ReadBeforeWrite` or `AnyWrite`, and route
`BatchWriteItem` in `AnyWrite` mode using batch-write voting. If metadata is missing or the partition key cannot be
hashed, the current request uses the normal query plan and discovery is triggered for later requests:

```cpp
Aws::DynamoDB::Model::PutItemRequest put;
put.SetTableName("orders");
Aws::DynamoDB::Model::AttributeValue id;
id.SetS("order-123");
put.AddItem("id", id);

auto outcome = helper.PutItem(put);
```

For `BatchWriteItem`-style operations, pass the put/delete candidates and the helper will vote for preferred nodes. Voted nodes are tried first by descending vote count, with deterministic node-order tie breaking:

```cpp
auto batch_plan = helper.NewBatchWriteQueryPlan({
    BatchWriteOperation::Put("orders", {{"id", AttributeValue::String("order-123")}}),
    BatchWriteOperation::Delete("orders", {{"id", AttributeValue::String("order-456")}}),
});
```

## Implemented Features

- `/localnodes` discovery with cluster, datacenter, and rack scopes.
- Scope fallback chains such as rack -> datacenter -> cluster.
- Cluster scope merge across configured initial nodes.
- Active and idle `/localnodes` refresh cadence.
- Reused libcurl discovery HTTP connections with an opt-out switch.
- TLS session cache enable/disable, cache size, and timeout configuration for HTTPS discovery.
- Persistent AWS SDK DynamoDB HTTP connection pooling via `max_connections`.
- Optional gzip request compression for AWS SDK DynamoDB requests routed through the Alternator HTTP client factory.
- Active, quarantined, and down node pools with rigid observation-based transitions.
- Round-robin `NextNode()` and flat per-request query plans, including deterministic seeded plans for affinity callers.
- Key-route affinity helpers for single-write partition keys and batch-write preferred-node voting.
- TLS verification toggle, CA file, client certificate/key file, and HTTP timeouts for libcurl discovery.
- Murmur3/AttributeValue hashing helpers for string, number, and binary partition key values.
- Optional AWS SDK for C++ DynamoDB helper and endpoint provider.

## Targets

- `alternator_client_cpp`: core discovery/load-balancing library.
- `alternator_client_cpp_aws`: optional AWS SDK for C++ adapter, available when `AWSSDK` is found.
- `alternator_client_cpp_tests`: unit tests when `GTest` is found.
- `alternator_client_cpp_aws_tests`: AWS adapter tests, available when `AWSSDK` and `GTest` are found.

After installation, downstream CMake projects can use:

```cmake
find_package(scylladb-alternator-client-cpp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ScyllaDB::alternator_client_cpp)
```

When the package was built with AWS SDK for C++ available, link the AWS adapter target instead:

```cmake
find_package(scylladb-alternator-client-cpp CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ScyllaDB::alternator_client_cpp_aws)
```
