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

#include <scylladb/alternator/http_client.h>

#include "http_compression.h"

#include <netdb.h>
#include <sys/socket.h>

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
#include <curl/curl.h>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL
#include <openssl/ssl.h>
#endif
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace scylladb::alternator {
namespace {

constexpr std::size_t kMaxDiscoveryResponseHeaderBytes = 64U * 1024U;

std::vector<std::string> ResolveAddresses(const Url& url) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* raw = nullptr;
    const auto service = std::to_string(url.port);
    const int code = getaddrinfo(url.host.c_str(), service.c_str(), &hints, &raw);
    if (code != 0) {
        throw std::runtime_error("DNS resolution failed for " + url.host + ": " + gai_strerror(code));
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(raw, &freeaddrinfo);

    std::vector<std::string> addresses;
    std::unordered_set<std::string> seen_addresses;
    for (auto* it = results.get(); it != nullptr; it = it->ai_next) {
        char host[NI_MAXHOST]{};
        if (getnameinfo(it->ai_addr,
                        it->ai_addrlen,
                        host,
                        sizeof(host),
                        nullptr,
                        0,
                        NI_NUMERICHOST) != 0) {
            continue;
        }
        std::string address(host);
        if (seen_addresses.insert(address).second) {
            addresses.push_back(std::move(address));
        }
    }
    if (addresses.empty()) {
        throw std::runtime_error("DNS resolution returned no usable addresses for " + url.host);
    }
    return addresses;
}

std::chrono::milliseconds ShorterPositiveTimeout(
    std::chrono::milliseconds left,
    std::chrono::milliseconds right) {
    if (left <= std::chrono::milliseconds::zero()) {
        return right;
    }
    if (right <= std::chrono::milliseconds::zero()) {
        return left;
    }
    return std::min(left, right);
}

std::chrono::milliseconds EffectiveDiscoveryTimeout(
    const Config& config,
    std::chrono::milliseconds call_timeout = std::chrono::milliseconds::zero()) {
    const auto request_timeout = config.http_client_timeout;
    const auto safety_timeout = config.discovery_attempt_timeout;
    return ShorterPositiveTimeout(
        ShorterPositiveTimeout(request_timeout, safety_timeout),
        call_timeout);
}

#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
std::size_t MaxRawDiscoveryResponseBytes(const Config& config) {
    if (config.max_discovery_response_bytes >
        std::numeric_limits<std::size_t>::max() -
            kMaxDiscoveryResponseHeaderBytes) {
        return std::numeric_limits<std::size_t>::max();
    }
    return config.max_discovery_response_bytes +
           kMaxDiscoveryResponseHeaderBytes;
}
#endif

} // namespace

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

struct BoundedWriteContext {
    std::string* output = nullptr;
    std::size_t limit = 0;
    bool exceeded = false;
    bool failed = false;
};

std::size_t WriteBounded(
    char* ptr,
    std::size_t size,
    std::size_t nmemb,
    void* userdata) noexcept {
    auto* context = static_cast<BoundedWriteContext*>(userdata);
    if (context == nullptr || context->output == nullptr ||
        (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size)) {
        if (context != nullptr) {
            context->failed = true;
        }
        return 0;
    }

    const auto bytes = size * nmemb;
    if (bytes > context->limit ||
        context->output->size() > context->limit - bytes) {
        context->exceeded = true;
        return 0;
    }
    try {
        context->output->append(ptr, bytes);
    } catch (...) {
        context->failed = true;
        return 0;
    }
    return bytes;
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
    const std::string& resolved_address,
    bool force_fresh_connection,
    std::chrono::milliseconds request_timeout,
    BoundedWriteContext& body_context,
    BoundedWriteContext& response_headers_context,
    curl_slist*& resolve_entries) {
    curl_easy_reset(curl);

    const auto url_string = url.ToString();
    curl_easy_setopt(curl, CURLOPT_URL, url_string.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteBounded);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_context);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &WriteBounded);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers_context);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_SESSIONID_CACHE, config.tls_session_cache_enabled ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, static_cast<long>(config.max_connections));
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, config.reuse_discovery_connections ? 0L : 1L);
    curl_easy_setopt(
        curl,
        CURLOPT_FRESH_CONNECT,
        !config.reuse_discovery_connections || force_fresh_connection ? 1L : 0L);

    // Address-level discovery must connect to the selected DNS result. A
    // process proxy setting would otherwise send the logical hostname to the
    // proxy and make every resolved-address attempt use the same proxy
    // connection instead of resolved_address.
    if (!resolved_address.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
    }

    if (!resolved_address.empty() && resolved_address != url.host) {
        auto curl_address = resolved_address;
        if (curl_address.find(':') != std::string::npos && curl_address.front() != '[') {
            curl_address = "[" + curl_address + "]";
        }
        const auto entry = url.host + ":" + std::to_string(url.port) + ":" + curl_address;
        resolve_entries = curl_slist_append(resolve_entries, entry.c_str());
        if (resolve_entries == nullptr) {
            throw std::runtime_error("curl_slist_append failed for resolved address");
        }
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_entries);
    }

    SetDuration(curl, CURLOPT_TIMEOUT_MS, request_timeout);
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

