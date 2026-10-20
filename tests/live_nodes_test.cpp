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

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace scylladb::alternator;

class FakeHttpClient final : public HttpClient {
public:
    using Handler = std::function<HttpResponse(const Url&)>;

    explicit FakeHttpClient(Handler handler)
        : handler_(std::move(handler)) {}

    HttpResponse Get(const Url& url) const override {
        return handler_(url);
    }

private:
    Handler handler_;
};

class CallbackRoutingScope final : public RoutingScope {
public:
    std::function<std::string()> query;
    std::function<RoutingScopePtr()> fallback = [] { return RoutingScopePtr{}; };
    std::function<bool()> cluster = [] { return false; };

    std::string Name() const override { return "custom"; }
    std::string ToString() const override { return "custom"; }
    std::string LocalNodesQuery() const override { return query(); }
    RoutingScopePtr Fallback() const override { return fallback(); }
    bool IsCluster() const override { return cluster(); }
};

class PassthroughContentEncodingDecoder final : public HttpContentEncodingDecoder {
public:
    explicit PassthroughContentEncodingDecoder(std::vector<std::string> accepted_encodings = {"br"})
        : accepted_encodings_(std::move(accepted_encodings)) {}

    std::vector<std::string> AcceptedResponseEncodings() const override {
        return accepted_encodings_;
    }

    std::string Decode(std::string body, const std::string&) const override {
        return body;
    }

private:
    std::vector<std::string> accepted_encodings_;
};

static std::vector<std::string> Hosts(const std::vector<Url>& nodes) {
    std::vector<std::string> out;
    out.reserve(nodes.size());
    for (const auto& node : nodes) {
        out.push_back(node.host);
    }
    return out;
}

static std::int64_t HashWhereFirstNodeIs(const std::vector<Url>& nodes, const Url& target) {
    for (std::int64_t hash = 0; hash < 100000; ++hash) {
        if (FirstNodeWithSeed(nodes, hash) == target) {
            return hash;
        }
    }
    throw std::runtime_error("failed to find hash for target node");
}

static std::string KeyWhereFirstNodeIs(const std::vector<Url>& nodes, const Url& target) {
    for (int i = 0; i < 100000; ++i) {
        auto key = "key-" + std::to_string(i);
        if (FirstNodeWithSeed(nodes, AttributeValue::String(key).Hash()) == target) {
            return key;
        }
    }
    throw std::runtime_error("failed to find key for target node");
}

class LocalDnsEntrypointServer {
public:
    explicit LocalDnsEntrypointServer(std::string body)
        : body_(std::move(body)) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket failed");
        }

        int yes = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("bind failed");
        }
        if (listen(fd_, 1) != 0) {
            throw std::runtime_error("listen failed");
        }

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(addr.sin_port);

        worker_ = std::thread([this] {
            int client = accept(fd_, nullptr, nullptr);
            if (client < 0) {
                return;
            }

            char buffer[2048];
            const auto n = recv(client, buffer, sizeof(buffer), 0);
            if (n > 0) {
                request_.assign(buffer, static_cast<std::size_t>(n));
            }

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << body_.size() << "\r\n"
                     << "Connection: close\r\n"
                     << "\r\n"
                     << body_;
            const auto response_text = response.str();
            send(client, response_text.data(), response_text.size(), 0);
            close(client);
        });
    }

    ~LocalDnsEntrypointServer() {
        Wait();
    }

    void Wait() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::uint16_t Port() const {
        return port_;
    }

    [[nodiscard]] const std::string& Request() const {
        return request_;
    }

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::string body_;
    std::string request_;
    std::thread worker_;
};

TEST(AlternatorLiveNodes, RoutingScopeFallbackRetriesKnownNodes) {
    Config cfg;
    cfg.routing_scope = NewDCScope("wrong", NewDCScope("target"));
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> fallback_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        if (url.query == "dc=wrong") {
            return HttpResponse{200, "[]"};
        }
        if (url.query == "dc=target") {
            ++fallback_requests;
            return HttpResponse{200, "[\"node3.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"node1.local", "node2.local"}, cfg, http);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node3.local"}));
    EXPECT_GT(fallback_requests.load(), 0);
}

TEST(AlternatorLiveNodes, ClusterScopeMergesSeedNodes) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> dc1_requests{0};
    std::atomic<int> dc2_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "");
        if (url.host == "dc1-node1.local") {
            ++dc1_requests;
            return HttpResponse{200, "[\"dc1-node1.local\",\"dc1-node2.local\"]"};
        }
        if (url.host == "dc2-node1.local") {
            ++dc2_requests;
            return HttpResponse{200, "[\"dc2-node1.local\",\"dc2-node2.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"dc1-node1.local", "dc2-node1.local"}, cfg, http);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({
                  "dc1-node1.local",
                  "dc1-node2.local",
                  "dc2-node1.local",
                  "dc2-node2.local",
              }));
    EXPECT_GT(dc1_requests.load(), 0);
    EXPECT_GT(dc2_requests.load(), 0);
}

