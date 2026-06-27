#include <scylladb/alternator/node_health.h>

#include <algorithm>
#include <utility>

namespace scylladb::alternator {

NodeHealthStore::NodeHealthStore(NodeHealthStoreConfig config, std::vector<Url> initial_nodes)
    : config_(std::move(config)) {
    if (config_.consecutive_server_error_threshold == 0) {
        config_.consecutive_server_error_threshold = 1;
    }
    if (config_.quarantine_success_threshold == 0) {
        config_.quarantine_success_threshold = 1;
    }
    if (config_.quarantine_traffic_interval == 0) {
        config_.quarantine_traffic_interval = 1;
    }
    for (const auto& node : initial_nodes) {
        AddNode(node);
    }
}

std::vector<Url> NodeHealthStore::GetActiveNodes() const {
    if (config_.disabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Url> nodes;
        nodes.reserve(statuses_.size());
        for (const auto& [node, _] : statuses_) {
            nodes.push_back(node);
        }
        std::sort(nodes.begin(), nodes.end());
        return nodes;
    }
    return GetNodesByState(NodeHealthState::Active);
}

std::vector<Url> NodeHealthStore::GetQuarantinedNodes() const {
    if (config_.disabled) {
        return {};
    }
    return GetNodesByState(NodeHealthState::Quarantined);
}

std::vector<Url> NodeHealthStore::GetDownNodes() const {
    if (config_.disabled) {
        return {};
    }
    return GetNodesByState(NodeHealthState::Down);
}

std::optional<NodeHealthStatus> NodeHealthStore::GetNodeStatus(const Url& node) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = statuses_.find(node);
    if (it == statuses_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void NodeHealthStore::AddNode(const Url& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (statuses_.find(node) != statuses_.end()) {
        return;
    }
    statuses_.emplace(node, NodeHealthStatus{
        NodeHealthState::Active,
        0,
        config_.quarantine_success_threshold,
        std::chrono::steady_clock::now()});
}

void NodeHealthStore::RemoveNode(const Url& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    statuses_.erase(node);
}

void NodeHealthStore::ReportNodeResult(const Url& node, NodeHealthObservation observation) {
    if (config_.disabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = statuses_.find(node);
    if (it == statuses_.end()) {
        return;
    }

    switch (observation) {
    case NodeHealthObservation::Success:
        it->second.consecutive_server_errors = 0;
        if (it->second.state == NodeHealthState::Down) {
            it->second.state = NodeHealthState::Quarantined;
            it->second.consecutive_successes = 1;
        } else if (it->second.state == NodeHealthState::Quarantined) {
            ++it->second.consecutive_successes;
            if (it->second.consecutive_successes >= config_.quarantine_success_threshold) {
                it->second.state = NodeHealthState::Active;
                it->second.consecutive_successes = config_.quarantine_success_threshold;
            }
        } else {
            it->second.consecutive_successes = config_.quarantine_success_threshold;
        }
        break;
    case NodeHealthObservation::ServerError:
        ++it->second.consecutive_server_errors;
        it->second.consecutive_successes = 0;
        if (it->second.consecutive_server_errors >= config_.consecutive_server_error_threshold) {
            it->second.state = NodeHealthState::Down;
        }
        break;
    case NodeHealthObservation::ConnectionFailure:
        it->second.state = NodeHealthState::Down;
        it->second.consecutive_server_errors = config_.consecutive_server_error_threshold;
        it->second.consecutive_successes = 0;
        break;
    }
    it->second.updated = std::chrono::steady_clock::now();
}

std::vector<Url> NodeHealthStore::ProbeDownNodes(const DownProbe& probe) {
    if (config_.disabled || !probe) {
        return {};
    }

    auto candidates = GetDownNodes();
    std::vector<Url> responsive;
    for (const auto& node : candidates) {
        auto status = GetNodeStatus(node);
        if (!status) {
            continue;
        }
        const auto observation = probe(node, *status);
        ReportNodeResult(node, observation);
        if (observation == NodeHealthObservation::Success) {
            responsive.push_back(node);
        }
    }
    return responsive;
}

std::vector<Url> NodeHealthStore::GetNodesByState(NodeHealthState state) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Url> nodes;
    for (const auto& [node, status] : statuses_) {
        if (status.state == state) {
            nodes.push_back(node);
        }
    }
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

} // namespace scylladb::alternator
