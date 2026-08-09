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

#include <scylladb/alternator/live_nodes.h>

#include <gtest/gtest.h>

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL && SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#endif

using namespace scylladb::alternator;

namespace {

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL && SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL

std::string OpenSslError() {
    const auto code = ERR_get_error();
    if (code == 0) {
        return "unknown OpenSSL error";
    }
    char buffer[256]{};
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return buffer;
}

void RequireOpenSsl(int result, const std::string& operation) {
    if (result != 1) {
        throw std::runtime_error(operation + ": " + OpenSslError());
    }
}

using EvpPkeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using EvpPkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using X509Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using X509Extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
using SslContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Ssl = std::unique_ptr<SSL, decltype(&SSL_free)>;

struct GeneratedCertificate {
    EvpPkey key;
    X509Certificate certificate;
};

void AddExtension(X509* certificate, X509V3_CTX* context, int nid, const char* value) {
    X509Extension extension(
        X509V3_EXT_conf_nid(nullptr, context, nid, const_cast<char*>(value)),
        &X509_EXTENSION_free);
    if (!extension) {
        throw std::runtime_error("X509V3_EXT_conf_nid: " + OpenSslError());
    }
    RequireOpenSsl(
        X509_add_ext(certificate, extension.get(), -1),
        "X509_add_ext");
}

GeneratedCertificate GenerateLocalhostCertificate() {
    EvpPkeyContext key_context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &EVP_PKEY_CTX_free);
    if (!key_context) {
        throw std::runtime_error("EVP_PKEY_CTX_new_id: " + OpenSslError());
    }
    RequireOpenSsl(EVP_PKEY_keygen_init(key_context.get()), "EVP_PKEY_keygen_init");
    RequireOpenSsl(
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048),
        "EVP_PKEY_CTX_set_rsa_keygen_bits");
    EVP_PKEY* raw_key = nullptr;
    RequireOpenSsl(EVP_PKEY_keygen(key_context.get(), &raw_key), "EVP_PKEY_keygen");
    EvpPkey key(raw_key, &EVP_PKEY_free);

    X509Certificate certificate(X509_new(), &X509_free);
    if (!certificate) {
        throw std::runtime_error("X509_new: " + OpenSslError());
    }
    RequireOpenSsl(X509_set_version(certificate.get(), 2), "X509_set_version");
    RequireOpenSsl(
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1),
        "ASN1_INTEGER_set");
    if (X509_gmtime_adj(X509_get_notBefore(certificate.get()), -60) == nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) == nullptr) {
        throw std::runtime_error("X509_gmtime_adj: " + OpenSslError());
    }
    RequireOpenSsl(X509_set_pubkey(certificate.get(), key.get()), "X509_set_pubkey");

    auto* subject = X509_get_subject_name(certificate.get());
    RequireOpenSsl(
        X509_NAME_add_entry_by_txt(
            subject,
            "CN",
            MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"),
            -1,
            -1,
            0),
        "X509_NAME_add_entry_by_txt");
    RequireOpenSsl(
        X509_set_issuer_name(certificate.get(), subject),
        "X509_set_issuer_name");

    X509V3_CTX extension_context{};
    X509V3_set_ctx(
        &extension_context,
        certificate.get(),
        certificate.get(),
        nullptr,
        nullptr,
        0);
    AddExtension(
        certificate.get(),
        &extension_context,
        NID_basic_constraints,
        "critical,CA:TRUE");
    AddExtension(
        certificate.get(),
        &extension_context,
        NID_key_usage,
        "critical,digitalSignature,keyEncipherment,keyCertSign");
    AddExtension(
        certificate.get(),
        &extension_context,
        NID_ext_key_usage,
        "serverAuth");
    AddExtension(
        certificate.get(),
        &extension_context,
        NID_subject_alt_name,
        "DNS:localhost");
    if (X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
        throw std::runtime_error("X509_sign: " + OpenSslError());
    }

    return {std::move(key), std::move(certificate)};
}

