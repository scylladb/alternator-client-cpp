#include <scylladb/alternator/http_client.h>

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL
#include <curl/curl.h>
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

void SetDuration(CURL* curl, CURLoption option, std::chrono::milliseconds value) {
    if (value > std::chrono::milliseconds::zero()) {
        curl_easy_setopt(curl, option, static_cast<long>(value.count()));
    }
}

} // namespace

CurlHttpClient::CurlHttpClient(Config config)
    : config_(std::move(config)) {
    EnsureCurlInitialized();
}

HttpResponse CurlHttpClient::Get(const Url& url) const {
    EnsureCurlInitialized();

    CURL* raw = curl_easy_init();
    if (raw == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);

    std::string body;
    const auto url_string = url.ToString();
    curl_easy_setopt(curl.get(), CURLOPT_URL, url_string.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &WriteBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);

    SetDuration(curl.get(), CURLOPT_TIMEOUT_MS, config_.http_client_timeout);
    SetDuration(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, config_.connect_timeout);

    if (!config_.user_agent.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, config_.user_agent.c_str());
    }

    if (!config_.verify_ssl) {
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (!config_.ca_file.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, config_.ca_file.c_str());
    }
    if (!config_.client_certificate_file.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_SSLCERT, config_.client_certificate_file.c_str());
    }
    if (!config_.client_private_key_file.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_SSLKEY, config_.client_private_key_file.c_str());
    }

    const auto code = curl_easy_perform(curl.get());
    if (code != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(code));
    }

    long status_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status_code);
    return HttpResponse{status_code, std::move(body)};
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

HttpResponse ParseHttpResponse(const std::string& raw) {
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

    return HttpResponse{status_code, raw.substr(header_end + 4)};
}

} // namespace

CurlHttpClient::CurlHttpClient(Config config)
    : config_(std::move(config)) {}

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
    if (!config_.user_agent.empty()) {
        request << "User-Agent: " << config_.user_agent << "\r\n";
    }
    request << "\r\n";

    auto fd = ConnectTcp(url);
    SendAll(fd.get(), request.str());
    return ParseHttpResponse(ReadAll(fd.get()));
}

#endif

std::shared_ptr<HttpClient> NewDefaultHttpClient(const Config& config) {
    return std::make_shared<CurlHttpClient>(config);
}

} // namespace scylladb::alternator
