# CCM integration implementation

This document maps the [CCM integration specification](../ccm-integration.md) to the C++ test
harness.

## Public test API

[`cluster_spec.h`](../../tests/testinfra/cluster_spec.h) defines immutable typed cluster settings:
Scylla version, datacenter/rack topology, HTTP and HTTPS transports, authentication and
authorization modes, per-node SMP and memory, and validated Scylla YAML overrides.
`ClusterSpecs::DefaultSpec()` applies `SCYLLA_VERSION`, defaulting to `release:2025.2.5`.

[`test_cluster.h`](../../tests/testinfra/test_cluster.h) exposes the binary-wide entry point:

- `TestClusters::AcquireReusable(const ClusterSpec&)` returns a reusable RAII lease.
- `TestClusters::ProvisionPrivate(const ClusterSpec&)` returns an exclusive RAII lease.

Reusable leases expose read-only cluster information and an independent resource scope. Private
leases expose the same connection information plus serialized cluster and node lifecycle controls.
Leases are move-safe and release ownership exactly once; destruction performs best-effort cleanup
without throwing. Cleanup failures poison retained state and are reported by subsequent pool or
test-teardown operations.

Connection information includes one seed endpoint, all node endpoints, optional signing
credentials, and the generated CA certificate path for HTTPS. Resource scopes create unique table
names and delete their owned tables when a reusable lease is released. `TestClusterInfo::ClientConfig()`
turns connection material into the library's normal `Config` type for integration tests.

## Configuration

The operational environment is intentionally small:

| Variable | Meaning | Default |
| --- | --- | --- |
| `SCYLLA_VERSION` | CCM relocatable-package selector | `release:2025.2.5` |
| `SCYLLA_INTEGRATION_VERSION` | Existing client-suite package selector | `release:2026.1.6` |
| `SCYLLA_CCM_PATH` | CCM executable | Repository-pinned virtual environment |
| `SCYLLA_CCM_ROOT` | Private live-state and address-reservation root | `/tmp/alternator-client-cpp-ccm-<uid>` |
| `SCYLLA_CCM_MAX_NODES` | Optional lower node ceiling | `9` |
| `SCYLLA_CCM_DIAGNOSTICS_DIR` | External artifact directory | `build/ccm` |

The repository-pinned CCM revision is
`d15a2fab9d22fffad8a30c806a7c8e1632e58aae`. A custom `SCYLLA_CCM_PATH` is accepted only after its
entry point passes a launch check. Native CCM targets require a successful configure-time runtime
probe of `pidfd_open(2)` and `pidfd_send_signal(2)`. This gates stale-process recovery on kernels and
sandbox policies that support race-safe signaling. Each child command receives a harness-selected
`SCYLLA_ARCH` (`x86_64` or `aarch64`) derived from the Linux host rather than inherited process state.

Provisioning contracts use `SCYLLA_VERSION`. Existing client integration tests use
`SCYLLA_INTEGRATION_VERSION` because compressed-request coverage requires Scylla 2026.1 or newer.
`make test-integration` adds `localhost`, `127.0.0.1`, and every possible allocated CCM node address
to both `NO_PROXY` and `no_proxy` for test processes while retaining configured proxy access for
package downloads and other remote traffic. Exact addresses keep the bypass compatible with libcurl
versions that predate CIDR matching in `NO_PROXY`.

## Internal architecture

[`test_cluster_internal.h`](../../tests/testinfra/test_cluster_internal.h) declares internal
`TestClusterPool`, `PhysicalTestCluster`, `CcmProvisioner`, and `CcmRunState` types. Their
implementation in [`test_cluster.cpp`](../../tests/testinfra/test_cluster.cpp) owns one
physical-cluster slot for the test binary, lease reference counts, resource scopes, normal
teardown, and handoff to durable run state. Matching reusable acquisitions share the slot. An idle
incompatible cluster is removed and replaced; incompatible acquisitions fail immediately while
leases are active. Operations are serialized directly rather than through a wait queue, memory
budget, admission scheduler, or LRU cache.

`CcmProvisioner` translates typed topology into CCM `create` and `add` operations, applies typed
Scylla configuration, generates CA and per-node certificates with OpenSSL, starts nodes, waits for
HTTP/HTTPS readiness, and writes every command to a durable log. Commands run in isolated process
groups. Ambiguous process cleanup marks the cluster dirty and retains it for next-start recovery.

