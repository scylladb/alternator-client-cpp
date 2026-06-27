#include <scylladb/alternator/query_plan.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include <scylladb/alternator/detail/go_random.h>

namespace scylladb::alternator {
namespace {

std::uint64_t SeedFromClock() {
    return static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

bool PopNode(std::vector<Url>& nodes, const Url& target, Url& out) {
    auto it = std::find(nodes.begin(), nodes.end(), target);
    if (it == nodes.end()) {
        return false;
    }
    out = *it;
    nodes.erase(it);
    return true;
}

std::vector<Url> DedupePreservingOrder(std::vector<Url> nodes) {
    std::vector<Url> out;
    out.reserve(nodes.size());
    for (auto& node : nodes) {
        if (node.Empty()) {
            continue;
        }
        if (std::find(out.begin(), out.end(), node) == out.end()) {
            out.push_back(std::move(node));
        }
    }
    return out;
}

} // namespace

QueryPlan::QueryPlan(std::vector<Url> nodes)
    : QueryPlan(std::move(nodes), SeedFromClock(), false, false, {}) {}

QueryPlan QueryPlan::WithSeed(std::vector<Url> nodes, std::int64_t seed) {
    return QueryPlan(std::move(nodes),
                     static_cast<std::uint64_t>(seed),
                     false,
                     false,
                     {});
}

QueryPlan QueryPlan::WithSortedSeed(std::vector<Url> nodes, std::int64_t seed) {
    return QueryPlan(std::move(nodes),
                     static_cast<std::uint64_t>(seed),
                     false,
                     true,
                     {});
}

QueryPlan QueryPlan::WithPreferredNode(std::vector<Url> nodes, Url preferred_node) {
    std::vector<Url> preferred_nodes;
    if (!preferred_node.Empty()) {
        preferred_nodes.push_back(std::move(preferred_node));
    }
    return WithPreferredNodes(std::move(nodes), std::move(preferred_nodes));
}

QueryPlan QueryPlan::WithPreferredNodes(std::vector<Url> nodes, std::vector<Url> preferred_nodes) {
    return QueryPlan(std::move(nodes),
                     0,
                     true,
                     true,
                     std::move(preferred_nodes));
}

QueryPlan QueryPlan::FromOrderedNodes(std::vector<Url> nodes) {
    return QueryPlan(DedupePreservingOrder(std::move(nodes)),
                     0,
                     true,
                     false,
                     {});
}

QueryPlan QueryPlan::FromNodesSource(const NodesSource& source) {
    return QueryPlan(source.GetQueryPlanNodes());
}

QueryPlan::QueryPlan(std::vector<Url> nodes,
                     std::uint64_t seed,
                     bool deterministic_order,
                     bool sort_nodes,
                     std::vector<Url> preferred_nodes)
    : nodes_(std::move(nodes))
    , seed_(static_cast<std::int64_t>(seed))
    , deterministic_order_(deterministic_order)
    , sort_nodes_(sort_nodes)
    , preferred_nodes_(std::move(preferred_nodes)) {}

Url QueryPlan::Next() {
    Prepare();

    while (!preferred_nodes_.empty()) {
        auto preferred_node = preferred_nodes_.front();
        preferred_nodes_.erase(preferred_nodes_.begin());
        if (preferred_node.Empty()) {
            continue;
        }
        Url out;
        if (PopNode(nodes_, preferred_node, out)) {
            return out;
        }
    }

    if (!nodes_.empty()) {
        return PickAndRemove(nodes_);
    }
    return {};
}

Url QueryPlan::PickAndRemove(std::vector<Url>& nodes) {
    if (deterministic_order_) {
        Url out = nodes.front();
        nodes.erase(nodes.begin());
        return out;
    }

    if (!go_rng_) {
        go_rng_ = std::make_unique<detail::GoRandom>(seed_);
    }
    const auto idx = static_cast<std::size_t>(go_rng_->Int31n(static_cast<std::int32_t>(nodes.size())));
    Url out = nodes[idx];
    nodes[idx] = nodes.back();
    nodes.pop_back();
    return out;
}

void QueryPlan::Prepare() {
    if (prepared_) {
        return;
    }
    prepared_ = true;
    if (sort_nodes_) {
        std::sort(nodes_.begin(), nodes_.end());
    }
}

Url FirstNodeWithSeed(std::vector<Url> nodes, std::int64_t seed) {
    nodes = SortAndDedupeNodes(std::move(nodes));
    if (nodes.empty()) {
        return {};
    }
    detail::GoRandom rng(seed);
    return nodes[static_cast<std::size_t>(rng.Int31n(static_cast<std::int32_t>(nodes.size())))];
}

std::vector<Url> SortAndDedupeNodes(std::vector<Url> nodes) {
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    return nodes;
}

} // namespace scylladb::alternator
