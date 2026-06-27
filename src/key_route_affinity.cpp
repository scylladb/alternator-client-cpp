#include <scylladb/alternator/key_route_affinity.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace scylladb::alternator {

PartitionKeyMetadata::PartitionKeyMetadata(std::map<std::string, std::string> partition_key_by_table)
    : partition_key_by_table_(std::move(partition_key_by_table)) {}

void PartitionKeyMetadata::SetPartitionKeyName(const std::string& table_name, std::string partition_key_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    partition_key_by_table_[table_name] = std::move(partition_key_name);
}

std::string PartitionKeyMetadata::GetPartitionKeyName(const std::string& table_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = partition_key_by_table_.find(table_name);
    if (it == partition_key_by_table_.end()) {
        return {};
    }
    return it->second;
}

std::map<std::string, std::string> PartitionKeyMetadata::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partition_key_by_table_;
}

AttributeValue AttributeValue::String(std::string value) {
    AttributeValue out;
    out.type = AttributeValueType::String;
    out.text = std::move(value);
    return out;
}

AttributeValue AttributeValue::Number(std::string value) {
    AttributeValue out;
    out.type = AttributeValueType::Number;
    out.text = std::move(value);
    return out;
}

AttributeValue AttributeValue::Binary(std::vector<std::uint8_t> value) {
    AttributeValue out;
    out.type = AttributeValueType::Binary;
    out.binary = std::move(value);
    return out;
}

std::int64_t AttributeValue::Hash() const {
    if (type == AttributeValueType::Binary) {
        return HashBinaryAttributeValue(binary);
    }
    return HashAttributeValue(type, text);
}

BatchWriteOperation BatchWriteOperation::Put(std::string table_name, AttributeMap item) {
    return BatchWriteOperation{std::move(table_name), std::move(item)};
}

BatchWriteOperation BatchWriteOperation::Delete(std::string table_name, AttributeMap key) {
    return BatchWriteOperation{std::move(table_name), std::move(key)};
}

bool DoesWriteNeedReadBeforeWrite(WriteOperationKind kind, const WriteOperationOptions& options) {
    switch (kind) {
    case WriteOperationKind::PutItem:
        return options.has_expected ||
               options.has_condition_expression ||
               options.return_values == ReturnValues::AllOld;

    case WriteOperationKind::DeleteItem:
        return options.has_expected ||
               options.has_condition_expression ||
               options.return_values == ReturnValues::AllOld;

    case WriteOperationKind::UpdateItem:
        if (options.has_update_expression || options.has_condition_expression || options.has_expected) {
            return true;
        }
        switch (options.return_values) {
        case ReturnValues::NotSet:
        case ReturnValues::None:
        case ReturnValues::UpdatedNew:
            break;
        case ReturnValues::AllOld:
        case ReturnValues::UpdatedOld:
        case ReturnValues::AllNew:
            return true;
        }
        for (const auto& update : options.attribute_updates) {
            if (update.action == AttributeAction::Add) {
                return true;
            }
            if (update.action == AttributeAction::Delete && update.has_value) {
                return true;
            }
        }
        return false;
    }
    return false;
}

bool ShouldUseKeyRouteAffinity(KeyRouteAffinityMode mode,
                               WriteOperationKind kind,
                               const WriteOperationOptions& options) {
    switch (mode) {
    case KeyRouteAffinityMode::None:
        return false;
    case KeyRouteAffinityMode::ReadBeforeWrite:
        return DoesWriteNeedReadBeforeWrite(kind, options);
    case KeyRouteAffinityMode::AnyWrite:
        return true;
    }
    return false;
}

std::int64_t HashPartitionKey(const AttributeMap& values,
                              const std::string& table_name,
                              const PartitionKeyMetadata& metadata) {
    const auto key_name = metadata.GetPartitionKeyName(table_name);
    if (key_name.empty()) {
        throw std::invalid_argument("partition key information not found for table " + table_name);
    }
    const auto it = values.find(key_name);
    if (it == values.end()) {
        throw std::invalid_argument("value for partition key " + key_name + " not found");
    }
    return it->second.Hash();
}

