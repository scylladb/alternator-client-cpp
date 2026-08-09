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
#include <chrono>
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

class ResolvedFakeHttpClient final : public HttpClient {
public:
    using Resolver = std::function<std::vector<std::string>(const Url&)>;
    using Handler = std::function<HttpResponse(const Url&, const std::string&)>;

    ResolvedFakeHttpClient(Resolver resolver, Handler handler)
        : resolver_(std::move(resolver))
        , handler_(std::move(handler)) {}

    std::vector<std::string> Resolve(const Url& url) const override {
        return resolver_(url);
    }

    HttpResponse Get(const Url& url) const override {
        return handler_(url, url.host);
    }

    HttpResponse GetResolved(const Url& url, const std::string& resolved_address) const override {
        return handler_(url, resolved_address);
    }

private:
    Resolver resolver_;
    Handler handler_;
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

TEST(AlternatorLiveNodes, ScopedEmptyAddressWinsOverSiblingFailureAndUsesFallbackScope) {
    Config cfg;
    cfg.routing_scope = NewDCScope("missing", NewDCScope("fallback"));
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::vector<std::string> attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.80", "192.0.2.81"};
        },
        [&](const Url& url, const std::string& address) -> HttpResponse {
            attempts.push_back(url.query + "@" + address);
            if (url.query == "dc=missing") {
                if (address == "192.0.2.80") {
                    return {200, "[]"};
                }
                throw std::runtime_error("connection reset");
            }
            EXPECT_EQ(url.query, "dc=fallback");
            return {200, R"(["fallback.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    EXPECT_EQ(attempts,
              std::vector<std::string>({
                  "dc=missing@192.0.2.80",
                  "dc=missing@192.0.2.81",
                  "dc=fallback@192.0.2.80",
              }));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"fallback.internal"}));
}

TEST(AlternatorLiveNodes, WhollyUnusableAddressWinsOverSiblingFailureAndFailsClearly) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1", NewClusterScope());
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.82", "192.0.2.83"};
        },
        [](const Url&, const std::string& address) -> HttpResponse {
            if (address == "192.0.2.82") {
                return {200, R"(["","https://node.internal/path","node.internal:8080"])"};
            }
            throw std::runtime_error("connection reset");
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    try {
        nodes.UpdateLiveNodes();
        FAIL() << "wholly unusable response unexpectedly used fallback scope";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("no usable node hosts"), std::string::npos);
    }
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));
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

TEST(AlternatorLiveNodes, DnsEntrypointFallsBackAfterNon200MalformedAndEmptyResponses) {
    Config cfg;
    cfg.scheme = "https";
    cfg.port = 8043;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::vector<std::string> attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url& url) {
            EXPECT_EQ(url.host, "seed.example");
            return std::vector<std::string>{
                "192.0.2.9",
                "192.0.2.10",
                "192.0.2.10",
                "192.0.2.11",
                "192.0.2.12",
                "192.0.2.13",
            };
        },
        [&](const Url& url, const std::string& address) {
            EXPECT_EQ(url.host, "seed.example");
            EXPECT_EQ(url.scheme, "https");
            EXPECT_EQ(url.port, 8043);
            EXPECT_EQ(url.path, "/localnodes");
            attempts.push_back(address);
            if (address == "192.0.2.9") {
                throw std::runtime_error("connection reset");
            }
            if (address == "192.0.2.10") {
                return HttpResponse{503, ""};
            }
            if (address == "192.0.2.11") {
                return HttpResponse{200, R"({"nodes":["wrong-shape"]})"};
            }
            if (address == "192.0.2.12") {
                return HttpResponse{200, "[]"};
            }
            return HttpResponse{200, R"(["node-a.internal","node-b.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    EXPECT_EQ(attempts,
              std::vector<std::string>({
                  "192.0.2.9",
                  "192.0.2.10",
                  "192.0.2.11",
                  "192.0.2.12",
                  "192.0.2.13",
              }));
    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"node-a.internal", "node-b.internal"}));
}

TEST(AlternatorLiveNodes, AllResolvedAddressesUnavailableFailsAndPreservesLearnedNodes) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    int phase = 0;
    std::vector<std::string> failed_attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url& url) {
            EXPECT_EQ(url.host, "seed.example");
            if (phase == 0) {
                return std::vector<std::string>{"192.0.2.20"};
            }
            return std::vector<std::string>{"192.0.2.21", "192.0.2.22", "192.0.2.21"};
        },
        [&](const Url&, const std::string& address) -> HttpResponse {
            if (phase == 0) {
                EXPECT_EQ(address, "192.0.2.20");
                return {200, R"(["learned-a.internal","learned-b.internal"])"};
            }
            failed_attempts.push_back(address);
            throw std::runtime_error("connect failed: " + address);
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"learned-a.internal", "learned-b.internal"}));

    phase = 1;
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);

    EXPECT_EQ(failed_attempts,
              std::vector<std::string>({"192.0.2.21", "192.0.2.22"}));
    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"learned-a.internal", "learned-b.internal"}));
}

