#pragma once

#include <memory>
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

    [[nodiscard]] HttpResponse Get(const Url& url) const override;

private:
    Config config_;
};

std::shared_ptr<HttpClient> NewDefaultHttpClient(const Config& config);

} // namespace scylladb::alternator
