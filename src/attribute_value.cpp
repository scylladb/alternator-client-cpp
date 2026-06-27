#include <scylladb/alternator/attribute_value.h>

#include <stdexcept>

#include <scylladb/alternator/murmur3.h>

namespace scylladb::alternator {
namespace {

constexpr std::uint8_t type_prefix_s = 0x01;
constexpr std::uint8_t type_prefix_n = 0x02;
constexpr std::uint8_t type_prefix_b = 0x03;

std::int64_t HashPrefixed(std::uint8_t prefix, std::string_view value) {
    std::vector<std::uint8_t> data;
    data.reserve(value.size() + 1);
    data.push_back(prefix);
    data.insert(data.end(), value.begin(), value.end());
    return Murmur3H1(data);
}

} // namespace

std::int64_t HashAttributeValue(AttributeValueType type, std::string_view value) {
    switch (type) {
    case AttributeValueType::String:
        return HashPrefixed(type_prefix_s, value);
    case AttributeValueType::Number:
        return HashPrefixed(type_prefix_n, value);
    case AttributeValueType::Binary:
        return HashPrefixed(type_prefix_b, value);
    }
    throw std::invalid_argument("unsupported attribute value type");
}

std::int64_t HashBinaryAttributeValue(const std::vector<std::uint8_t>& value) {
    std::vector<std::uint8_t> data;
    data.reserve(value.size() + 1);
    data.push_back(type_prefix_b);
    data.insert(data.end(), value.begin(), value.end());
    return Murmur3H1(data);
}

} // namespace scylladb::alternator
