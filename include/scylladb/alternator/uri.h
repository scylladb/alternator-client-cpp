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
#include <string>

namespace scylladb::alternator {

class Url {
public:
    Url() = default;
    Url(std::string scheme, std::string host, std::uint16_t port);

    static Url FromHostPort(std::string scheme, std::string host, std::uint16_t port);

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] std::string Authority() const;
    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] Url WithPathAndQuery(std::string path, std::string query = {}) const;

    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string path;
    std::string query;
};

bool operator==(const Url& lhs, const Url& rhs);
bool operator!=(const Url& lhs, const Url& rhs);
bool operator<(const Url& lhs, const Url& rhs);

} // namespace scylladb::alternator
