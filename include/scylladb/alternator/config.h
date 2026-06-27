#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <scylladb/alternator/key_route_affinity.h>
#include <scylladb/alternator/node_health.h>
#include <scylladb/alternator/routing_scope.h>

namespace scylladb::alternator {

struct Credentials {
    std::string access_key_id;
    std::string secret_access_key;
};

struct Config {
    std::uint16_t port = 8080;
    std::string scheme = "http";
    RoutingScopePtr routing_scope = NewClusterScope();
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

    unsigned max_connections = 100;
    bool reuse_discovery_connections = true;
    std::string user_agent = "scylladb-alternator-client-cpp/devel";

    NodeHealthStoreConfig node_health;
    KeyRouteAffinityConfig key_route_affinity;
};

void ValidateConfig(const Config& config);

} // namespace scylladb::alternator