`CcmRunState` owns the private per-user state tree, cross-process loopback-range reservations,
owner and cluster manifests, stale-run scanning, marked process termination, safe-path validation,
diagnostics transfer, and address release.

## Provisioning model

CCM IDs range from 1 through 99. ID `N` owns loopback addresses `127.0.N.1` through
`127.0.N.9`, allowing later private-node additions without colliding with another test process.
Reservation probes cover Scylla storage, CQL, shard-aware CQL, API, Prometheus, requested Alternator
ports, and JMX when required by the selected CCM/package combination.

Default Alternator ports are 8080 for HTTP and 8043 for HTTPS. Provisioning also sets
`alternator_write_isolation=only_rmw_uses_lwt`, enables native transport, and uses
`GossipingPropertyFileSnitch` so exposed node metadata matches the requested datacenter/rack
topology.

HTTPS provisioning creates a per-cluster CA and per-node certificates with IP subject alternative
names. Client connection builders trust that CA while retaining certificate and hostname
validation. Private keys remain inside live state and are excluded from diagnostics.

YAML override keys are canonicalized, sorted for reuse identity, parsed with YAML scalar semantics,
and applied to both cluster configuration and every node's `scylla.yaml`. Keys owned by typed
topology, transport, security, addressing, and startup settings cannot be overridden.

## Lifecycle and recovery

Reusable lease release first cleans the lease's table prefix. Cleanup failure poisons the slot. A
healthy idle matching cluster remains available for reuse; private leases always remove their
physical cluster at release.

Private controls support whole-cluster start/stop and node start/stop/add/remove. Node additions
choose the lowest free `nodeN` identity, apply TLS and YAML settings before startup, and attempt one
rollback after failure. Node removal starts a stopped node if needed, decommissions it, then removes
its CCM state. Removal is rejected before mutation when projected running voters cannot form quorum.
Foreign or stale node handles and removal of the final node are rejected.

Each run records PID, process start ticks, boot ID, owned cluster manifest, and CCM ID reservation
before provisioning. The next harness startup skips live owners and recovers stale owners. Recovery
terminates only same-user processes carrying the exact run marker, snapshots diagnostics, performs
bounded CCM removal, then releases ownership. Malformed or unremovable state remains quarantined;
another free CCM ID may still be used.

## Integration-suite wiring

The integration-test global fixture acquires one default reusable lease and publishes its HTTP and
HTTPS endpoints, first-node datacenter/rack, credentials, and CA path to existing tests. Tests no
longer depend on Docker bridge addresses, Docker Compose lifecycle, or checked-in server
configuration.

The Makefile remains orchestration only: validate/install pinned CCM, preflight required external
tools, build the AWS integration target, and run provisioning contracts plus the existing CTest
integration suite in foreground phases. C++ code owns cluster lifecycle, diagnostics, and crash
recovery; no standalone shell helper is part of the design.

## Requirement mapping

| Requirement | Primary implementation | Required evidence |
| --- | --- | --- |
| `CCM-REQ-001` | `cluster_spec.h` | Validation, immutability, canonical reuse-key tests |
| `CCM-REQ-002` | `CcmProvisioner` in `test_cluster_internal.h` and `ccm_provisioner.cpp` | Real native topology, TLS, auth, and override integration tests |
| `CCM-REQ-003` | `test_cluster.h/.cpp` | Shared instance plus independent resource cleanup test |
| `CCM-REQ-004` | `test_cluster.h/.cpp`, `CcmProvisioner` | Private node lifecycle and ambiguous-failure tests |
| `CCM-REQ-005` | `test_cluster.cpp` | Fail-fast active incompatibility, idle replacement, node-limit tests |
| `CCM-REQ-006` | `CcmRunState` in `test_cluster_internal.h` and `ccm_run_state.cpp` | Hard-kill/restart, quarantine, diagnostics, and safe-path tests |

## Intentional language differences

Java uses `AutoCloseable` and try-with-resources; C++ uses move-aware RAII leases. Java's
process-wide singleton becomes one binary-wide C++ pool. Public naming follows C++ conventions,
but provisioning, isolation, lifecycle, failure, and recovery behavior remains equivalent.