HttpResponse PerformCurlGet(
    CURL* curl,
    const Url& url,
    const Config& config,
    const std::string& resolved_address,
    bool force_fresh_connection,
    std::chrono::milliseconds request_timeout) {
    std::string body;
    std::string response_headers;
    BoundedWriteContext body_context{
        &body,
        config.max_discovery_response_bytes,
    };
    BoundedWriteContext response_headers_context{
        &response_headers,
        kMaxDiscoveryResponseHeaderBytes,
    };
    curl_slist* resolve_entries = nullptr;
    ConfigureCurlForGet(
        curl,
        url,
        config,
        resolved_address,
        force_fresh_connection,
        request_timeout,
        body_context,
        response_headers_context,
        resolve_entries);

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
    curl_easy_setopt(curl, CURLOPT_RESOLVE, nullptr);
    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    if (resolve_entries != nullptr) {
        curl_slist_free_all(resolve_entries);
    }
    if (code != CURLE_OK) {
        if (body_context.exceeded) {
            throw std::runtime_error("HTTP response body exceeds max_discovery_response_bytes");
        }
        if (response_headers_context.exceeded) {
            throw std::runtime_error("HTTP response headers exceed discovery limit");
        }
        if (body_context.failed || response_headers_context.failed) {
            throw std::runtime_error("failed to buffer HTTP response");
        }
        throw std::runtime_error(curl_easy_strerror(code));
    }

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    body = detail::DecodeHttpResponseBody(
        std::move(body),
        detail::FindHttpHeaderValue(response_headers, "content-encoding"),
        config.content_encoding_decoders,
        config.max_discovery_response_bytes);
    return HttpResponse{status_code, std::move(body)};
}

} // namespace

CurlHttpClient::CurlHttpClient(Config config)
    : config_(std::move(config)) {
    EnsureCurlInitialized();
}

CurlHttpClient::~CurlHttpClient() {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (reusable_handle_ != nullptr) {
        curl_easy_cleanup(static_cast<CURL*>(reusable_handle_));
        reusable_handle_ = nullptr;
    }
}

HttpResponse CurlHttpClient::Get(const Url& url) const {
    return GetResolved(url, {});
}

std::vector<std::string> CurlHttpClient::Resolve(const Url& url) const {
    return ResolveAddresses(url);
}

HttpResponse CurlHttpClient::GetResolved(
    const Url& url,
    const std::string& resolved_address) const {
    return GetResolvedWithTimeout(
        url,
        resolved_address,
        EffectiveDiscoveryTimeout(config_));
}

