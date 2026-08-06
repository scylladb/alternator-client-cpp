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