TEST(AlternatorLiveNodes, EmptyResolutionFailsClearlyWithoutDiscardingSeed) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"", ""};
        },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    try {
        nodes.UpdateLiveNodes();
        FAIL() << "empty DNS resolution unexpectedly succeeded";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("no usable addresses"), std::string::npos);
    }
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));
}

TEST(AlternatorLiveNodes, EmptyClusterResponsesFailClearly) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.25"};
        },
        [](const Url&, const std::string&) {
            return HttpResponse{200, "[]"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    try {
        nodes.UpdateLiveNodes();
        FAIL() << "empty cluster discovery unexpectedly succeeded";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("empty or unusable"), std::string::npos);
    }
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));
}

TEST(AlternatorLiveNodes, FailedImmediateRecoveryReturnsNoNode) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) -> std::vector<std::string> {
            throw std::runtime_error("SERVFAIL");
        },
        [](const Url&, const std::string&) -> HttpResponse {
            throw std::runtime_error("seed unavailable");
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.ReportNodeResult(
        Url("http", "seed.example", 8080),
        NodeHealthObservation::ConnectionFailure);

    EXPECT_TRUE(nodes.NextNode().Empty());
    EXPECT_EQ(nodes.GetDownNodes(),
              std::vector<Url>({Url("http", "seed.example", 8080)}));
}

TEST(AlternatorLiveNodes, AllApplicationResponsesInvalidFailAndPreserveLearnedNodes) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    int phase = 0;
    std::vector<std::string> attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url&) {
            if (phase == 0) {
                return std::vector<std::string>{"192.0.2.30"};
            }
            return std::vector<std::string>{"192.0.2.31", "192.0.2.32", "192.0.2.33"};
        },
        [&](const Url&, const std::string& address) {
            if (phase == 0) {
                return HttpResponse{200, R"(["learned.internal"])"};
            }
            attempts.push_back(address);
            if (address == "192.0.2.31") {
                return HttpResponse{502, ""};
            }
            if (address == "192.0.2.32") {
                return HttpResponse{200, "not-json"};
            }
            return HttpResponse{200, "[]"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    phase = 1;

    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(attempts,
              std::vector<std::string>({"192.0.2.31", "192.0.2.32", "192.0.2.33"}));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"learned.internal"}));
}

TEST(AlternatorLiveNodes, InvalidConfiguredSeedsDoNotBlockLaterUsableSeed) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::vector<std::string> attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url& url) {
            return std::vector<std::string>{"address-for-" + url.host};
        },
        [&](const Url& url, const std::string&) {
            attempts.push_back(url.host);
            if (url.host == "seed-a.example") {
                return HttpResponse{500, ""};
            }
            if (url.host == "seed-b.example") {
                return HttpResponse{200, "[]"};
            }
            return HttpResponse{200, R"(["learned.internal"])"};
        });

    AlternatorLiveNodes nodes(
        {"seed-a.example", "seed-b.example", "seed-c.example"},
        cfg,
        http);

    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    std::sort(attempts.begin(), attempts.end());
    EXPECT_EQ(attempts,
              std::vector<std::string>({
                  "seed-a.example",
                  "seed-b.example",
                  "seed-c.example",
              }));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"learned.internal"}));
}

