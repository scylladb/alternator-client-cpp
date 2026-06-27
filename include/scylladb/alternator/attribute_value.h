#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace scylladb::alternator {

enum class AttributeValueType {
    String,
    Number,
    Binary,
};

std::int64_t HashAttributeValue(AttributeValueType type, std::string_view value);
std::int64_t HashBinaryAttributeValue(const std::vector<std::uint8_t>& value);

} // namespace scylladb::alternator
