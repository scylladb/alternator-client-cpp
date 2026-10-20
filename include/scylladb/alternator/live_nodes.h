/*
 * Copyright ScyllaDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <scylladb/alternator/config.h>
#include <scylladb/alternator/http_client.h>
#include <scylladb/alternator/query_plan.h>

namespace scylladb::alternator {

class AlternatorLiveNodes final : public NodesSource {
public:
    AlternatorLiveNodes(std::vector<std::string> initial_nodes,
                        Config config = {},
                        std::shared_ptr<HttpClient> http_client = nullptr);
    ~AlternatorLiveNodes();

    AlternatorLiveNodes(const AlternatorLiveNodes&) = delete;
    AlternatorLiveNodes& operator=(const AlternatorLiveNodes&) = delete;

    [[nodiscard]] Url NextNode();
    [[nodiscard]] std::vector<Url> GetNodes() const;
    [[nodiscard]] std::vector<Url> GetKnownAffinityNodes() const;
    [[nodiscard]] std::vector<Url> GetActiveNodes() const override;
    [[nodiscard]] std::vector<Url> GetActiveKeyRouteAffinityNodes() const override;
    [[nodiscard]] std::vector<Url> GetQueryPlanNodes() const override;
    [[nodiscard]] std::vector<Url> GetQueryPlanNodesForHash(std::int64_t hash) const override;
    [[nodiscard]] std::vector<Url> GetQuarantinedNodes() const;
    [[nodiscard]] std::vector<Url> GetDownNodes() const override;

    void UpdateLiveNodes();
    void Start();
    void Stop();

    void ReportNodeResult(const Url& node, NodeHealthObservation observation);
    std::vector<Url> ProbeDownNodes();

    void CheckIfRackAndDatacenterSetCorrectly();
    [[nodiscard]] bool CheckIfRackDatacenterFeatureIsSupported();

    [[nodiscard]] const Config& GetConfig() const;

private:
    using ScopeOperation = std::pair<std::string, bool>;
    struct ScopeOccurrence {
        RoutingScopePtr original;
        RoutingScopePtr affinity;
        const ScopeOperation* operation;
        bool fallback_resolved;
    };
    struct DiscoveryOutcome {
        std::vector<Url> nodes;
        std::exception_ptr error;
    };
    struct DiscoveryResults {
        // Only the proven common prefix of ordinary and canonical occurrences.
        std::vector<std::vector<Url>> ordinary_nodes;
        // Unknown custom paths become canonical only after ordinary rack discovery.
        std::vector<RoutingScopePtr> ordinary_scopes;
        std::map<ScopeOperation, DiscoveryOutcome> operations;
        std::map<std::pair<Url, bool>, DiscoveryOutcome> endpoints;
    };

    void AppendAffinityScopes(RoutingScopePtr scope);
    void ResolveAffinityFallback(std::size_t index, RoutingScopePtr fallback);
    [[nodiscard]] std::vector<Url> FetchLiveNodes(DiscoveryResults& results);
    [[nodiscard]] std::vector<Url> GetAffinityNodesForScope(std::size_t index, DiscoveryResults& results);
    [[nodiscard]] std::vector<Url> GetNodesForScope(const RoutingScope& scope,
                                                  DiscoveryResults* results = nullptr,
                                                  bool reuse_endpoints = false);
    [[nodiscard]] std::vector<Url> GetDiscoveryNodesForScope(bool cluster_scope) const;
    [[nodiscard]] std::vector<Url> ReadDiscoveryEndpoint(const Url& endpoint, DiscoveryResults& results,
                                                       bool cluster_scope, bool reuse, bool& reused);
    [[nodiscard]] std::vector<Url> GetNodesFromEndpoint(const Url& endpoint) const;
    [[nodiscard]] std::vector<Url> GetQuarantinedKeyRouteAffinityNodes() const;
    [[nodiscard]] std::vector<Url> GetQuarantinedOrdinaryNodes() const;
    std::vector<Url> ProbeDownCandidates(const std::vector<Url>& candidates);
    [[nodiscard]] Url NextKnownNode();
    [[nodiscard]] bool ShouldTryQuarantinedNode(bool active_nodes_empty,
                                                const std::vector<Url>& quarantined_nodes) const;
    [[nodiscard]] Url NextQuarantinedNode(const std::vector<Url>& quarantined_nodes) const;
    [[nodiscard]] Url StickyQuarantinedNodeForHash(std::int64_t hash, const std::vector<Url>& active_nodes) const;
    void RemoveQuarantineHashAssignmentsForNode(const Url& node);
    void MarkActivity();
    [[nodiscard]] std::chrono::milliseconds RefreshIntervalForNow(std::chrono::steady_clock::time_point now) const;
    void ScheduleNextRefresh(std::chrono::steady_clock::time_point now);
    void BackgroundLoop();

    Config config_;
    // Entries are never erased; occurrences retain pointers to these fixed keys.
    std::map<ScopeOperation, RoutingScopePtr> fixed_affinity_scopes_;
    std::vector<ScopeOccurrence> affinity_scopes_;
    bool affinity_has_rack_ = false;
    std::shared_ptr<HttpClient> http_client_;
    std::vector<Url> initial_nodes_;

    std::mutex refresh_mutex_;
    mutable std::mutex mutex_;
    std::vector<Url> live_nodes_;
    std::vector<Url> known_affinity_nodes_;
    bool has_affinity_snapshot_ = false;
    mutable std::map<std::int64_t, Url> quarantine_by_hash_;
    std::unique_ptr<NodeHealthStore> health_store_;
    std::atomic<std::uint64_t> next_node_index_{0};
    mutable std::atomic<std::uint64_t> quarantine_plan_index_{0};
    mutable std::atomic<std::uint64_t> quarantine_node_index_{0};
    std::chrono::steady_clock::time_point next_update_;
    std::chrono::steady_clock::time_point last_activity_;

    std::mutex background_mutex_;
    std::condition_variable background_cv_;
    bool background_started_ = false;
    bool stopping_ = false;
    std::thread background_thread_;
};

} // namespace scylladb::alternator
