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
}

} // namespace scylladb::alternator