TEST(AlternatorLiveNodes, KeyRouteAffinityUsesDatacenterNodesAcrossRackScopes) {
    Config rack1_config;
    rack1_config.routing_scope = NewRackScope("dc1", "rack1");
    rack1_config.key_route_affinity.mode = KeyRouteAffinityMode::ReadBeforeWrite;
    rack1_config.nodes_list_update_period = std::chrono::milliseconds{0};
    Config rack2_config = rack1_config;
    rack2_config.routing_scope = NewRackScope("dc1", "rack2");

    std::atomic<int> dc_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        if (url.query == "dc=dc1&rack=rack1") {
            return HttpResponse{200, "[\"rack1-node.local\"]"};
        }
        if (url.query == "dc=dc1&rack=rack2") {
            return HttpResponse{200, "[\"rack2-node.local\"]"};
        }
        if (url.query == "dc=dc1") {
            ++dc_requests;
            return HttpResponse{
                200,
                "[\"rack1-node.local\",\"rack2-node.local\",\"rack3-node.local\"]"};
        }
        ADD_FAILURE() << "unexpected discovery scope: " << url.query;
        return HttpResponse{200, "[\"other-dc.local\"]"};
    });

    AlternatorLiveNodes rack1_nodes({"seed.local"}, rack1_config, http);
    AlternatorLiveNodes rack2_nodes({"seed.local"}, rack2_config, http);
    rack1_nodes.UpdateLiveNodes();
    rack2_nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(rack1_nodes.GetQueryPlanNodes()), std::vector<std::string>({"rack1-node.local"}));
    EXPECT_EQ(Hosts(rack2_nodes.GetQueryPlanNodes()), std::vector<std::string>({"rack2-node.local"}));

    const std::vector<Url> dc_nodes{
        {"http", "rack1-node.local", 8080},
        {"http", "rack2-node.local", 8080},
        {"http", "rack3-node.local", 8080},
    };
    EXPECT_EQ(rack1_nodes.GetActiveKeyRouteAffinityNodes(), dc_nodes);
    EXPECT_EQ(rack2_nodes.GetActiveKeyRouteAffinityNodes(), dc_nodes);
    EXPECT_EQ(dc_requests.load(), 2);

    const auto target = dc_nodes[2];
    const auto key = KeyWhereFirstNodeIs(dc_nodes, target);
    PartitionKeyMetadata metadata({{"orders", "id"}});
    auto rack1_plan = QueryPlanForPartitionKey(
        rack1_nodes,
        {{"id", AttributeValue::String(key)}},
        "orders",
        metadata);
    auto rack2_plan = QueryPlanForPartitionKey(
        rack2_nodes,
        {{"id", AttributeValue::String(key)}},
        "orders",
        metadata);

    EXPECT_EQ(rack1_plan.Next(), target);
    EXPECT_EQ(rack2_plan.Next(), target);

    auto batch1 = QueryPlanForBatchWrite(
        rack1_nodes, {BatchWriteOperation::Put("orders", {{"id", AttributeValue::String(key)}})}, metadata);
    auto batch2 = QueryPlanForBatchWrite(
        rack2_nodes, {BatchWriteOperation::Put("orders", {{"id", AttributeValue::String(key)}})}, metadata);
    EXPECT_EQ(batch1.Next(), target);
    EXPECT_EQ(batch2.Next(), target);
    std::vector<Url> retry_nodes{target};
    for (auto node = rack1_plan.Next(); !node.Empty(); node = rack1_plan.Next()) {
        retry_nodes.push_back(node);
    }
    EXPECT_EQ(SortAndDedupeNodes(retry_nodes), dc_nodes);
    EXPECT_THROW(QueryPlanForPartitionKey(rack1_nodes, {}, "orders", metadata), std::invalid_argument);

    rack1_nodes.ReportNodeResult(target, NodeHealthObservation::ConnectionFailure);
    EXPECT_EQ(Hosts(rack1_nodes.GetActiveKeyRouteAffinityNodes()),
              std::vector<std::string>({"rack1-node.local", "rack2-node.local"}));
    EXPECT_EQ(Hosts(rack1_nodes.GetQueryPlanNodes()), std::vector<std::string>({"rack1-node.local"}));
}

TEST(AlternatorLiveNodes, AffinityPreservesExplicitDatacenterAndClusterFallbackOrder) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", NewRackScope("B", "r2", NewClusterScope()));
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    int phase = 0;
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        if (url.query == "dc=A&rack=r1") {
            return HttpResponse{200, R"(["rack-a.local"])"};
        }
        if (url.query == "dc=A") {
            return HttpResponse{200, phase == 0 ? R"(["a1.local","a2.local"])" : "[]"};
        }
        if (url.query == "dc=B") {
            return HttpResponse{200, phase == 1 ? R"(["b1.local","b2.local"])" : "[]"};
        }
        EXPECT_TRUE(url.query.empty());
        return HttpResponse{200, R"(["cluster.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=A"}));
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()), std::vector<std::string>({"a1.local", "a2.local"}));
    auto snapshot = QueryPlan::FromOrderedNodes(nodes.GetActiveKeyRouteAffinityNodes());

    phase = 1;
    queries.clear();
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=A", "dc=B"}));
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()), std::vector<std::string>({"b1.local", "b2.local"}));
    EXPECT_EQ(snapshot.Next().host, "a1.local");
    EXPECT_EQ(snapshot.Next().host, "a2.local");
    EXPECT_TRUE(snapshot.Next().Empty());

    phase = 2;
    queries.clear();
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=A", "dc=B", ""}));
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()), std::vector<std::string>({"cluster.local"}));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"rack-a.local"}));
}

TEST(AlternatorLiveNodes, AffinityReusesOrdinaryFallbackOnlyAfterPreferredDatacenter) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", NewDCScope("B"));
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    bool preferred_available = true;
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        if (url.query == "dc=A&rack=r1") {
            return HttpResponse{200, "[]"};
        }
        if (url.query == "dc=B") {
            return HttpResponse{200, R"(["b.local"])"};
        }
        EXPECT_EQ(url.query, "dc=A");
        return HttpResponse{200, preferred_available ? R"(["a.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=B", "dc=A"}));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"b.local"}));
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()), std::vector<std::string>({"a.local"}));
    preferred_available = false;
    queries.clear();
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=B", "dc=A"}));
    EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), nodes.GetActiveNodes());
}

TEST(AlternatorLiveNodes, AffinityReusesMatchingOrdinaryDatacenter) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", NewDCScope("A"));
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        return HttpResponse{200, url.query == "dc=A" ? R"(["a.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=A"}));
    EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), nodes.GetActiveNodes());
}

TEST(AlternatorLiveNodes, AffinityKeepsStartupAndFailureFallbackThenRecoversWithoutImplicitCluster) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    int phase = 0;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.query == "dc=A&rack=r1") {
            if (phase == 3) {
                throw std::runtime_error("ordinary discovery failed");
            }
            return HttpResponse{200, phase == 4 ? "[]" : R"(["rack.local"])"};
        }
        EXPECT_EQ(url.query, "dc=A");
        EXPECT_LT(phase, 3) << "ordinary failure must not attempt affinity discovery";
        if (phase == 2) {
            throw std::runtime_error("affinity discovery failed");
        }
        return HttpResponse{200, phase == 0 ? "[]" : R"(["dc.local","rack.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()), std::vector<std::string>({"seed.local"}));
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()), std::vector<std::string>({"rack.local"}));
    phase = 1;
    nodes.UpdateLiveNodes();
    const auto discovered = nodes.GetActiveKeyRouteAffinityNodes();
    const auto ordinary = nodes.GetNodes();
    EXPECT_EQ(Hosts(discovered), std::vector<std::string>({"dc.local", "rack.local"}));
    phase = 2;
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), discovered);
    phase = 3;
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), discovered);
    EXPECT_EQ(nodes.GetNodes(), ordinary);
    phase = 4;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), discovered);
    EXPECT_EQ(nodes.GetNodes(), ordinary);
    phase = 1;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), discovered);
}

