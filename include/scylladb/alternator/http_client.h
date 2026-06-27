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
