#include <scylladb/alternator/config.h>

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