TEST(AlternatorLiveNodes, AffinityPreservesCustomScopesAndAvoidsUnnecessaryDiscovery) {
    class CustomScope final : public RoutingScope {
    public:
        explicit CustomScope(RoutingScopePtr fallback = nullptr) : fallback_(std::move(fallback)) {}
        std::string Name() const override { return "Rack"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override { return "opaque=scope"; }
        RoutingScopePtr Fallback() const override { return fallback_; }
    private:
        RoutingScopePtr fallback_;
    };
    for (int scenario = 0; scenario < 5; ++scenario) {
        Config cfg;
        cfg.key_route_affinity.mode = scenario == 0 ? KeyRouteAffinityMode::None : KeyRouteAffinityMode::AnyWrite;
        cfg.routing_scope = scenario == 0 ? NewRackScope("A", "r1") :
            scenario == 1 ? NewDCScope("A") :
            scenario == 2 ? NewClusterScope() :
            std::make_shared<CustomScope>(scenario == 4 ? NewRackScope("B", "r2") : nullptr);
        std::vector<std::string> queries;
        auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
            queries.push_back(url.query);
            return HttpResponse{200, R"(["node.local"])"};
        });
        AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
        nodes.UpdateLiveNodes();
        EXPECT_EQ(queries, std::vector<std::string>({cfg.routing_scope->LocalNodesQuery()}));
        EXPECT_EQ(nodes.GetActiveKeyRouteAffinityNodes(), nodes.GetActiveNodes());
    }
    Config cfg;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.routing_scope = std::make_shared<CustomScope>(NewRackScope("B", "r2"));
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        return HttpResponse{200, url.query == "opaque=scope" ? "[]" : R"(["b.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"opaque=scope", "dc=B&rack=r2", "dc=B"}));
    queries.clear();
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"opaque=scope", "dc=B&rack=r2", "dc=B"}));
}

TEST(AlternatorLiveNodes, AffinityDeduplicatesRepeatedRacksAndMatchingDatacenters) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1",
        NewRackScope("A", "r1", NewDCScope("A", NewRackScope("B", "r2"))));
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        return HttpResponse{200, url.query == "dc=A" ? "[]" : R"(["node.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    for (int refresh = 0; refresh < 2; ++refresh) {
        queries.clear();
        nodes.UpdateLiveNodes();
        EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=A", "dc=B"}));
    }
}

TEST(AlternatorLiveNodes, AffinityRetainsCustomAggregationForAnEquivalentQuery) {
    class AggregatingScope final : public RoutingScope {
    public:
        std::string Name() const override { return "custom"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override { return "dc=A"; }
        RoutingScopePtr Fallback() const override { return nullptr; }
        bool IsCluster() const override { return true; }
    };
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", std::make_shared<AggregatingScope>());
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    int dc_requests = 0;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.query == "dc=A&rack=r1") {
            return HttpResponse{200, R"(["rack.local"])"};
        }
        EXPECT_EQ(url.query, "dc=A");
        ++dc_requests;
        return HttpResponse{200, dc_requests <= 2 ? "[]" :
            url.host == "seed1.local" ? R"(["one.local"])" : R"(["two.local"])"};
    });
    AlternatorLiveNodes nodes({"seed1.local", "seed2.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(dc_requests, 4);
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"one.local", "two.local"}));
}

TEST(AlternatorLiveNodes, AffinityRetainsCustomScopesWhoseEquivalentQueriesLaterChange) {
    class CustomScope final : public RoutingScope {
    public:
        std::string query = "dc=A";
        std::string Name() const override { return "custom"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override { return query; }
        RoutingScopePtr Fallback() const override { return nullptr; }
    };
    auto custom = std::make_shared<CustomScope>();
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", custom);
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    bool changed = false;
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        if (url.query == "dc=B") {
            return HttpResponse{200, R"(["b.local"])"};
        }
        return HttpResponse{200, changed ? "[]" : R"(["a.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=A"}));
    custom->query = "dc=B";
    changed = true;
    queries.clear();
    nodes.UpdateLiveNodes();
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A&rack=r1", "dc=B", "dc=A"}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b.local"}));
}

TEST(AlternatorLiveNodes, AffinityDoesNotResolveAnUnusedCustomFallback) {
    class CountingScope final : public RoutingScope {
    public:
        mutable int fallback_reads = 0;
        std::weak_ptr<const RoutingScope> self;
        std::string Name() const override { return "custom"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override { return "custom=opaque"; }
        RoutingScopePtr Fallback() const override {
            if (++fallback_reads > 2) {
                throw std::runtime_error("unused self fallback was traversed");
            }
            return self.lock();
        }
    };
    auto scope = std::make_shared<CountingScope>();
    scope->self = scope;
    Config cfg;
    cfg.routing_scope = scope;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, R"(["node.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    EXPECT_EQ(scope->fallback_reads, 0);
    nodes.UpdateLiveNodes();
    nodes.UpdateLiveNodes();
    EXPECT_EQ(scope->fallback_reads, 0);
}

TEST(AlternatorLiveNodes, AffinityRetainsSeparateCustomFallbackOccurrencesAndResolvedLinks) {
    class StatefulScope final : public RoutingScope {
    public:
        mutable int fallback_reads = 0;
        mutable int query_reads = 0;
        std::weak_ptr<const RoutingScope> self;
        std::string Name() const override { return "custom"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override {
            ++query_reads;
            return "custom=opaque";
        }
        RoutingScopePtr Fallback() const override {
            return ++fallback_reads == 1 ? self.lock() : NewRackScope("B", "r2");
        }
    };
    auto scope = std::make_shared<StatefulScope>();
    scope->self = scope;
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", scope);
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        if (url.query == "dc=A&rack=r1") {
            return HttpResponse{200, R"(["ordinary.local"])"};
        }
        return HttpResponse{200, url.query == "dc=B" ? R"(["b.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    EXPECT_EQ(scope->fallback_reads, 0);
    EXPECT_EQ(scope->query_reads, 0);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"ordinary.local"}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b.local"}));
    EXPECT_EQ(scope->fallback_reads, 2);
    EXPECT_EQ(scope->query_reads, 2);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b.local"}));
    EXPECT_EQ(scope->fallback_reads, 2);
    EXPECT_EQ(scope->query_reads, 4);
}

TEST(AlternatorLiveNodes, AffinityRetriesAnUnresolvedCustomFallbackAfterFailure) {
    class FailingScope final : public RoutingScope {
    public:
        mutable int fallback_reads = 0;
        std::string Name() const override { return "custom"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override { return "custom=opaque"; }
        RoutingScopePtr Fallback() const override {
            if (++fallback_reads == 1) {
                throw std::runtime_error("temporary fallback failure");
            }
            return NewRackScope("B", "r2");
        }
    };
    auto scope = std::make_shared<FailingScope>();
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", scope);
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        if (url.query == "dc=A&rack=r1") {
            return HttpResponse{200, R"(["ordinary.local"])"};
        }
        return HttpResponse{200, url.query == "dc=B" ? R"(["b.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    EXPECT_EQ(scope->fallback_reads, 0);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"ordinary.local"}));
    EXPECT_EQ(scope->fallback_reads, 1);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b.local"}));
    nodes.UpdateLiveNodes();
    EXPECT_EQ(scope->fallback_reads, 2);
}

TEST(AlternatorLiveNodes, AffinityReusesTheOrdinaryCustomQueryWithoutEvaluatingItAgain) {
    class CustomScope final : public RoutingScope {
    public:
        mutable int query_reads = 0;
        std::string Name() const override { return "custom"; }
        std::string ToString() const override { return "custom"; }
        std::string LocalNodesQuery() const override {
            return ++query_reads % 2 == 1 ? "dc=A" : "dc=B";
        }
        RoutingScopePtr Fallback() const override { return NewRackScope("A", "r1"); }
    };
    auto scope = std::make_shared<CustomScope>();
    Config cfg;
    cfg.routing_scope = scope;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    std::vector<std::string> queries;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        queries.push_back(url.query);
        return HttpResponse{200, url.query == "dc=A" ? R"(["a.local"])" : R"(["b.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    EXPECT_EQ(scope->query_reads, 0);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"a.local"}));
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"b.local"}));
    EXPECT_EQ(nodes.GetKnownAffinityNodes(), nodes.GetNodes());
    EXPECT_EQ(scope->query_reads, 2);
    EXPECT_EQ(queries, std::vector<std::string>({"dc=A", "dc=B"}));
}

