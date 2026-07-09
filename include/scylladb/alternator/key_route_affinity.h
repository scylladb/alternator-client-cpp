#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <scylladb/alternator/attribute_value.h>
#include <scylladb/alternator/query_plan.h>

namespace scylladb::alternator {

enum class KeyRouteAffinityMode {
    None,
    ReadBeforeWrite,
    AnyWrite,
};

struct KeyRouteAffinityConfig {
    KeyRouteAffinityMode mode = KeyRouteAffinityMode::None;
    std::map<std::string, std::string> partition_key_by_table;
    std::uint32_t partition_key_discovery_attempts = 3;
    std::chrono::milliseconds partition_key_discovery_initial_backoff{100};
};

class PartitionKeyMetadata {
public:
    PartitionKeyMetadata() = default;
    explicit PartitionKeyMetadata(std::map<std::string, std::string> partition_key_by_table);

    void SetPartitionKeyName(const std::string& table_name, std::string partition_key_name);
    [[nodiscard]] std::string GetPartitionKeyName(const std::string& table_name) const;
    [[nodiscard]] std::map<std::string, std::string> Snapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> partition_key_by_table_;
};

struct AttributeValue {
    static AttributeValue String(std::string value);
    static AttributeValue Number(std::string value);
    static AttributeValue Binary(std::vector<std::uint8_t> value);

    [[nodiscard]] std::int64_t Hash() const;

    AttributeValueType type = AttributeValueType::String;
    std::string text;
    std::vector<std::uint8_t> binary;
};

using AttributeMap = std::map<std::string, AttributeValue>;

enum class WriteOperationKind {
    PutItem,
    UpdateItem,
    DeleteItem,
};

enum class ReturnValues {
    NotSet,
    None,
    AllOld,
    UpdatedOld,
    AllNew,
    UpdatedNew,
};

enum class AttributeAction {
    Put,
    Add,
    Delete,
};

struct AttributeUpdate {
    AttributeAction action = AttributeAction::Put;
    bool has_value = false;
};

struct WriteOperationOptions {
    bool has_update_expression = false;
    bool has_condition_expression = false;
    bool has_expected = false;
    ReturnValues return_values = ReturnValues::NotSet;
    std::vector<AttributeUpdate> attribute_updates;
};

struct BatchWriteOperation {
    static BatchWriteOperation Put(std::string table_name, AttributeMap item);
    static BatchWriteOperation Delete(std::string table_name, AttributeMap key);

    std::string table_name;
    AttributeMap values;
};

[[nodiscard]] bool DoesWriteNeedReadBeforeWrite(WriteOperationKind kind, const WriteOperationOptions& options);
[[nodiscard]] bool ShouldUseKeyRouteAffinity(KeyRouteAffinityMode mode,
                                             WriteOperationKind kind,
                                             const WriteOperationOptions& options);

[[nodiscard]] std::int64_t HashPartitionKey(const AttributeMap& values,
                                            const std::string& table_name,
                                            const PartitionKeyMetadata& metadata);
[[nodiscard]] QueryPlan QueryPlanForPartitionKey(const NodesSource& nodes,
                                                 const AttributeMap& values,
                                                 const std::string& table_name,
                                                 const PartitionKeyMetadata& metadata);

[[nodiscard]] std::vector<Url> SelectBatchWritePreferredNodes(const NodesSource& nodes,
                                                              const std::vector<BatchWriteOperation>& operations,
                                                              const PartitionKeyMetadata& metadata);
[[nodiscard]] QueryPlan QueryPlanForBatchWrite(const NodesSource& nodes,
                                               const std::vector<BatchWriteOperation>& operations,
                                               const PartitionKeyMetadata& metadata);

} // namespace scylladb::alternator
