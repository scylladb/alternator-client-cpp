#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace scylladb::alternator {

std::int64_t Murmur3H1(const std::vector<std::uint8_t>& data);
std::int64_t Murmur3H1(std::string_view data);

} // namespace scylladb::alternator