HttpResponse CurlHttpClient::GetResolvedWithTimeout(
    const Url& url,
    const std::string& resolved_address,
    std::chrono::milliseconds timeout) const {
    EnsureCurlInitialized();
    const auto request_timeout = EffectiveDiscoveryTimeout(config_, timeout);
    const auto started = std::chrono::steady_clock::now();

    const auto remaining_timeout = [&]() {
        if (request_timeout <= std::chrono::milliseconds::zero()) {
            return request_timeout;
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed >= request_timeout) {
            throw std::runtime_error("HTTP client wait timed out");
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            request_timeout - elapsed);
        if (remaining <= std::chrono::milliseconds::zero()) {
            remaining = std::chrono::milliseconds{1};
        }
        return remaining;
    };

    if (config_.reuse_discovery_connections) {
        std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
        if (request_timeout > std::chrono::milliseconds::zero()) {
            if (!lock.try_lock_for(request_timeout)) {
                throw std::runtime_error("HTTP client wait timed out");
            }
        } else {
            lock.lock();
        }
        if (reusable_handle_ == nullptr) {
            reusable_handle_ = curl_easy_init();
            if (reusable_handle_ == nullptr) {
                throw std::runtime_error("curl_easy_init failed");
            }
        }
        const bool force_fresh_connection =
            !resolved_address.empty() &&
            reusable_resolved_address_ != resolved_address;
        reusable_resolved_address_ = resolved_address;
        return PerformCurlGet(
            static_cast<CURL*>(reusable_handle_),
            url,
            config_,
            resolved_address,
            force_fresh_connection,
            remaining_timeout());
    }

    CURL* raw = curl_easy_init();
    if (raw == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);
    return PerformCurlGet(
        curl.get(),
        url,
        config_,
        resolved_address,
        false,
        remaining_timeout());
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

using IoClock = std::chrono::steady_clock;
using IoDeadline = std::optional<IoClock::time_point>;

IoDeadline MakeDeadline(
    IoClock::time_point started,
    std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::nullopt;
    }
    const auto maximum_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        IoClock::time_point::max() - started);
    if (timeout >= maximum_timeout) {
        return IoClock::time_point::max();
    }
    return started + timeout;
}

IoDeadline EarlierDeadline(const IoDeadline& left, const IoDeadline& right) {
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    return std::min(*left, *right);
}

int RemainingTimeoutMilliseconds(const IoDeadline& deadline) {
    if (!deadline) {
        return -1;
    }
    const auto remaining = *deadline - IoClock::now();
    if (remaining <= IoClock::duration::zero()) {
        return 0;
    }
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining) {
        milliseconds += std::chrono::milliseconds{1};
    }
    if (milliseconds.count() > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(milliseconds.count());
}

void WaitForSocket(int fd, short events, const IoDeadline& deadline, const char* operation) {
    while (true) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        const int result = poll(
            &descriptor,
            1,
            RemainingTimeoutMilliseconds(deadline));
        if (result > 0) {
            return;
        }
        if (result == 0) {
            throw std::runtime_error(std::string(operation) + " timed out");
        }
        if (errno != EINTR) {
            throw std::runtime_error(
                std::string(operation) + " poll failed: " + std::strerror(errno));
        }
    }
}

void SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        throw std::runtime_error("failed to configure nonblocking socket");
    }
}

std::string ReadAll(
    int fd,
    const IoDeadline& deadline,
    std::size_t maximum_size) {
    std::string data;
    char buffer[4096];
    while (true) {
        WaitForSocket(fd, POLLIN, deadline, "HTTP receive");
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error("recv failed: " + std::string(std::strerror(errno)));
        }
        const auto received = static_cast<std::size_t>(n);
        if (received > maximum_size || data.size() > maximum_size - received) {
            throw std::runtime_error("HTTP response exceeds discovery size limit");
        }
        data.append(buffer, received);
    }
    return data;
}

void SendAll(int fd, const std::string& data, const IoDeadline& deadline) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        WaitForSocket(fd, POLLOUT, deadline, "HTTP send");
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto n = send(
            fd,
            data.data() + sent,
            data.size() - sent,
            send_flags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error("send failed: " + std::string(std::strerror(errno)));
        }
        if (n == 0) {
            throw std::runtime_error("send returned no progress");
        }
        sent += static_cast<std::size_t>(n);
    }
}