TEST(AlternatorLiveNodes, RepeatedCustomOccurrencesKeepTheirActualOrdinaryQueries) {
    for (auto mode : {KeyRouteAffinityMode::None, KeyRouteAffinityMode::AnyWrite}) {
        int queries = 0;
        int fallbacks = 0;
        auto scope = std::make_shared<CallbackRoutingScope>();
        std::weak_ptr<const RoutingScope> self = scope;
        scope->query = [&] { return ++queries == 1 ? "dc=A" : "dc=B"; };
        scope->fallback = [&, self]() -> RoutingScopePtr {
            return ++fallbacks == 1 ? self.lock() : NewDCScope("C");
        };
        Config cfg;
        cfg.routing_scope = scope;
        cfg.key_route_affinity.mode = mode;
        cfg.node_health.disabled = true;
        auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
            return HttpResponse{200, url.query == "dc=B" ? R"(["b.local"])" : "[]"};
        });
        AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
        nodes.UpdateLiveNodes();
        EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"b.local"}));
        EXPECT_EQ(nodes.GetKnownAffinityNodes(), nodes.GetNodes());
        EXPECT_EQ(queries, 2);
        EXPECT_EQ(fallbacks, 1);
    }
}

TEST(AlternatorLiveNodes, OrdinaryEdgesEstablishCanonicalOccurrencesWithoutReplacingThemLater) {
    int queries = 0;
    int fallbacks = 0;
    auto scope = std::make_shared<CallbackRoutingScope>();
    std::weak_ptr<const RoutingScope> self = scope;
    scope->query = [&] { ++queries; return "custom=opaque"; };
    scope->fallback = [&, self]() -> RoutingScopePtr {
        if (++fallbacks == 1) {
            return self.lock();
        }
        return fallbacks == 2 ? NewRackScope("B", "r2") : NewRackScope("C", "r3");
    };
    Config cfg;
    cfg.routing_scope = scope;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        if (url.query == "dc=B&rack=r2") {
            return HttpResponse{200, R"(["b-rack.local"])"};
        }
        if (url.query == "dc=C&rack=r3") {
            return HttpResponse{200, R"(["c-rack.local"])"};
        }
        return HttpResponse{200, url.query == "dc=B" ? R"(["b-dc.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"b-rack.local"}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b-dc.local"}));
    EXPECT_EQ(queries, 2);
    EXPECT_EQ(fallbacks, 2);

    // The ordinary path is now shorter and points elsewhere. Index 1 is no longer
    // the canonical second custom occurrence; its rack result must not be reused there.
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"c-rack.local"}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b-dc.local"}));
    EXPECT_EQ(queries, 4);
    EXPECT_EQ(fallbacks, 3);
}

TEST(AlternatorLiveNodes, CustomOnlyAffinityTracksTheCurrentOrdinaryChild) {
    int phase = -2;
    int fallbacks = 0;
    int queries = 0;
    auto first = std::make_shared<CallbackRoutingScope>();
    first->query = [] { return "custom=first"; };
    auto second = std::make_shared<CallbackRoutingScope>();
    second->query = [] { return "custom=second"; };
    auto root = std::make_shared<CallbackRoutingScope>();
    root->query = [&] { ++queries; return "custom=root"; };
    root->fallback = [&]() -> RoutingScopePtr {
        ++fallbacks;
        if (phase < 0) {
            return NewRackScope(phase == -2 ? "empty" : "failed", "r0");
        }
        if (phase == 2) {
            return NewRackScope("B", "r2");
        }
        return phase == 0 ? first : second;
    };
    Config cfg;
    cfg.routing_scope = NewDCScope("prefix", root);
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        if (url.query == "dc=failed&rack=r0") {
            throw std::runtime_error("ordinary discovery failed");
        }
        return HttpResponse{200, url.query == "custom=first" ? R"(["first.local"])" :
            url.query == "custom=second" ? R"(["second.local"])" :
            url.query == "dc=B&rack=r2" ? R"(["rack-b.local"])" :
            url.query == "dc=B" ? R"(["dc-b.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.local"}));
    phase = -1;
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"seed.local"}));
    phase = 0;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"first.local"}));
    phase = 1;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"second.local"}));
    EXPECT_EQ(nodes.GetKnownAffinityNodes(), nodes.GetNodes());
    EXPECT_EQ(fallbacks, 4);
    phase = 2;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"rack-b.local"}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"dc-b.local"}));
    EXPECT_EQ(fallbacks, 5);
    EXPECT_EQ(queries, 5);
}

TEST(AlternatorLiveNodes, CustomOrdinaryQueriesAreEvaluatedForEachAttemptedSeed) {
    for (auto mode : {KeyRouteAffinityMode::None, KeyRouteAffinityMode::AnyWrite}) {
        int queries = 0;
        auto scope = std::make_shared<CallbackRoutingScope>();
        scope->query = [&] { return ++queries == 1 ? "dc=A" : "dc=B"; };
        Config cfg;
        cfg.routing_scope = scope;
        cfg.key_route_affinity.mode = mode;
        cfg.node_health.disabled = true;
        auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
            return HttpResponse{200, url.query == "dc=B" ? R"(["b.local"])" : "[]"};
        });
        AlternatorLiveNodes nodes({"seed1.local", "seed2.local"}, cfg, http);
        nodes.UpdateLiveNodes();
        EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"b.local"}));
        EXPECT_EQ(queries, 2);
    }
}

