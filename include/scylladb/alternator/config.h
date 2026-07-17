#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <scylladb/alternator/key_route_affinity.h>
#include <scylladb/alternator/node_health.h>
#include <scylladb/alternator/routing_scope.h>

#ifndef SCYLLADB_ALTERNATOR_CLIENT_CPP_VERSION
#define SCYLLADB_ALTERNATOR_CLIENT_CPP_VERSION "devel"
#endif

namespace scylladb::alternator {

struct Credentials {
    std::string access_key_id;
    std::string secret_access_key;
};

class HttpContentEncodingDecoder {
public:
    virtual ~HttpContentEncodingDecoder() = default;

    [[nodiscard]] virtual std::vector<std::string> AcceptedResponseEncodings() const = 0;
    [[nodiscard]] virtual std::string Decode(std::string body, const std::string& content_encoding) const = 0;
};

class HttpRequestCompressor {
public:
    virtual ~HttpRequestCompressor() = default;

    [[nodiscard]] virtual std::string ContentEncoding() const = 0;
    // Returns false to leave the request uncompressed, for example when
    // input_size is below an implementation-defined minimum.
    [[nodiscard]] virtual bool Compress(
        std::istream& input,
        std::uint64_t input_size,
        std::ostream& output) const = 0;
};

class ZlibContentEncodingDecoder final : public HttpContentEncodingDecoder {
public:
    explicit ZlibContentEncodingDecoder(std::vector<std::string> accepted_response_encodings = {"gzip", "deflate"});

    [[nodiscard]] std::vector<std::string> AcceptedResponseEncodings() const override;
    [[nodiscard]] std::string Decode(std::string body, const std::string& content_encoding) const override;

private:
    std::vector<std::string> accepted_response_encodings_;
};

class GzipRequestCompressor final : public HttpRequestCompressor {
public:
    explicit GzipRequestCompressor(std::uint64_t min_size_bytes = 1024);

    [[nodiscard]] std::string ContentEncoding() const override;
    [[nodiscard]] bool Compress(
        std::istream& input,
        std::uint64_t input_size,
        std::ostream& output) const override;

private:
    std::uint64_t min_size_bytes_ = 0;
};

struct HeaderOptimizationContext {
    bool credentials_configured = false;
    bool user_agent_configured = false;
};

class HeaderOptimization {
public:
    virtual ~HeaderOptimization() = default;

    [[nodiscard]] virtual std::vector<std::string> AllowedHeaders(
        const HeaderOptimizationContext& context) const = 0;
};

class DefaultHeaderOptimization final : public HeaderOptimization {
public:
    [[nodiscard]] std::vector<std::string> AllowedHeaders(
        const HeaderOptimizationContext& context) const override;
};

class HeaderAllowlistOptimization final : public HeaderOptimization {
public:
    explicit HeaderAllowlistOptimization(std::vector<std::string> allowed_headers);

    [[nodiscard]] std::vector<std::string> AllowedHeaders(
        const HeaderOptimizationContext& context) const override;
private:
    std::vector<std::string> allowed_headers_;
};

struct Config {
    std::uint16_t port = 8080;
    std::string scheme = "http";
    RoutingScopePtr routing_scope = NewClusterScope();
    // The AWS SDK requires a non-empty region for client configuration,
    // signing scope, and diagnostics. Alternator routing ignores this value,
    // so the default is only a placeholder. Set it to the deployment or
    // Scylla Cloud region when SDK logs, traces, or metrics should show a
    // meaningful region.
    std::string aws_region = "default-alb-region";
    Credentials credentials;

    std::chrono::milliseconds nodes_list_update_period{std::chrono::seconds(1)};
    std::chrono::milliseconds idle_nodes_list_update_period{std::chrono::minutes(1)};
    std::chrono::milliseconds http_client_timeout{0};
    std::chrono::milliseconds connect_timeout{1000};

    bool verify_ssl = true;
    std::string ca_file;
    std::string client_certificate_file;
    std::string client_private_key_file;
    bool tls_session_cache_enabled = true;
    std::uint32_t tls_session_cache_size = 1024;
    std::chrono::seconds tls_session_timeout{86400};

    unsigned max_connections = 100;
    bool reuse_discovery_connections = true;
    std::shared_ptr<HttpRequestCompressor> request_compressor;
    std::vector<std::shared_ptr<HttpContentEncodingDecoder>> content_encoding_decoders;
    std::string user_agent = "scylladb-alternator-client-cpp/" SCYLLADB_ALTERNATOR_CLIENT_CPP_VERSION;
    std::shared_ptr<HeaderOptimization> header_optimization;

    NodeHealthStoreConfig node_health;
    KeyRouteAffinityConfig key_route_affinity;
};

void ValidateConfig(const Config& config);

} // namespace scylladb::alternator
