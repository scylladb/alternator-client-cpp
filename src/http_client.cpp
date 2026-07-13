#include <scylladb/alternator/http_client.h>

#include "http_compression.h"

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
#include <curl/curl.h>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL
#include <openssl/ssl.h>
#endif
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace scylladb::alternator {

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
namespace {

std::once_flag curl_init_once;

void EnsureCurlInitialized() {
    std::call_once(curl_init_once, [] {
        const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK) {
            throw std::runtime_error("curl_global_init failed");
        }
    });
}

std::size_t WriteBody(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

std::size_t WriteHeader(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::string*>(userdata);
    headers->append(ptr, size * nmemb);
    return size * nmemb;
}

void SetDuration(CURL* curl, CURLoption option, std::chrono::milliseconds value) {
    if (value > std::chrono::milliseconds::zero()) {
        curl_easy_setopt(curl, option, static_cast<long>(value.count()));
    }
}

bool CurlUsesOpenSslBackend() {
    const auto* info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr || info->ssl_version == nullptr) {
        return false;
    }
    const std::string ssl_version = info->ssl_version;
    return ssl_version.find("OpenSSL") != std::string::npos ||
           ssl_version.find("LibreSSL") != std::string::npos ||
           ssl_version.find("BoringSSL") != std::string::npos;
}

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL
CURLcode ConfigureSslContext(CURL*, void* ssl_context, void* userdata) {
    const auto* config = static_cast<const Config*>(userdata);
    if (config == nullptr || ssl_context == nullptr) {
        return CURLE_OK;
    }

    auto* ctx = static_cast<SSL_CTX*>(ssl_context);
    if (!config->tls_session_cache_enabled) {
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
        return CURLE_OK;
    }

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_cache_size(ctx, static_cast<long>(config->tls_session_cache_size));
    SSL_CTX_set_timeout(ctx, static_cast<long>(config->tls_session_timeout.count()));
    return CURLE_OK;
}
#endif

void ConfigureCurlForGet(
    CURL* curl,
    const Url& url,
    const Config& config,
    std::string& body,
    std::string& response_headers) {
    curl_easy_reset(curl);

    const auto url_string = url.ToString();
    curl_easy_setopt(curl, CURLOPT_URL, url_string.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &WriteHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, config.tls_session_cache_enabled ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, static_cast<long>(config.max_connections));
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, config.reuse_discovery_connections ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, config.reuse_discovery_connections ? 0L : 1L);

    SetDuration(curl, CURLOPT_TIMEOUT_MS, config.http_client_timeout);
    SetDuration(curl, CURLOPT_CONNECTTIMEOUT_MS, config.connect_timeout);

    if (!config.user_agent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, config.user_agent.c_str());
    }

    if (!config.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!config.ca_file.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, config.ca_file.c_str());
    }
    if (!config.client_certificate_file.empty()) {
        curl_easy_setopt(curl, CURLOPT_SSLCERT, config.client_certificate_file.c_str());
    }
    if (!config.client_private_key_file.empty()) {
        curl_easy_setopt(curl, CURLOPT_SSLKEY, config.client_private_key_file.c_str());
    }

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL
    if (CurlUsesOpenSslBackend()) {
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, &ConfigureSslContext);
        curl_easy_setopt(curl, CURLOPT_SSL_CTX_DATA, &config);
    }
#endif
}

HttpResponse PerformCurlGet(CURL* curl, const Url& url, const Config& config) {
    std::string body;
    std::string response_headers;
    ConfigureCurlForGet(curl, url, config, body, response_headers);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(
        headers,
        config.reuse_discovery_connections ? "Connection: keep-alive" : "Connection: close");
    const auto accept_encoding_value = detail::BuildAcceptEncodingValue(config.content_encoding_decoders);
    if (!accept_encoding_value.empty()) {
        const auto accept_encoding = "Accept-Encoding: " + accept_encoding_value;
        headers = curl_slist_append(headers, accept_encoding.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const auto code = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    if (code != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(code));
    }

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    body = detail::DecodeHttpResponseBody(
        std::move(body),
        detail::FindHttpHeaderValue(response_headers, "content-encoding"),
        config.content_encoding_decoders);
    return HttpResponse{status_code, std::move(body)};
}

} // namespace

CurlHttpClient::CurlHttpClient(Config config)
    : config_(std::move(config)) {
    EnsureCurlInitialized();
}

