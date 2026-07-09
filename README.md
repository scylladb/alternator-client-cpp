# C++ Alternator client

This repository provides a C++ Alternator client for ScyllaDB deployments that expose the DynamoDB API across multiple nodes.

The core library discovers Alternator nodes through `/localnodes`, keeps a live node set, supports rack/datacenter routing scopes with fallbacks, tracks active/quarantined/down node state, and feeds query plans from one candidate list. Quarantine sampling is handled by the live-node source before a query plan is built. An optional AWS SDK for C++ adapter creates a DynamoDB client with a dynamic endpoint provider.

## Build

```sh
make build
make test
```

The core target requires C++17. The default discovery HTTP client uses libcurl when CMake can find the libcurl development package; otherwise it falls back to a small POSIX plain-HTTP client. HTTPS discovery requires libcurl or a caller-provided `HttpClient`. Tests use GoogleTest when it is installed. The AWS adapter target is built only when CMake can find `AWSSDK` with the `dynamodb` component.

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

The AWS adapter uses the SDK's per-client `DynamoDBEndpointProviderBase` hook by default. AWS SDK for C++ resolves that endpoint once for a retried operation, so `DynamoDBHelper::ApplyToSDKOptions()` can also install a process-wide HTTP client factory before `Aws::InitAPI()`. That factory delegates to the SDK HTTP client and rotates only requests already aimed at the helper's Alternator endpoints.

The HTTP client factory runs after request signing. This is the closest public AWS SDK for C++ hook for retry-aware endpoint rewriting, but it means applications that depend on strict SigV4 host validation should prefer endpoint-provider routing or implement their own SDK wrapper. Automatic middleware features such as transparent header optimization and request-content-based endpoint rewriting are not available in the same form. The core library does provide deterministic key-route affinity primitives, and `DynamoDBHelper` exposes them for applications that integrate request-specific routing in their own AWS SDK wrapper.

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