namespace {

std::vector<Url> DrainPlan(QueryPlan plan) {
    std::vector<Url> out;
    for (auto node = plan.Next(); !node.Empty(); node = plan.Next()) {
        out.push_back(std::move(node));
    }
    return out;
}

void AppendMissingNodes(std::vector<Url>& out, std::vector<Url> nodes) {
    for (auto& node : nodes) {
        if (!node.Empty() && std::find(out.begin(), out.end(), node) == out.end()) {
            out.push_back(std::move(node));
        }
    }
}

std::vector<Url> NodesMissingFrom(std::vector<Url> nodes, const std::vector<Url>& present) {
    std::vector<Url> out;
    for (auto& node : nodes) {
        if (!node.Empty() &&
            std::find(present.begin(), present.end(), node) == present.end() &&
            std::find(out.begin(), out.end(), node) == out.end()) {
            out.push_back(std::move(node));
        }
    }
    return out;
}

std::vector<std::int64_t> BatchWriteHashes(const std::vector<BatchWriteOperation>& operations,
                                           const PartitionKeyMetadata& metadata) {
    std::vector<std::int64_t> hashes;
    for (const auto& operation : operations) {
        const auto key_name = metadata.GetPartitionKeyName(operation.table_name);
        if (key_name.empty()) {
            continue;
        }
        const auto value = operation.values.find(key_name);
        if (value == operation.values.end()) {
            continue;
        }
        hashes.push_back(value->second.Hash());
    }
    return hashes;
}

std::vector<Url> QueryPlanNodesForHashes(const NodesSource& nodes, const std::vector<std::int64_t>& hashes) {
    std::vector<Url> out;
    for (const auto hash : hashes) {
        AppendMissingNodes(out, nodes.GetQueryPlanNodesForHash(hash));
    }
    if (out.empty()) {
        return nodes.GetQueryPlanNodes();
    }
    return out;
}

std::vector<Url> AffinityNodes(const NodesSource& nodes) {
    auto active_nodes = nodes.GetActiveNodes();
    if (!active_nodes.empty()) {
        return SortAndDedupeNodes(std::move(active_nodes));
    }
    return nodes.GetQueryPlanNodes();
}

std::vector<Url> SelectBatchWritePreferredNodesForCandidates(const std::vector<Url>& candidate_nodes,
                                                             const std::vector<BatchWriteOperation>& operations,
                                                             const PartitionKeyMetadata& metadata) {
    if (candidate_nodes.empty()) {
        throw std::invalid_argument("batch write request does not have candidate nodes");
    }

    std::map<Url, int> votes;
    for (const auto& operation : operations) {
        const auto key_name = metadata.GetPartitionKeyName(operation.table_name);
        if (key_name.empty()) {
            continue;
        }
        const auto value = operation.values.find(key_name);
        if (value == operation.values.end()) {
            continue;
        }

        const auto node = FirstNodeWithSeed(candidate_nodes, value->second.Hash());
        if (!node.Empty()) {
            ++votes[node];
        }
    }

    if (votes.empty()) {
        throw std::invalid_argument("batch write request does not have usable preferred nodes");
    }

    std::vector<Url> preferred_nodes;
    preferred_nodes.reserve(votes.size());
    for (const auto& [node, count] : votes) {
        if (count > 0) {
            preferred_nodes.push_back(node);
        }
    }

    std::sort(preferred_nodes.begin(), preferred_nodes.end(), [&](const Url& lhs, const Url& rhs) {
        const auto lhs_votes = votes.at(lhs);
        const auto rhs_votes = votes.at(rhs);
        if (lhs_votes != rhs_votes) {
            return lhs_votes > rhs_votes;
        }
        return lhs.ToString() < rhs.ToString();
    });
    return preferred_nodes;
}

} // namespace

QueryPlan QueryPlanForPartitionKey(const NodesSource& nodes,
                                   const AttributeMap& values,
                                   const std::string& table_name,
                                   const PartitionKeyMetadata& metadata) {
    const auto seed = HashPartitionKey(values, table_name, metadata);
    auto active_nodes = nodes.GetActiveNodes();
    auto query_plan_nodes = nodes.GetQueryPlanNodesForHash(seed);
    if (active_nodes.empty()) {
        return QueryPlan::WithSortedSeed(std::move(query_plan_nodes), seed);
    }

    active_nodes = SortAndDedupeNodes(std::move(active_nodes));
    auto ordered_nodes = NodesMissingFrom(std::move(query_plan_nodes), active_nodes);
    AppendMissingNodes(ordered_nodes, DrainPlan(QueryPlan::WithSortedSeed(std::move(active_nodes), seed)));
    return QueryPlan::FromOrderedNodes(std::move(ordered_nodes));
}

std::vector<Url> SelectBatchWritePreferredNodes(const NodesSource& nodes,
                                                const std::vector<BatchWriteOperation>& operations,
                                                const PartitionKeyMetadata& metadata) {
    return SelectBatchWritePreferredNodesForCandidates(AffinityNodes(nodes), operations, metadata);
}

QueryPlan QueryPlanForBatchWrite(const NodesSource& nodes,
                                 const std::vector<BatchWriteOperation>& operations,
                                 const PartitionKeyMetadata& metadata) {
    auto active_nodes = nodes.GetActiveNodes();
    auto query_plan_nodes = QueryPlanNodesForHashes(nodes, BatchWriteHashes(operations, metadata));
    if (active_nodes.empty()) {
        auto candidate_nodes = std::move(query_plan_nodes);
        return QueryPlan::WithPreferredNodes(
            candidate_nodes,
            SelectBatchWritePreferredNodesForCandidates(candidate_nodes, operations, metadata));
    }

    active_nodes = SortAndDedupeNodes(std::move(active_nodes));
    auto ordered_nodes = NodesMissingFrom(std::move(query_plan_nodes), active_nodes);
    AppendMissingNodes(ordered_nodes, SelectBatchWritePreferredNodesForCandidates(active_nodes, operations, metadata));
    AppendMissingNodes(ordered_nodes, active_nodes);
    return QueryPlan::FromOrderedNodes(std::move(ordered_nodes));
}

} // namespace scylladb::alternator