TEST(AlternatorLiveNodes, AffinityOnlyCustomQueriesKeepLaterSeedReadsDespiteACachedEmptyOperation) {
    int queries = 0;
    auto scope = std::make_shared<CallbackRoutingScope>();
    scope->query = [&] { return ++queries == 1 ? "dc=A" : "dc=B"; };
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", scope);
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    std::vector<std::string> requests;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        requests.push_back(url.query);
        return HttpResponse{200, url.query == "dc=A&rack=r1" ? R"(["rack.local"])" :
            url.query == "dc=B" ? R"(["b.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed1.local", "seed2.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b.local"}));
    EXPECT_EQ(queries, 2);
    EXPECT_EQ(requests, std::vector<std::string>({"dc=A&rack=r1", "dc=A", "dc=A", "dc=B"}));
}

TEST(AlternatorLiveNodes, CustomAggregationAndSeedSelectionKeepTheirSeparateGetterReads) {
    int cluster_reads = 0;
    int phase = 0;
    auto scope = std::make_shared<CallbackRoutingScope>();
    scope->cluster = [&] { return ++cluster_reads % 2 == 0; };
    scope->query = [] { return "custom=scope"; };
    Config cfg;
    cfg.routing_scope = scope;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        return HttpResponse{200, url.host == "seed2.local" ?
            "[\"node" + std::to_string(phase) + ".local\"]" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed1.local", "seed2.local"}, cfg, http);
    for (; phase < 3; ++phase) {
        nodes.UpdateLiveNodes();
        EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node" + std::to_string(phase) + ".local"}));
    }
    EXPECT_EQ(cluster_reads, 6);
}

TEST(AlternatorLiveNodes, CustomQueryExceptionsRemainOutsideHttpFailureHandling) {
    bool fail = true;
    int requests = 0;
    auto scope = std::make_shared<CallbackRoutingScope>();
    scope->query = [&]() -> std::string {
        if (fail) {
            throw std::runtime_error("custom query failure");
        }
        return "custom=scope";
    };
    Config cfg;
    cfg.routing_scope = scope;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        ++requests;
        return HttpResponse{200, R"(["node.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(requests, 0);
    EXPECT_TRUE(nodes.GetDownNodes().empty());
    fail = false;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node.local"}));
    EXPECT_EQ(requests, 1);
}

TEST(AlternatorLiveNodes, ReusedCustomEndpointErrorsDoNotReportHealthTwiceAndRetryLater) {
    bool recover = false;
    int dc_requests = 0;
    auto scope = std::make_shared<CallbackRoutingScope>();
    scope->query = [] { return "dc=A"; };
    scope->fallback = [] { return NewDCScope("B"); };
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", scope);
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.consecutive_server_error_threshold = 2;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.query == "dc=A&rack=r1") {
            return HttpResponse{200, R"(["seed.local"])"};
        }
        if (url.query == "dc=A") {
            ++dc_requests;
            return HttpResponse{recover ? 200L : 500L, recover ? R"(["a.local"])" : "failure"};
        }
        return HttpResponse{200, R"(["b.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"b.local"}));
    EXPECT_EQ(dc_requests, 1);
    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
    recover = true;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"a.local"}));
    EXPECT_EQ(dc_requests, 2);
}

TEST(AlternatorLiveNodes, AffinityFailureRetainsExactSnapshotAndTriesExplicitFallbacks) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1", NewDCScope("B"));
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    int phase = 0;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) -> HttpResponse {
        if (url.query == "dc=A&rack=r1") {
            return {200, phase == 0 ? R"(["rack-a.local"])" : R"(["rack-b.local"])"};
        }
        if (url.query == "dc=A") {
            if (phase != 0) {
                throw std::runtime_error("preferred affinity discovery unavailable");
            }
            return {200, R"(["dc-a.local"])"};
        }
        EXPECT_EQ(url.query, "dc=B");
        return {200, phase == 2 ? R"(["dc-b.local"])" : "[]"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    phase = 1;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"rack-b.local"}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"dc-a.local"}));
    phase = 2;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"dc-b.local"}));
    phase = 0;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"dc-a.local"}));
}

TEST(AlternatorLiveNodes, CapturedSingleAndBatchAffinityPlansSurviveTopologyUpdates) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    bool changed = false;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        return HttpResponse{200, changed ? R"(["new.local"])" :
            url.query == "dc=A" ? R"(["a.local","b.local"])" : R"(["a.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    const auto old_membership = nodes.GetKnownAffinityNodes();
    const auto target = old_membership.back();
    const auto key = KeyWhereFirstNodeIs(old_membership, target);
    PartitionKeyMetadata metadata({{"orders", "id"}});
    auto single = QueryPlanForPartitionKey(nodes, {{"id", AttributeValue::String(key)}}, "orders", metadata);
    auto batch = QueryPlanForBatchWrite(nodes,
        {BatchWriteOperation::Put("orders", {{"id", AttributeValue::String(key)}})}, metadata);
    changed = true;
    nodes.UpdateLiveNodes();
    for (auto* plan : {&single, &batch}) {
        EXPECT_EQ(plan->Next(), target);
        EXPECT_EQ(plan->Next(), old_membership.front());
        EXPECT_TRUE(plan->Next().Empty());
        nodes.UpdateLiveNodes();
        EXPECT_TRUE(plan->Next().Empty());
    }
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"new.local"}));
}

TEST(AlternatorLiveNodes, AffinityPlansRetainCapturedCandidatesWhenHealthChangesDuringConstruction) {
    class CaptureNodes final : public NodesSource {
    public:
        explicit CaptureNodes(AlternatorLiveNodes& live) : live_(live) {}
        mutable std::function<void()> after_capture;
        std::vector<Url> GetActiveNodes() const override { return live_.GetActiveNodes(); }
        std::vector<Url> GetDownNodes() const override { return live_.GetDownNodes(); }
        std::vector<Url> GetQueryPlanNodes() const override { return live_.GetQueryPlanNodes(); }
        std::vector<Url> GetQueryPlanNodesForHash(std::int64_t hash) const override {
            return live_.GetQueryPlanNodesForHash(hash);
        }
        std::vector<Url> GetActiveKeyRouteAffinityNodes() const override {
            auto captured = live_.GetActiveKeyRouteAffinityNodes();
            if (after_capture) {
                auto callback = std::move(after_capture);
                after_capture = {};
                callback();
            }
            return captured;
        }
    private:
        AlternatorLiveNodes& live_;
    };
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        return HttpResponse{200, url.query == "dc=A" ? R"(["affinity.local"])" : R"(["ordinary.local"])"};
    });
    const Url affinity("http", "affinity.local", 8080);
    PartitionKeyMetadata metadata({{"orders", "id"}});
    const AttributeMap values{{"id", AttributeValue::String("key")}};
    for (const bool batch : {false, true}) {
        SCOPED_TRACE(batch ? "batch" : "single");
        AlternatorLiveNodes live({"seed.local"}, cfg, http);
        live.UpdateLiveNodes();
        CaptureNodes nodes(live);
        nodes.after_capture = [&] { live.ReportNodeResult(affinity, NodeHealthObservation::ConnectionFailure); };
        auto plan = batch
            ? QueryPlanForBatchWrite(nodes, {BatchWriteOperation::Put("orders", values)}, metadata)
            : QueryPlanForPartitionKey(nodes, values, "orders", metadata);
        EXPECT_EQ(plan.Next(), affinity);
        EXPECT_TRUE(plan.Next().Empty());
    }
}