FdGuard ConnectTcp(
    const Url& url,
    const std::string& resolved_address,
    const IoDeadline& deadline) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (!resolved_address.empty()) {
        hints.ai_flags = AI_NUMERICHOST;
    }

    addrinfo* raw = nullptr;
    const auto service = std::to_string(url.port);
    const auto& connect_host = resolved_address.empty() ? url.host : resolved_address;
    const int code = getaddrinfo(connect_host.c_str(), service.c_str(), &hints, &raw);
    if (code != 0) {
        throw std::runtime_error(gai_strerror(code));
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> results(raw, &freeaddrinfo);

    std::string last_error = "connect failed";
    for (auto* it = results.get(); it != nullptr; it = it->ai_next) {
        if (deadline && IoClock::now() >= *deadline) {
            throw std::runtime_error("connect timed out");
        }
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        FdGuard guard(fd);
        try {
            SetNonBlocking(fd);
        } catch (const std::exception& error) {
            last_error = error.what();
            continue;
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            return guard;
        }
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            last_error = "connect failed: " + std::string(std::strerror(errno));
            continue;
        }

        WaitForSocket(fd, POLLOUT, deadline, "connect");
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        if (getsockopt(
                fd,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &socket_error_size) != 0) {
            last_error = "getsockopt(SO_ERROR) failed: " + std::string(std::strerror(errno));
            continue;
        }
        if (socket_error == 0) {
            return guard;
        }
        last_error = "connect failed: " + std::string(std::strerror(socket_error));
    }
    throw std::runtime_error(last_error);
}

HttpResponse ParseHttpResponse(
    const std::string& raw,
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders,
    std::size_t maximum_decoded_size) {
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
        content_encoding_decoders,
        maximum_decoded_size);
    return HttpResponse{status_code, std::move(body)};
}

} // namespace

CurlHttpClient::CurlHttpClient(Config config)
    : config_(std::move(config)) {}

CurlHttpClient::~CurlHttpClient() = default;

HttpResponse CurlHttpClient::Get(const Url& url) const {
    return GetResolved(url, {});
}

std::vector<std::string> CurlHttpClient::Resolve(const Url& url) const {
    return ResolveAddresses(url);
}

HttpResponse CurlHttpClient::GetResolved(
    const Url& url,
    const std::string& resolved_address) const {
    return GetResolvedWithTimeout(
        url,
        resolved_address,
        EffectiveDiscoveryTimeout(config_));
}

HttpResponse CurlHttpClient::GetResolvedWithTimeout(
    const Url& url,
    const std::string& resolved_address,
    std::chrono::milliseconds timeout) const {
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

    const auto started = IoClock::now();
    const auto request_deadline = MakeDeadline(
        started,
        EffectiveDiscoveryTimeout(config_, timeout));
    const auto connect_deadline = EarlierDeadline(
        MakeDeadline(started, config_.connect_timeout),
        request_deadline);
    auto fd = ConnectTcp(url, resolved_address, connect_deadline);
    SendAll(fd.get(), request.str(), request_deadline);
    auto response = ParseHttpResponse(
        ReadAll(
            fd.get(),
            request_deadline,
            MaxRawDiscoveryResponseBytes(config_)),
        config_.content_encoding_decoders,
        config_.max_discovery_response_bytes);
    return response;
}

#endif

std::vector<std::string> HttpClient::Resolve(const Url& url) const {
    return {url.host};
}

HttpResponse HttpClient::GetResolved(
    const Url& url,
    const std::string&) const {
    return Get(url);
}

HttpResponse HttpClient::GetResolvedWithTimeout(
    const Url& url,
    const std::string& resolved_address,
    std::chrono::milliseconds) const {
    return GetResolved(url, resolved_address);
}

std::shared_ptr<HttpClient> NewDefaultHttpClient(const Config& config) {
    return std::make_shared<CurlHttpClient>(config);
}

} // namespace scylladb::alternator