class TemporaryCaFile {
public:
    explicit TemporaryCaFile(X509* certificate) {
        char path[] = "/tmp/alternator-client-cpp-ca-XXXXXX";
        const int fd = mkstemp(path);
        if (fd < 0) {
            throw std::runtime_error("mkstemp failed");
        }
        FILE* file = fdopen(fd, "w");
        if (file == nullptr) {
            close(fd);
            unlink(path);
            throw std::runtime_error("fdopen failed");
        }
        const int write_result = PEM_write_X509(file, certificate);
        const int close_result = fclose(file);
        if (write_result != 1 || close_result != 0) {
            unlink(path);
            throw std::runtime_error("failed to write test CA certificate");
        }
        path_ = path;
    }

    ~TemporaryCaFile() {
        if (!path_.empty()) {
            unlink(path_.c_str());
        }
    }

    TemporaryCaFile(const TemporaryCaFile&) = delete;
    TemporaryCaFile& operator=(const TemporaryCaFile&) = delete;

    [[nodiscard]] const std::string& Path() const {
        return path_;
    }

private:
    std::string path_;
};

class LocalHttpsDiscoveryServer {
public:
    LocalHttpsDiscoveryServer()
        : certificate_(GenerateLocalhostCertificate())
        , ca_file_(certificate_.certificate.get())
        , ssl_context_(SSL_CTX_new(TLS_server_method()), &SSL_CTX_free) {
        if (!ssl_context_) {
            throw std::runtime_error("SSL_CTX_new: " + OpenSslError());
        }
        RequireOpenSsl(
            SSL_CTX_use_certificate(ssl_context_.get(), certificate_.certificate.get()),
            "SSL_CTX_use_certificate");
        RequireOpenSsl(
            SSL_CTX_use_PrivateKey(ssl_context_.get(), certificate_.key.get()),
            "SSL_CTX_use_PrivateKey");
        RequireOpenSsl(
            SSL_CTX_check_private_key(ssl_context_.get()),
            "SSL_CTX_check_private_key");

        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            throw std::runtime_error("IPv4 socket failed");
        }
        int yes = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listen_fd_, 1) != 0) {
            CloseSockets();
            throw std::runtime_error("IPv4 bind/listen failed");
        }
        socklen_t address_size = sizeof(address);
        if (getsockname(
                listen_fd_,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) != 0) {
            CloseSockets();
            throw std::runtime_error("IPv4 getsockname failed");
        }
        port_ = ntohs(address.sin_port);

        bad_fd_ = socket(AF_INET6, SOCK_STREAM, 0);
        if (bad_fd_ < 0) {
            CloseSockets();
            throw std::runtime_error("IPv6 socket failed");
        }
        setsockopt(bad_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(bad_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
        sockaddr_in6 bad_address{};
        bad_address.sin6_family = AF_INET6;
        bad_address.sin6_addr = in6addr_loopback;
        bad_address.sin6_port = htons(port_);
        if (bind(
                bad_fd_,
                reinterpret_cast<sockaddr*>(&bad_address),
                sizeof(bad_address)) != 0) {
            CloseSockets();
            throw std::runtime_error("IPv6 bad-address bind failed");
        }

        worker_ = std::thread(&LocalHttpsDiscoveryServer::Serve, this);
    }

    ~LocalHttpsDiscoveryServer() {
        Stop();
    }

    void Stop() {
        CloseSockets();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

    [[nodiscard]] const std::string& CaFile() const {
        return ca_file_.Path();
    }

    [[nodiscard]] const std::string& Sni() const {
        return sni_;
    }

    [[nodiscard]] const std::string& Request() const {
        return request_;
    }

    [[nodiscard]] const std::string& Error() const {
        return error_;
    }

private:
    void Serve() {
        const int client = accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            error_ = "accept failed";
            return;
        }

        Ssl ssl(SSL_new(ssl_context_.get()), &SSL_free);
        if (!ssl) {
            error_ = "SSL_new: " + OpenSslError();
            close(client);
            return;
        }
        SSL_set_fd(ssl.get(), client);
        if (SSL_accept(ssl.get()) != 1) {
            error_ = "SSL_accept: " + OpenSslError();
            close(client);
            return;
        }

        const char* server_name = SSL_get_servername(ssl.get(), TLSEXT_NAMETYPE_host_name);
        if (server_name != nullptr) {
            sni_ = server_name;
        }

        char buffer[2048];
        while (request_.find("\r\n\r\n") == std::string::npos) {
            const int count = SSL_read(ssl.get(), buffer, sizeof(buffer));
            if (count <= 0) {
                error_ = "SSL_read failed";
                close(client);
                return;
            }
            request_.append(buffer, static_cast<std::size_t>(count));
        }

        const std::string body = R"(["secure-node.internal"])";
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        std::size_t offset = 0;
        while (offset < response.size()) {
            const int count = SSL_write(
                ssl.get(),
                response.data() + offset,
                static_cast<int>(response.size() - offset));
            if (count <= 0) {
                error_ = "SSL_write failed";
                close(client);
                return;
            }
            offset += static_cast<std::size_t>(count);
        }
        SSL_shutdown(ssl.get());
        close(client);
    }

    void CloseSockets() {
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (bad_fd_ >= 0) {
            close(bad_fd_);
            bad_fd_ = -1;
        }
    }

    GeneratedCertificate certificate_;
    TemporaryCaFile ca_file_;
    SslContext ssl_context_;
    int listen_fd_ = -1;
    int bad_fd_ = -1;
    std::uint16_t port_ = 0;
    std::string sni_;
    std::string request_;
    std::string error_;
    std::thread worker_;
};