TEST(AlternatorLiveNodes, BatchOrdinaryFallbackSamplesQuarantineOncePerPlan) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 3;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        return HttpResponse{200, url.query == "dc=A" ? R"(["affinity.local"])"
            : R"(["ordinary.local","quarantine.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    const Url ordinary("http", "ordinary.local", 8080);
    const Url quarantine("http", "quarantine.local", 8080);
    nodes.ReportNodeResult(quarantine, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();
    nodes.ReportNodeResult(Url("http", "affinity.local", 8080), NodeHealthObservation::ConnectionFailure);
    PartitionKeyMetadata metadata({{"orders", "id"}});
    std::vector<BatchWriteOperation> operations;
    for (const auto* key : {"first", "second", "third"}) {
        operations.push_back(BatchWriteOperation::Put("orders", {{"id", AttributeValue::String(key)}}));
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        auto plan = QueryPlanForBatchWrite(nodes, operations, metadata);
        std::vector<Url> candidates;
        for (auto node = plan.Next(); !node.Empty(); node = plan.Next()) {
            candidates.push_back(node);
        }
        EXPECT_EQ(SortAndDedupeNodes(std::move(candidates)),
            attempt == 3 ? SortAndDedupeNodes({ordinary, quarantine}) : std::vector<Url>({ordinary}));
    }
}

TEST(AlternatorLiveNodes, PublicHealthViewsIncludeAffinityOnlyNodesWithoutChangingOrdinaryRouting) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.quarantine_success_threshold = 2;
    cfg.node_health.quarantine_traffic_interval = 1;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        return HttpResponse{200, url.query == "dc=A" ? R"(["affinity.local","rack.local"])" :
            url.query.empty() ? "[]" : R"(["rack.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    const Url affinity("http", "affinity.local", 8080);
    nodes.ReportNodeResult(affinity, NodeHealthObservation::ConnectionFailure);
    EXPECT_EQ(nodes.GetDownNodes(), std::vector<Url>({affinity}));
    nodes.ProbeDownNodes();
    EXPECT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({affinity}));
    EXPECT_EQ(nodes.NextNode().host, "rack.local");
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodes()), std::vector<std::string>({"rack.local"}));
    nodes.ReportNodeResult(affinity, NodeHealthObservation::Success);
    nodes.ReportNodeResult(affinity, NodeHealthObservation::Success);
    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
    EXPECT_EQ(Hosts(nodes.GetActiveKeyRouteAffinityNodes()),
        std::vector<std::string>({"affinity.local", "rack.local"}));
}

TEST(AlternatorLiveNodes, HealthStateRemainsUntilANodeLeavesBothMemberships) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    int phase = 0;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.query.empty()) {
            return HttpResponse{500, ""};
        }
        const bool keep_first = url.query == "dc=A" ? phase == 0 : phase < 2;
        return HttpResponse{200, keep_first ? R"(["one.local","two.local"])" : R"(["two.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    const Url first("http", "one.local", 8080);
    nodes.ReportNodeResult(first, NodeHealthObservation::ConnectionFailure);
    phase = 1;
    nodes.UpdateLiveNodes();
    EXPECT_EQ(nodes.GetDownNodes(), std::vector<Url>({first}));
    EXPECT_EQ(Hosts(nodes.GetKnownAffinityNodes()), std::vector<std::string>({"two.local"}));
    phase = 2;
    nodes.UpdateLiveNodes();
    EXPECT_TRUE(nodes.GetDownNodes().empty());
}

TEST(AlternatorLiveNodes, QuarantinedAffinityTargetExcludesOrdinaryFallbackForSingleAndBatchPlans) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.quarantine_success_threshold = 3;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        return HttpResponse{200, url.query == "dc=A" ? R"(["affinity.local"])" : R"(["ordinary.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    const Url affinity("http", "affinity.local", 8080);
    nodes.ReportNodeResult(affinity, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();
    ASSERT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({affinity}));
    ASSERT_TRUE(nodes.GetActiveKeyRouteAffinityNodes().empty());
    ASSERT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"ordinary.local"}));

    PartitionKeyMetadata metadata({{"orders", "id"}});
    const AttributeMap values{{"id", AttributeValue::String("key")}};
    auto single = QueryPlanForPartitionKey(nodes, values, "orders", metadata);
    auto batch = QueryPlanForBatchWrite(nodes, {BatchWriteOperation::Put("orders", values)}, metadata);
    EXPECT_EQ(single.Next(), affinity);
    EXPECT_TRUE(single.Next().Empty());
    EXPECT_EQ(batch.Next(), affinity);
    EXPECT_TRUE(batch.Next().Empty());
}

TEST(AlternatorLiveNodes, AffinityWithoutUsableTargetsFallsBackToOrdinaryRouting) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        return HttpResponse{200, url.query == "dc=A" ? R"(["affinity.local"])" : R"(["rack.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    nodes.ReportNodeResult(Url("http", "affinity.local", 8080), NodeHealthObservation::ConnectionFailure);
    PartitionKeyMetadata metadata({{"orders", "id"}});
    auto single = QueryPlanForPartitionKey(nodes, {{"id", AttributeValue::String("key")}}, "orders", metadata);
    auto batch = QueryPlanForBatchWrite(nodes,
        {BatchWriteOperation::Put("orders", {{"id", AttributeValue::String("key")}})}, metadata);
    EXPECT_EQ(single.Next().host, "rack.local");
    EXPECT_EQ(batch.Next().host, "rack.local");
}

