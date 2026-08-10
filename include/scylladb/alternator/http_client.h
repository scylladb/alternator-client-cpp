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
};

class CurlHttpClient final : public HttpClient {
public:
    explicit CurlHttpClient(Config config);
    ~CurlHttpClient() override;

    [[nodiscard]] HttpResponse Get(const Url& url) const override;

private:
    Config config_;
    mutable std::mutex mutex_;
    mutable void* reusable_handle_ = nullptr;
};

std::shared_ptr<HttpClient> NewDefaultHttpClient(const Config& config);

} // namespace scylladb::alternator
