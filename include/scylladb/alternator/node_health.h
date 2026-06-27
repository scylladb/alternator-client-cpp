#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include <scylladb/alternator/uri.h>

namespace scylladb::alternator {

enum class NodeHealthObservation {
    Success,
    ServerError,
    ConnectionFailure,
};

enum class NodeHealthState {
    Active,
    Quarantined,
    Down,
};

struct NodeHealthStoreConfig {
    // Number of consecutive HTTP 5xx observations required to mark a node down.
    std::uint32_t consecutive_server_error_threshold = 3;
    // Number of consecutive non-5xx observations required to promote a quarantined node to active.
    std::uint32_t quarantine_success_threshold = 3;
    // Background period for probing down nodes with GET /localnodes. Non-positive disables background probing.
    std::chrono::milliseconds down_node_probe_period{std::chrono::seconds(30)};
    // Include a quarantined node in normal routing once every N routing attempts. 1 means every attempt.
    std::uint32_t quarantine_traffic_interval = 10;
    bool disabled = false;
};

struct NodeHealthStatus {
    NodeHealthState state = NodeHealthState::Active;
    std::uint32_t consecutive_server_errors = 0;
    std::uint32_t consecutive_successes = 0;
    std::chrono::steady_clock::time_point updated{};
};

class NodeHealthStore {
public:
    using DownProbe = std::function<NodeHealthObservation(const Url&, const NodeHealthStatus&)>;

    NodeHealthStore(NodeHealthStoreConfig config, std::vector<Url> initial_nodes);

    [[nodiscard]] std::vector<Url> GetActiveNodes() const;
    [[nodiscard]] std::vector<Url> GetQuarantinedNodes() const;
    [[nodiscard]] std::vector<Url> GetDownNodes() const;
    [[nodiscard]] std::optional<NodeHealthStatus> GetNodeStatus(const Url& node) const;

    void AddNode(const Url& node);
    void RemoveNode(const Url& node);
    void ReportNodeResult(const Url& node, NodeHealthObservation observation);
    std::vector<Url> ProbeDownNodes(const DownProbe& probe);

private:
    [[nodiscard]] std::vector<Url> GetNodesByState(NodeHealthState state) const;

    NodeHealthStoreConfig config_;
    mutable std::mutex mutex_;
    std::map<Url, NodeHealthStatus> statuses_;
};

} // namespace scylladb::alternator