TEST(AlternatorLiveNodes, OrdinarySelectionProbesOnlyOrdinaryDownNodes) {
    Config cfg;
    cfg.routing_scope = NewRackScope("A", "r1");
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    std::vector<std::string> probes;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.query.empty()) {
            probes.push_back(url.host);
            return HttpResponse{200, "[]"};
        }
        return HttpResponse{200, url.query == "dc=A" ? R"(["affinity.local","rack.local"])" : R"(["rack.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    nodes.ReportNodeResult(Url("http", "affinity.local", 8080), NodeHealthObservation::ConnectionFailure);
    nodes.ReportNodeResult(Url("http", "rack.local", 8080), NodeHealthObservation::ConnectionFailure);
    EXPECT_EQ(nodes.NextNode().host, "rack.local");
    EXPECT_EQ(probes, std::vector<std::string>({"rack.local"}));
    EXPECT_EQ(Hosts(nodes.GetDownNodes()), std::vector<std::string>({"affinity.local"}));
    nodes.ProbeDownNodes();
    EXPECT_EQ(probes, std::vector<std::string>({"rack.local", "affinity.local"}));
}

TEST(AlternatorLiveNodes, HealthyTopologyReplacementKeepsProjectedViewsUsable) {
    Config cfg;
    cfg.key_route_affinity.mode = KeyRouteAffinityMode::AnyWrite;
    cfg.node_health.disabled = true;
    std::atomic<int> generation{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        return HttpResponse{200, generation.fetch_add(1) % 2 == 0 ? R"(["a.local"])" : R"(["b.local"])"};
    });
    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    std::atomic<bool> start{false};
    std::atomic<int> empty_views{0};
    std::thread reader([&] {
        while (!start.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 20000; ++i) {
            if (nodes.GetActiveNodes().empty() || nodes.GetActiveKeyRouteAffinityNodes().empty()) {
                ++empty_views;
            }
        }
    });
    start = true;
    for (int i = 0; i < 2000; ++i) {
        nodes.UpdateLiveNodes();
    }
    reader.join();
    EXPECT_EQ(empty_views.load(), 0);
}

TEST(AlternatorLiveNodes, SerializesConcurrentRefreshReconciliation) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::mutex request_mutex;
    std::condition_variable request_cv;
    int request_count = 0;
    int in_flight = 0;
    int max_in_flight = 0;
    bool second_refresh_started = false;
    bool release_first_request = false;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");

        std::unique_lock<std::mutex> lock(request_mutex);
        const auto request_number = ++request_count;
        ++in_flight;
        max_in_flight = std::max(max_in_flight, in_flight);
        request_cv.notify_all();
        if (request_number == 1) {
            request_cv.wait(lock, [&] { return release_first_request; });
        }
        --in_flight;
        lock.unlock();

        if (request_number == 1) {
            return HttpResponse{200, "[\"node1.local\"]"};
        }
        return HttpResponse{200, "[\"node1.local\",\"node2.local\"]"};
    });

    AlternatorLiveNodes nodes({"seed.local"}, cfg, http);
    std::thread first_refresh([&] { nodes.UpdateLiveNodes(); });
    bool first_request_started = false;
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        first_request_started = request_cv.wait_for(
            lock, std::chrono::seconds{5}, [&] { return request_count == 1; });
    }

    std::thread second_refresh([&] {
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            second_refresh_started = true;
        }
        request_cv.notify_all();
        nodes.UpdateLiveNodes();
    });

    bool requests_overlapped = false;
    bool second_refresh_observed = false;
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        second_refresh_observed = request_cv.wait_for(
            lock, std::chrono::seconds{5}, [&] { return second_refresh_started; });
        requests_overlapped = request_cv.wait_for(
            lock,
            std::chrono::milliseconds{100},
            [&] { return request_count > 1; });
        release_first_request = true;
    }
    request_cv.notify_all();
    first_refresh.join();
    second_refresh.join();

    EXPECT_TRUE(first_request_started);
    EXPECT_TRUE(second_refresh_observed);
    EXPECT_FALSE(requests_overlapped);
    EXPECT_EQ(max_in_flight, 1);
    EXPECT_EQ(request_count, 2);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node1.local", "node2.local"}));
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"node1.local", "node2.local"}));
}

TEST(AlternatorLiveNodes, DnsEntrypointDiscoversDnsNodeRecords) {
    LocalDnsEntrypointServer server(R"(["localhost","node-a.internal"])");
    Config cfg;
    cfg.scheme = "http";
    cfg.port = server.Port();
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    AlternatorLiveNodes nodes({"localhost"}, cfg);
    nodes.UpdateLiveNodes();
    server.Wait();

    EXPECT_NE(server.Request().find("GET /localnodes HTTP/1.1"), std::string::npos);
    EXPECT_TRUE(server.Request().find("Host: localhost:") != std::string::npos ||
                server.Request().find("host: localhost:") != std::string::npos);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"localhost", "node-a.internal"}));
}

TEST(AlternatorLiveNodes, IPv6LiteralDiscoversIPv6NodeRecords) {
    std::vector<std::string> requested_urls;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        requested_urls.push_back(url.ToString());
        if (url.host == "2001:db8::10") {
            return HttpResponse{200, R"(["2001:db8::20","node-a.internal"])"};
        }
        return HttpResponse{200, "[]"};
    });

    Config cfg;
    cfg.scheme = "https";
    cfg.port = 8043;
    cfg.routing_scope = std::make_shared<ClusterScope>();
    AlternatorLiveNodes nodes({"2001:db8::10"}, cfg, http);

    nodes.UpdateLiveNodes();

    EXPECT_EQ(requested_urls, std::vector<std::string>({"https://[2001:db8::10]:8043/localnodes"}));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"2001:db8::20", "node-a.internal"}));
    const auto discovered = nodes.GetNodes();
    ASSERT_EQ(discovered.size(), 2U);
    EXPECT_EQ(discovered[0].ToString(), "https://[2001:db8::20]:8043");
}

TEST(AlternatorLiveNodes, ClusterScopeRefreshUsesConfiguredSeedNodes) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::vector<std::string> requested_hosts;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "");
        requested_hosts.push_back(url.host);
        if (url.host == "seed-dc1.local") {
            return HttpResponse{200, "[\"dc1-node1.local\",\"dc1-node2.local\"]"};
        }
        if (url.host == "seed-dc2.local") {
            return HttpResponse{200, "[\"dc2-node1.local\",\"dc2-node2.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"seed-dc1.local", "seed-dc2.local"}, cfg, http);
    nodes.UpdateLiveNodes();
    nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({
                  "dc1-node1.local",
                  "dc1-node2.local",
                  "dc2-node1.local",
                  "dc2-node2.local",
              }));
    EXPECT_EQ(requested_hosts.size(), 4U);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "seed-dc1.local"), 2);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "seed-dc2.local"), 2);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyRejectsWrongDatacenter) {
    Config cfg;
    cfg.routing_scope = NewDCScope("wrongDC");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=wrongDC");
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyAcceptsCorrectDatacenter) {
    Config cfg;
    cfg.routing_scope = NewDCScope("datacenter1");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=datacenter1");
        ++requests;
        return HttpResponse{200, "[\"node1.local\"]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
    EXPECT_GT(requests.load(), 0);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyRejectsWrongRack) {
    Config cfg;
    cfg.routing_scope = NewRackScope("datacenter1", "wrongRack");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=datacenter1&rack=wrongRack");
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly(), std::runtime_error);
}