class RecordingResolvedCurlClient final : public HttpClient {
public:
    RecordingResolvedCurlClient(Config config, std::vector<std::string> addresses)
        : delegate_(std::move(config))
        , addresses_(std::move(addresses)) {}

    HttpResponse Get(const Url& url) const override {
        return delegate_.Get(url);
    }

    std::vector<std::string> Resolve(const Url&) const override {
        return addresses_;
    }

    HttpResponse GetResolved(
        const Url& url,
        const std::string& resolved_address) const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            attempts_.push_back(resolved_address);
        }
        return delegate_.GetResolved(url, resolved_address);
    }

    [[nodiscard]] std::vector<std::string> Attempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return attempts_;
    }

private:
    CurlHttpClient delegate_;
    std::vector<std::string> addresses_;
    mutable std::mutex mutex_;
    mutable std::vector<std::string> attempts_;
};

#endif

} // namespace

TEST(DnsFallbackHttps, PreservesLogicalSniCertificateAndHostAcrossBadThenGoodAddress) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL && SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL
    LocalHttpsDiscoveryServer server;

    Config config;
    config.scheme = "https";
    config.port = server.Port();
    config.ca_file = server.CaFile();
    config.connect_timeout = std::chrono::milliseconds{200};
    config.http_client_timeout = std::chrono::milliseconds{1000};
    config.nodes_list_update_period = std::chrono::milliseconds{0};
    config.node_health.down_node_probe_period = std::chrono::milliseconds{0};
    auto http = std::make_shared<RecordingResolvedCurlClient>(
        config,
        std::vector<std::string>{"::1", "127.0.0.1"});

    AlternatorLiveNodes nodes({"localhost"}, config, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    server.Stop();

    EXPECT_TRUE(server.Error().empty()) << server.Error();
    EXPECT_EQ(http->Attempts(), std::vector<std::string>({"::1", "127.0.0.1"}));
    EXPECT_EQ(nodes.GetNodes(),
              std::vector<Url>({Url("https", "secure-node.internal", server.Port())}));
    EXPECT_EQ(server.Sni(), "localhost");
    EXPECT_NE(
        server.Request().find("Host: localhost:" + std::to_string(server.Port())),
        std::string::npos);
#else
    GTEST_SKIP() << "libcurl and OpenSSL are required";
#endif
}