TEST(AlternatorLiveNodes, MixedLocalNodesResponseKeepsUsableUniqueEntries) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.60"};
        },
        [](const Url&, const std::string&) {
            return HttpResponse{
                200,
                R"(["","   ","https://bad.internal/path","node.internal:8080","node-a.internal","node-a.internal","node-b.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"node-a.internal", "node-b.internal"}));
}

TEST(AlternatorLiveNodes, OverlappingRefreshesPublishOnlyCompleteNodeSets) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::mutex refresh_mutex;
    std::condition_variable refresh_cv;
    bool block_refresh = false;
    bool refresh_entered = false;
    int in_flight = 0;
    int max_in_flight = 0;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.70"};
        },
        [&](const Url&, const std::string&) {
            std::unique_lock<std::mutex> lock(refresh_mutex);
            ++in_flight;
            max_in_flight = std::max(max_in_flight, in_flight);
            if (block_refresh) {
                refresh_entered = true;
                refresh_cv.notify_all();
                refresh_cv.wait(lock, [&] { return !block_refresh; });
            }
            --in_flight;
            return HttpResponse{
                200,
                refresh_entered
                    ? R"(["new-a.internal","new-b.internal"])"
                    : R"(["old-a.internal","old-b.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"old-a.internal", "old-b.internal"}));

    {
        std::lock_guard<std::mutex> lock(refresh_mutex);
        block_refresh = true;
        refresh_entered = false;
    }
    std::thread first_refresh([&] { nodes.UpdateLiveNodes(); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(refresh_mutex);
        entered = refresh_cv.wait_for(
            lock,
            std::chrono::seconds(1),
            [&] { return refresh_entered; });
        if (!entered) {
            block_refresh = false;
        }
    }
    if (!entered) {
        refresh_cv.notify_all();
        first_refresh.join();
        FAIL() << "refresh did not enter HTTP handler";
    }
    std::thread second_refresh([&] { nodes.UpdateLiveNodes(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"old-a.internal", "old-b.internal"}));
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()),
              std::vector<std::string>({"old-a.internal", "old-b.internal"}));
    {
        std::lock_guard<std::mutex> lock(refresh_mutex);
        EXPECT_EQ(in_flight, 1);
        EXPECT_EQ(max_in_flight, 1);
        block_refresh = false;
    }
    refresh_cv.notify_all();
    first_refresh.join();
    second_refresh.join();

    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"new-a.internal", "new-b.internal"}));
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()),
              std::vector<std::string>({"new-a.internal", "new-b.internal"}));
    EXPECT_EQ(max_in_flight, 1);
}

TEST(AlternatorLiveNodes, ActiveSessionRecoversThroughReresolvedSeedEntrypoint) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    int phase = 0;
    int seed_resolutions = 0;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url& url) {
            if (url.host == "live-a.internal" || url.host == "live-b.internal") {
                return std::vector<std::string>{url.host};
            }
            EXPECT_EQ(url.host, "seed.example");
            EXPECT_EQ(url.query, "dc=dc1");
            ++seed_resolutions;
            return phase == 0
                ? std::vector<std::string>{"192.0.2.40"}
                : std::vector<std::string>{"192.0.2.41"};
        },
        [&](const Url& url, const std::string& address) -> HttpResponse {
            if (url.host == "live-a.internal" || url.host == "live-b.internal") {
                throw std::runtime_error("learned node unavailable");
            }
            EXPECT_EQ(url.host, "seed.example");
            if (address == "192.0.2.40") {
                return {200, R"(["live-a.internal","live-b.internal"])"};
            }
            EXPECT_EQ(address, "192.0.2.41");
            return {200, R"(["live-c.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"live-a.internal", "live-b.internal"}));

    nodes.ReportNodeResult(Url("http", "live-a.internal", 8080), NodeHealthObservation::ConnectionFailure);
    nodes.ReportNodeResult(Url("http", "live-b.internal", 8080), NodeHealthObservation::ConnectionFailure);
    phase = 1;

    EXPECT_EQ(nodes.NextNode(), Url("http", "live-c.internal", 8080));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"live-c.internal"}));
    EXPECT_EQ(seed_resolutions, 2);
}

TEST(AlternatorLiveNodes, FailedDnsResolutionRetainsSeedForLaterRecovery) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    int resolution_attempts = 0;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url&) {
            ++resolution_attempts;
            if (resolution_attempts == 1) {
                throw std::runtime_error("NXDOMAIN");
            }
            return std::vector<std::string>{"192.0.2.50"};
        },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["recovered.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));

    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"recovered.internal"}));
    EXPECT_EQ(resolution_attempts, 2);
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
        return HttpResponse{200, "[\"recovering.local\"]"};
    });

    AlternatorLiveNodes nodes({"active.local", "recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    auto responsive = nodes.ProbeDownNodes();

    EXPECT_EQ(responsive, std::vector<Url>({recovering}));
    EXPECT_EQ(nodes.GetQuarantinedNodes(), std::vector<Url>({recovering}));
    EXPECT_TRUE(nodes.GetDownNodes().empty());
}

TEST(AlternatorLiveNodes, ProbeDownNodesRejectsEmptyLocalNodesResponse) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"recovering.local"}, cfg, http);
    Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    EXPECT_TRUE(nodes.ProbeDownNodes().empty());
    EXPECT_EQ(nodes.GetDownNodes(), std::vector<Url>({recovering}));
    EXPECT_TRUE(nodes.GetQuarantinedNodes().empty());
}

TEST(AlternatorLiveNodes, QuarantineTrafficIsSampledByConfiguredInterval) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 3;
    cfg.node_health.quarantine_traffic_interval = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[\"recovering.local\"]"};
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
        return HttpResponse{200, "[\"recovering.local\"]"};
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
        return HttpResponse{200, "[\"recovering.local\"]"};
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
        return HttpResponse{200, "[\"recovering.local\"]"};
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
        return HttpResponse{200, "[\"recovering.local\"]"};
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
