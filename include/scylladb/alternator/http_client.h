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

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <scylladb/alternator/config.h>
#include <scylladb/alternator/uri.h>

namespace scylladb::alternator {

struct HttpResponse {
    long status_code = 0;
    std::string body;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;

    [[nodiscard]] virtual HttpResponse Get(const Url& url) const = 0;

    // Resolve every address for a logical endpoint. Discovery calls this for
    // each refresh so DNS entrypoints can change without pinning stale answers.
    // Custom clients may keep the default behavior and let Get() resolve the
    // hostname itself.
    [[nodiscard]] virtual std::vector<std::string> Resolve(const Url& url) const;

    // Connect to one resolved address while retaining url as the logical
    // endpoint. Implementations must preserve its Host header and, for HTTPS,
    // TLS SNI and certificate verification semantics.
    [[nodiscard]] virtual HttpResponse GetResolved(
        const Url& url,
        const std::string& resolved_address) const;
};

class CurlHttpClient final : public HttpClient {
public:
    explicit CurlHttpClient(Config config);
    ~CurlHttpClient() override;

    [[nodiscard]] HttpResponse Get(const Url& url) const override;
    [[nodiscard]] std::vector<std::string> Resolve(const Url& url) const override;
    [[nodiscard]] HttpResponse GetResolved(
        const Url& url,
        const std::string& resolved_address) const override;

private:
    Config config_;
    mutable std::mutex mutex_;
    mutable void* reusable_handle_ = nullptr;
    mutable std::string reusable_resolved_address_;
};

std::shared_ptr<HttpClient> NewDefaultHttpClient(const Config& config);

} // namespace scylladb::alternator
