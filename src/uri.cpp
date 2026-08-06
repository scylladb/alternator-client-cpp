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

#include <scylladb/alternator/uri.h>

#include <sstream>
#include <utility>

namespace scylladb::alternator {
namespace {

std::string FormatHostForAuthority(const std::string& host) {
    if (host.find(':') != std::string::npos && (host.empty() || host.front() != '[')) {
        return "[" + host + "]";
    }
    return host;
}

} // namespace

Url::Url(std::string scheme_value, std::string host_value, std::uint16_t port_value)
    : scheme(std::move(scheme_value))
    , host(std::move(host_value))
    , port(port_value) {}

Url Url::FromHostPort(std::string scheme, std::string host, std::uint16_t port) {
    return Url(std::move(scheme), std::move(host), port);
}

bool Url::Empty() const {
    return host.empty();
}

std::string Url::Authority() const {
    std::ostringstream out;
    out << FormatHostForAuthority(host);
    if (port != 0) {
        out << ':' << port;
    }
    return out.str();
}

std::string Url::ToString() const {
    if (host.empty()) {
        return {};
    }

    std::ostringstream out;
    out << scheme << "://" << Authority();
    if (!path.empty()) {
        out << path;
    }
    if (!query.empty()) {
        out << '?' << query;
    }
    return out.str();
}

Url Url::WithPathAndQuery(std::string path_value, std::string query_value) const {
    Url out = *this;
    out.path = std::move(path_value);
    out.query = std::move(query_value);
    return out;
}

bool operator==(const Url& lhs, const Url& rhs) {
    return lhs.scheme == rhs.scheme &&
           lhs.host == rhs.host &&
           lhs.port == rhs.port &&
           lhs.path == rhs.path &&
           lhs.query == rhs.query;
}

bool operator!=(const Url& lhs, const Url& rhs) {
    return !(lhs == rhs);
}

bool operator<(const Url& lhs, const Url& rhs) {
    return lhs.ToString() < rhs.ToString();
}

} // namespace scylladb::alternator
