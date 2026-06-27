#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    [[nodiscard]] std::vector<Url> GetActiveNodes() const override;
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
    [[nodiscard]] std::vector<Url> FetchLiveNodes();
    [[nodiscard]] std::vector<Url> GetNodesForScope(const RoutingScope& scope);
    [[nodiscard]] std::vector<Url> GetNodesFromEndpoint(const Url& endpoint) const;
    [[nodiscard]] Url NextKnownNode();
    [[nodiscard]] bool ShouldTryQuarantinedNode(bool active_nodes_empty) const;
    [[nodiscard]] Url NextQuarantinedNode() const;
    [[nodiscard]] Url StickyQuarantinedNodeForHash(std::int64_t hash, const std::vector<Url>& active_nodes) const;
    void RemoveQuarantineHashAssignmentsForNode(const Url& node);
    void MaybeRefresh();
    void BackgroundLoop();

    Config config_;
    std::shared_ptr<HttpClient> http_client_;
    std::vector<Url> initial_nodes_;

    mutable std::mutex mutex_;
    std::vector<Url> live_nodes_;
    mutable std::map<std::int64_t, Url> quarantine_by_hash_;
    std::unique_ptr<NodeHealthStore> health_store_;
    std::atomic<std::uint64_t> next_node_index_{0};
    mutable std::atomic<std::uint64_t> quarantine_plan_index_{0};
    mutable std::atomic<std::uint64_t> quarantine_node_index_{0};
    std::chrono::steady_clock::time_point next_update_;

    std::mutex background_mutex_;
    std::condition_variable background_cv_;
    bool background_started_ = false;
    bool stopping_ = false;
    std::thread background_thread_;
};

} // namespace scylladb::alternator