TEST(AlternatorLiveNodes, CheckIfRackAndDatacenterSetCorrectlyAcceptsCorrectRack) {
    Config cfg;
    cfg.routing_scope = NewRackScope("datacenter1", "rack1");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=datacenter1&rack=rack1");
        ++requests;
        return HttpResponse{200, "[\"node1.local\"]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_NO_THROW(nodes.CheckIfRackAndDatacenterSetCorrectly());
    EXPECT_GT(requests.load(), 0);
}

TEST(AlternatorLiveNodes, CheckRackDatacenterFeatureSupport) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        if (url.path == "/localnodes" && url.query == "rack=fakeRack") {
            return HttpResponse{200, "[]"};
        }
        if (url.path == "/localnodes") {
            return HttpResponse{200, "[\"node1.local\"]"};
        }
        return HttpResponse{200, ""};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
}

TEST(AlternatorLiveNodes, FeatureSupportProbeUsesSingleNode) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};

    std::vector<std::string> requested_hosts;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        requested_hosts.push_back(url.host);
        if (url.path == "/localnodes" && url.query == "rack=fakeRack") {
            return HttpResponse{200, "[]"};
        }
        if (url.path == "/localnodes") {
            return HttpResponse{200, "[\"node1.local\",\"node2.local\"]"};
        }
        return HttpResponse{500, ""};
    });

    AlternatorLiveNodes nodes({"node1.local", "node2.local"}, cfg, http);

    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
    ASSERT_EQ(requested_hosts.size(), 2U);
    EXPECT_EQ(requested_hosts[0], requested_hosts[1]);
}

TEST(AlternatorLiveNodes, ProbeDownNodesMovesResponsiveNodeToQuarantine) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    auto responsive = nodes.ProbeDownNodes();

    EXPECT_EQ(responsive, std::vector<Url>({recovering}));
    EXPECT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));
    EXPECT_TRUE(nodes.GetDownNodes().empty());
}

TEST(AlternatorLiveNodes, QuarantineTrafficIsSampledByConfiguredInterval) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url active("http", "active.local", 8080);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();

    EXPECT_EQ(nodes.NextNode(), active);
    EXPECT_EQ(nodes.NextNode(), recovering);
    EXPECT_EQ(nodes.NextNode(), active);
}

TEST(AlternatorLiveNodes, QuarantineHashAssignmentStaysExposedUntilVerified) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url active("http", "active.local", 8080);
    Url recovering("http", "recovering.local", 8080);
    const auto affinity_nodes = std::vector<Url>{active, recovering};
    const auto active_hash = HashWhereFirstNodeIs(affinity_nodes, active);
    const auto recovering_hash = HashWhereFirstNodeIs(affinity_nodes, recovering);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();

    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(active_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(active_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(active_hash)), std::vector<std::string>({"active.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::Success);
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::Success);
    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"active.local", "recovering.local"}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"active.local", "recovering.local"}));
}

TEST(AlternatorLiveNodes, QuarantineHashAssignmentIsRemovedWhenNodeGoesDown) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 1;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url active("http", "active.local", 8080);
    Url recovering("http", "recovering.local", 8080);
    const auto recovering_hash = HashWhereFirstNodeIs({active, recovering}, recovering);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();

    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)),
              std::vector<std::string>({"recovering.local", "active.local"}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    EXPECT_EQ(nodes.GetDownNodes(), std::vector<Url>({recovering}));
    EXPECT_EQ(Hosts(nodes.GetQueryPlanNodesForHash(recovering_hash)), std::vector<std::string>({"active.local"}));
}

TEST(AlternatorLiveNodes, QuarantinePromotesAfterSuccessfulTraffic) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);
    nodes.ProbeDownNodes();
    ASSERT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));

    nodes.ReportNodeResult(recovering, NodeHealthObservation::Success);

    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
    EXPECT_TRUE(nodes.GetDownNodes().empty());
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"active.local", "recovering.local"}));
}

TEST(AlternatorLiveNodes, BackgroundProbesDownNodesPeriodically) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{10};
    cfg.node_health.quarantine_success_threshold = 2;

    std::atomic<int> probes{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (url.host == "recovering.local" && url.path == "/localnodes") {
            ++probes;
        }
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    nodes.Start();
    for (int i = 0; i < 50 && nodes.GetQuarantinedNodes().empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    nodes.Stop();

    EXPECT_GT(probes.load(), 0);
    EXPECT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));
}

TEST(AlternatorLiveNodes, BackgroundRefreshUsesActivePeriodOnlyAfterActivity) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{10};
    cfg.idle_nodes_list_update_period = std::chrono::hours{1};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.path, "/localnodes");
        ++requests;
        return HttpResponse{200, "[\"node2.local\"]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    nodes.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    EXPECT_EQ(requests.load(), 0);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node1.local"}));

    EXPECT_EQ(nodes.NextNode().host, "node1.local");
    for (int i = 0; i < 50 && requests.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    nodes.Stop();

    EXPECT_GT(requests.load(), 0);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node2.local"}));
}

TEST(AlternatorLiveNodes, RejectsInvalidTlsSessionCacheConfig) {
    Config cfg;
    cfg.tls_session_cache_size = 0;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    EXPECT_THROW(AlternatorLiveNodes({"node1.local"}, cfg, http), std::invalid_argument);
}

TEST(AlternatorLiveNodes, RejectsDuplicateContentEncodingDecoders) {
    Config cfg;
    cfg.content_encoding_decoders = {
        std::make_shared<PassthroughContentEncodingDecoder>(std::vector<std::string>{"br"}),
        std::make_shared<PassthroughContentEncodingDecoder>(std::vector<std::string>{"BR"}),
    };

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    EXPECT_THROW(AlternatorLiveNodes({"node1.local"}, cfg, http), std::invalid_argument);
}

TEST(AlternatorLiveNodes, RejectsEmptyContentEncodingDecoderEncoding) {
    Config cfg;
    cfg.content_encoding_decoders = {
        std::make_shared<PassthroughContentEncodingDecoder>(std::vector<std::string>{""}),
    };

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    EXPECT_THROW(AlternatorLiveNodes({"node1.local"}, cfg, http), std::invalid_argument);
}

TEST(AlternatorLiveNodes, AcceptsCustomContentEncodingDecoder) {
    Config cfg;
    cfg.content_encoding_decoders = {std::make_shared<PassthroughContentEncodingDecoder>()};

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    EXPECT_NO_THROW(AlternatorLiveNodes({"node1.local"}, cfg, http));
}