CurlHttpClient::~CurlHttpClient() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reusable_handle_ != nullptr) {
        curl_easy_cleanup(static_cast<CURL*>(reusable_handle_));
        reusable_handle_ = nullptr;
    }
}

HttpResponse CurlHttpClient::Get(const Url& url) const {
    EnsureCurlInitialized();

    if (config_.reuse_discovery_connections) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reusable_handle_ == nullptr) {
            reusable_handle_ = curl_easy_init();
            if (reusable_handle_ == nullptr) {
                throw std::runtime_error("curl_easy_init failed");
            }
        }
        return PerformCurlGet(static_cast<CURL*>(reusable_handle_), url, config_);
    }

    CURL* raw = curl_easy_init();
    if (raw == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);
    return PerformCurlGet(curl.get(), url, config_);
}

#else
namespace {

class FdGuard {
public:
    explicit FdGuard(int fd)
        : fd_(fd) {}

    ~FdGuard() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                close(fd_);
            }
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const {
        return fd_;
    }

private:
    int fd_ = -1;
};

std::string ReadAll(int fd) {
    std::string data;
    char buffer[4096];
    while (true) {
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("recv failed");
        }
        data.append(buffer, static_cast<std::size_t>(n));
    }
    return data;
}

void SendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("send failed");
        }
        sent += static_cast<std::size_t>(n);
    }
}

FdGuard ConnectTcp(const Url& url) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* raw = nullptr;
    const auto service = std::to_string(url.port);
    const int code = getaddrinfo(url.host.c_str(), service.c_str(), &hints, &raw);
    if (code != 0) {
        throw std::runtime_error(gai_strerror(code));
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(raw, &freeaddrinfo);

    for (auto* it = results.get(); it != nullptr; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        FdGuard guard(fd);
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            return guard;
        }
    }
    throw std::runtime_error("connect failed");
}

HttpResponse ParseHttpResponse(
    const std::string& raw,
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders) {
    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::runtime_error("invalid HTTP response");
    }

    const auto status_line_end = raw.find("\r\n");
    if (status_line_end == std::string::npos || status_line_end > header_end) {
        throw std::runtime_error("invalid HTTP status line");
    }

    const auto status_line = raw.substr(0, status_line_end);
    std::istringstream status_stream(status_line);
    std::string http_version;
    long status_code = 0;
    status_stream >> http_version >> status_code;
    if (http_version.rfind("HTTP/", 0) != 0 || status_code == 0) {
        throw std::runtime_error("invalid HTTP status line");
    }

    auto body = raw.substr(header_end + 4);
    body = detail::DecodeHttpResponseBody(
        std::move(body),
        detail::FindHttpHeaderValue(raw.substr(0, header_end), "content-encoding"),
        content_encoding_decoders);
    return HttpResponse{status_code, std::move(body)};
}

} // namespace

CurlHttpClient::CurlHttpClient(Config config)
    : config_(std::move(config)) {}

CurlHttpClient::~CurlHttpClient() = default;

HttpResponse CurlHttpClient::Get(const Url& url) const {
    if (url.scheme != "http") {
        throw std::runtime_error("alternator_client_cpp was built without libcurl support; https is unavailable");
    }

    const auto path = url.path.empty() ? "/" : url.path;
    const auto target = url.query.empty() ? path : path + "?" + url.query;
    std::ostringstream request;
    request << "GET " << target << " HTTP/1.1\r\n"
            << "Host: " << url.Authority() << "\r\n"
            << "Connection: close\r\n";
    const auto accept_encoding_value = detail::BuildAcceptEncodingValue(config_.content_encoding_decoders);
    if (!accept_encoding_value.empty()) {
        request << "Accept-Encoding: " << accept_encoding_value << "\r\n";
    }
    if (!config_.user_agent.empty()) {
        request << "User-Agent: " << config_.user_agent << "\r\n";
    }
    request << "\r\n";

    auto fd = ConnectTcp(url);
    SendAll(fd.get(), request.str());
    return ParseHttpResponse(
        ReadAll(fd.get()),
        config_.content_encoding_decoders);
}

#endif

std::shared_ptr<HttpClient> NewDefaultHttpClient(const Config& config) {
    return std::make_shared<CurlHttpClient>(config);
}

} // namespace scylladb::alternator
