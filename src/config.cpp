#include <scylladb/alternator/config.h>

#include "http_compression.h"

#include <stdexcept>

namespace scylladb::alternator {

void ValidateConfig(const Config& config) {
    if (config.scheme != "http" && config.scheme != "https") {
        throw std::invalid_argument("scheme must be http or https");
    }
    if (config.port == 0) {
        throw std::invalid_argument("port must be > 0");
    }
    if (!config.routing_scope) {
        throw std::invalid_argument("routing_scope must not be null");
    }
    if (config.max_connections == 0) {
        throw std::invalid_argument("max_connections must be > 0");
    }
    (void)detail::BuildAcceptEncodingValue(config.content_encoding_decoders);
    if (config.key_route_affinity.partition_key_discovery_attempts == 0) {
        throw std::invalid_argument("partition_key_discovery_attempts must be > 0");
    }
    if (config.key_route_affinity.partition_key_discovery_initial_backoff < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("partition_key_discovery_initial_backoff must be >= 0");
    }
    if (config.tls_session_cache_enabled) {
        if (config.tls_session_cache_size == 0) {
            throw std::invalid_argument("tls_session_cache_size must be > 0 when TLS session cache is enabled");
        }
        if (config.tls_session_timeout <= std::chrono::seconds::zero()) {
            throw std::invalid_argument("tls_session_timeout must be > 0 when TLS session cache is enabled");
        }
    }
}

} // namespace scylladb::alternator
