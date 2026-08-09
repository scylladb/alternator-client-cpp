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
#include <future>
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

class DeadlineAwareFakeHttpClient final : public HttpClient {
public:
    using Resolver = std::function<std::vector<std::string>(const Url&)>;
    using Handler = std::function<HttpResponse(
        const Url&,
        const std::string&,
        std::chrono::milliseconds)>;

    DeadlineAwareFakeHttpClient(Resolver resolver, Handler handler)
        : resolver_(std::move(resolver))
        , handler_(std::move(handler)) {}

    std::vector<std::string> Resolve(const Url& url) const override {
        return resolver_(url);
    }

    HttpResponse Get(const Url& url) const override {
        return handler_(url, url.host, std::chrono::milliseconds{100});
    }

    HttpResponse GetResolved(
        const Url& url,
        const std::string& resolved_address) const override {
        return handler_(url, resolved_address, std::chrono::milliseconds{100});
    }

    HttpResponse GetResolvedWithTimeout(
        const Url& url,
        const std::string& resolved_address,
        std::chrono::milliseconds timeout) const override {
        return handler_(url, resolved_address, timeout);
    }

private:
    Resolver resolver_;
    Handler handler_;
};

class ResolverGate {
public:
    std::vector<std::string> Resolve() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++calls_;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        ++finished_;
        condition_.notify_all();
        return {"127.0.0.1"};
    }

    [[nodiscard]] bool WaitForCalls(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, expected] {
            return calls_ >= expected;
        });
    }

    [[nodiscard]] bool WaitForFinished(
        std::size_t expected,
        std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, expected] {
            return finished_ >= expected;
        });
    }

    [[nodiscard]] std::size_t Calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t calls_ = 0;
    std::size_t finished_ = 0;
    bool released_ = false;
};

class ResolverReleaseGuard {
public:
    explicit ResolverReleaseGuard(std::shared_ptr<ResolverGate> gate)
        : gate_(std::move(gate)) {}

    ~ResolverReleaseGuard() {
        gate_->Release();
    }

    ResolverReleaseGuard(const ResolverReleaseGuard&) = delete;
    ResolverReleaseGuard& operator=(const ResolverReleaseGuard&) = delete;

private:
    std::shared_ptr<ResolverGate> gate_;
};

class DestructorGate {
public:
    void Block() {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        finished_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] bool WaitForEntered(
        std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    [[nodiscard]] bool WaitForFinished(
        std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return finished_; });
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
    bool finished_ = false;
};

class DestructorReleaseGuard {
public:
    explicit DestructorReleaseGuard(std::shared_ptr<DestructorGate> gate)
        : gate_(std::move(gate)) {}

    ~DestructorReleaseGuard() {
        gate_->Release();
    }

    DestructorReleaseGuard(const DestructorReleaseGuard&) = delete;
    DestructorReleaseGuard& operator=(const DestructorReleaseGuard&) = delete;

private:
    std::shared_ptr<DestructorGate> gate_;
};

class BlockingDestructorHttpClient final : public HttpClient {
public:
    BlockingDestructorHttpClient(
        std::shared_ptr<ResolverGate> resolver_gate,
        std::shared_ptr<DestructorGate> destructor_gate)
        : resolver_gate_(std::move(resolver_gate))
        , destructor_gate_(std::move(destructor_gate)) {}

    ~BlockingDestructorHttpClient() override {
        destructor_gate_->Block();
    }

    std::vector<std::string> Resolve(const Url&) const override {
        return resolver_gate_->Resolve();
    }

    HttpResponse Get(const Url&) const override {
        return {200, R"(["resolved.internal"])"};
    }

    HttpResponse GetResolved(const Url&, const std::string&) const override {
        return {200, R"(["resolved.internal"])"};
    }

private:
    std::shared_ptr<ResolverGate> resolver_gate_;
    std::shared_ptr<DestructorGate> destructor_gate_;
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

class CyclicRoutingScope final
    : public RoutingScope
    , public std::enable_shared_from_this<CyclicRoutingScope> {
public:
    std::string Name() const override {
        return "Cyclic";
    }

    std::string ToString() const override {
        return "Cyclic()";
    }

    std::string LocalNodesQuery() const override {
        return "dc=missing";
    }

    RoutingScopePtr Fallback() const override {
        return shared_from_this();
    }
};

class MutableFallbackRoutingScope final : public RoutingScope {
public:
    std::string Name() const override {
        return "Mutable";
    }

    std::string ToString() const override {
        return "Mutable()";
    }

    std::string LocalNodesQuery() const override {
        return "dc=mutable";
    }

    RoutingScopePtr Fallback() const override {
        return fallback_;
    }

    void SetFallback(RoutingScopePtr fallback) {
        fallback_ = std::move(fallback);
    }

private:
    RoutingScopePtr fallback_;
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

TEST(AlternatorLiveNodes, ScopedEntrypointIsNeverRoutedBeforeMatchingDiscovery) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> discovery_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.host, "seed.example");
        EXPECT_EQ(url.path, "/localnodes");
        EXPECT_EQ(url.query, "dc=dc1");
        ++discovery_requests;
        return HttpResponse{200, "[]"};
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    EXPECT_TRUE(nodes.GetActiveNodes().empty());
    EXPECT_TRUE(nodes.NextNode().Empty());
    EXPECT_EQ(discovery_requests.load(), 1);
    EXPECT_TRUE(nodes.GetNodes().empty());
}

TEST(AlternatorLiveNodes, ClusterEntrypointRemainsRoutableBeforeDiscovery) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{500, {}};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"seed.example"}));
}

TEST(AlternatorLiveNodes, TerminalClusterFallbackAuthorizesEntrypointBeforeDiscovery) {
    Config cfg;
    cfg.routing_scope = NewRackScope(
        "dc1",
        "rack1",
        NewDCScope("dc1", NewClusterScope()));

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{500, {}};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"seed.example"}));
}

TEST(AlternatorLiveNodes, OverdepthClusterFallbackDoesNotAuthorizeEntrypoint) {
    RoutingScopePtr scope = NewClusterScope();
    for (std::size_t index = 0; index < 64U; ++index) {
        scope = NewDCScope("dc" + std::to_string(index), std::move(scope));
    }

    Config cfg;
    cfg.routing_scope = std::move(scope);
    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        ++requests;
        return HttpResponse{200, R"(["escaped.internal"])"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    EXPECT_TRUE(nodes.GetNodes().empty());
    EXPECT_TRUE(nodes.GetActiveNodes().empty());
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(requests.load(), 0);
}

TEST(AlternatorLiveNodes, ScopedEntrypointDiscoversMatchingNodeBeforeRouting) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    auto http = std::make_shared<FakeHttpClient>([](const Url& url) {
        EXPECT_EQ(url.host, "seed.example");
        EXPECT_EQ(url.query, "dc=dc1");
        return HttpResponse{200, R"(["dc1-node.internal"])"};
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    EXPECT_EQ(nodes.NextNode(), Url("http", "dc1-node.internal", 8080));
    EXPECT_EQ(Hosts(nodes.GetActiveNodes()), std::vector<std::string>({"dc1-node.internal"}));
}

TEST(AlternatorLiveNodes, EmptyScopedDiscoveryFailsWithScopeError) {
    Config cfg;
    cfg.routing_scope = NewRackScope("dc1", "rack1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    try {
        nodes.UpdateLiveNodes();
        FAIL() << "empty scoped discovery unexpectedly succeeded";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("routing scope has no usable nodes"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Rack(dc=dc1, rack=rack1)"), std::string::npos);
    }
    EXPECT_TRUE(nodes.GetActiveNodes().empty());
}

TEST(AlternatorLiveNodes, EmptyScopedRefreshRemovesPreviouslyRoutableNodes) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<bool> scope_has_nodes{true};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.query, "dc=dc1");
        return scope_has_nodes.load()
            ? HttpResponse{200, R"(["dc1-node.internal"])"}
            : HttpResponse{200, "[]"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(
        Hosts(nodes.GetActiveNodes()),
        std::vector<std::string>({"dc1-node.internal"}));

    scope_has_nodes.store(false);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);

    EXPECT_TRUE(nodes.GetActiveNodes().empty());
    EXPECT_TRUE(nodes.NextNode().Empty());
    EXPECT_TRUE(nodes.GetNodes().empty());
}

TEST(AlternatorLiveNodes, EmptyPrimaryScopeFailsClosedWhenFallbackDiscoveryFails) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1", NewDCScope("dc2"));
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<bool> initial_refresh{true};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (initial_refresh.load()) {
            EXPECT_EQ(url.query, "dc=dc1");
            return HttpResponse{200, R"(["dc1-node.internal"])"};
        }
        if (url.query == "dc=dc1") {
            return HttpResponse{200, "[]"};
        }
        return HttpResponse{400, "fallback query rejected"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_FALSE(nodes.GetActiveNodes().empty());

    initial_refresh.store(false);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);

    EXPECT_TRUE(nodes.GetActiveNodes().empty());
    EXPECT_TRUE(nodes.GetNodes().empty());
}

TEST(AlternatorLiveNodes, EmptyPrimaryScopeRetainsEqualNamedFallbackByIdentity) {
    Config cfg;
    cfg.routing_scope = NewDCScope("same", NewDCScope("same"));
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> phase{0};
    std::atomic<int> phase_requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.query, "dc=same");
        const auto request = phase_requests.fetch_add(1);
        if (phase.load() == 0) {
            return request == 0
                ? HttpResponse{200, "[]"}
                : HttpResponse{200, R"(["fallback-node.internal"])"};
        }
        if (request < 2) {
            return HttpResponse{200, "[]"};
        }
        return HttpResponse{503, "fallback unavailable"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({"fallback-node.internal"}));

    phase.store(1);
    phase_requests.store(0);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);

    EXPECT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({"fallback-node.internal"}));
}

TEST(AlternatorLiveNodes, EmptyStrictScopeAndEmptyClusterRetainClusterOriginSeed) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1", NewClusterScope());
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, "[]"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    ASSERT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));

    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"seed.example"}));
}

TEST(AlternatorLiveNodes, InvalidFallbackGraphErrorsRetainPublishedSnapshot) {
    auto mutable_scope = std::make_shared<MutableFallbackRoutingScope>();
    Config cfg;
    cfg.routing_scope = mutable_scope;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        EXPECT_EQ(url.query, "dc=mutable");
        ++requests;
        return HttpResponse{200, R"(["stable.internal"])"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"stable.internal"}));

    mutable_scope->SetFallback(mutable_scope);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"stable.internal"}));

    RoutingScopePtr overdepth_scope;
    for (std::size_t index = 0; index < 64U; ++index) {
        overdepth_scope = NewDCScope(
            "deep" + std::to_string(index),
            std::move(overdepth_scope));
    }
    mutable_scope->SetFallback(std::move(overdepth_scope));
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"stable.internal"}));
    EXPECT_EQ(requests.load(), 1);
}

TEST(AlternatorLiveNodes, CyclicEmptyScopeFailsWithoutRepeatedDiscovery) {
    Config cfg;
    cfg.routing_scope = std::make_shared<CyclicRoutingScope>();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        ++requests;
        return HttpResponse{200, "[]"};
    });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(requests.load(), 0);
    EXPECT_TRUE(nodes.GetActiveNodes().empty());
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

TEST(AlternatorLiveNodes, ClusterRefreshTriesLearnedNodesBeforeRetainedSeeds) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> phase{0};
    std::vector<std::string> second_refresh_hosts;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (phase.load() == 0) {
            EXPECT_EQ(url.host, "seed.example");
            return HttpResponse{200, R"(["learned.internal"])"};
        }
        second_refresh_hosts.push_back(url.host);
        if (url.host == "learned.internal") {
            return HttpResponse{200, R"(["recovered.internal"])"};
        }
        return HttpResponse{503, "seed unavailable"};
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    phase.store(1);
    nodes.UpdateLiveNodes();

    ASSERT_FALSE(second_refresh_hosts.empty());
    EXPECT_EQ(second_refresh_hosts.front(), "learned.internal");
    EXPECT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({"learned.internal", "recovered.internal"}));
}

TEST(AlternatorLiveNodes, PartialClusterRefreshRetainsUnavailablePartition) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> phase{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (phase.load() == 0) {
            return HttpResponse{200, R"(["dc-a.internal","dc-b.internal"])"};
        }
        if (url.host == "dc-a.internal") {
            return HttpResponse{200, R"(["dc-a-new.internal"])"};
        }
        throw std::runtime_error("partition unavailable");
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    phase.store(1);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({
            "dc-a-new.internal",
            "dc-a.internal",
            "dc-b.internal",
        }));
}

TEST(AlternatorLiveNodes, CompleteClusterRefreshReplacesPreviousSnapshot) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> phase{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        return phase.load() == 0
            ? HttpResponse{200, R"(["old.internal"])"}
            : HttpResponse{200, R"(["fresh.internal"])"};
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"old.internal"}));

    phase.store(1);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"fresh.internal"}));
}

TEST(AlternatorLiveNodes, PartialClusterUnionPrioritizesFreshNodesWithinBound) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.max_discovery_response_bytes = 35U;

    std::atomic<int> phase{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (phase.load() == 0) {
            return HttpResponse{200, R"(["old-a.internal","old-b.internal"])"};
        }
        if (url.host == "old-a.internal") {
            return HttpResponse{200, R"(["fresh.internal"])"};
        }
        throw std::runtime_error("partition unavailable");
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({"old-a.internal", "old-b.internal"}));

    phase.store(1);
    nodes.UpdateLiveNodes();

    EXPECT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({"fresh.internal", "old-a.internal"}));
}

TEST(AlternatorLiveNodes, ClusterAggregateDiscoveryLimitPreservesPreviousSnapshot) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.max_discovery_response_bytes = 48;

    std::atomic<int> phase{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (phase.load() == 0) {
            return HttpResponse{200, R"(["stable.internal"])"};
        }
        if (url.host == "seed-a.example") {
            return HttpResponse{200, R"(["node-a1.internal","node-a2.internal"])"};
        }
        return HttpResponse{200, R"(["node-b1.internal","node-b2.internal"])"};
    });

    AlternatorLiveNodes nodes({"seed-a.example", "seed-b.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"stable.internal"}));

    phase.store(1);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"stable.internal"}));
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

TEST(AlternatorLiveNodes, InvalidJsonAndDnsHostsFallBackToLaterAddress) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    const std::string invalid_utf8 =
        std::string{"[\"wrong-utf8.internal\",\"bad"} +
        static_cast<char>(0xc0) + static_cast<char>(0xaf) + "\"]";
    std::vector<std::string> attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{
                "192.0.2.201",
                "192.0.2.202",
                "192.0.2.203",
                "192.0.2.204",
                "192.0.2.205",
            };
        },
        [&](const Url&, const std::string& address) {
            attempts.push_back(address);
            if (address == "192.0.2.201") {
                return HttpResponse{200, "[\"wrong-control.internal\",\"bad\nname\"]"};
            }
            if (address == "192.0.2.202") {
                return HttpResponse{200, invalid_utf8};
            }
            if (address == "192.0.2.203") {
                return HttpResponse{200, "[\f\"wrong-whitespace.internal\"]"};
            }
            if (address == "192.0.2.204") {
                return HttpResponse{200, R"(["bad..name"])"};
            }
            return HttpResponse{200, R"(["good\u002einternal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    EXPECT_EQ(attempts,
              std::vector<std::string>({
                  "192.0.2.201",
                  "192.0.2.202",
                  "192.0.2.203",
                  "192.0.2.204",
                  "192.0.2.205",
              }));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"good.internal"}));
}

TEST(AlternatorLiveNodes, OversizedResponseFallsBackToNextResolvedAddress) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.max_discovery_response_bytes = 32;

    std::vector<std::string> attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.14", "192.0.2.15"};
        },
        [&](const Url&, const std::string& address) {
            attempts.push_back(address);
            if (address == "192.0.2.14") {
                return HttpResponse{200, R"(["node-name-that-exceeds-the-configured-discovery-response-limit.internal"])"};
            }
            return HttpResponse{200, R"(["node-ok.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());

    EXPECT_EQ(attempts, std::vector<std::string>({"192.0.2.14", "192.0.2.15"}));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node-ok.internal"}));
}

TEST(AlternatorLiveNodes, AllResolvedAddressesUnavailableFailsAndPreservesLearnedNodes) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    int phase = 0;
    std::vector<std::string> resolved_hosts;
    std::vector<std::string> failed_attempts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url& url) {
            if (phase == 0) {
                EXPECT_EQ(url.host, "seed.example");
                return std::vector<std::string>{"192.0.2.20"};
            }
            resolved_hosts.push_back(url.host);
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

    ASSERT_EQ(resolved_hosts.size(), 3U);
    EXPECT_EQ(resolved_hosts.back(), "seed.example");
    std::sort(resolved_hosts.begin(), resolved_hosts.begin() + 2);
    EXPECT_EQ(
        std::vector<std::string>(resolved_hosts.begin(), resolved_hosts.begin() + 2),
        std::vector<std::string>({"learned-a.internal", "learned-b.internal"}));
    EXPECT_EQ(failed_attempts,
              std::vector<std::string>({
                  "192.0.2.21", "192.0.2.22",
                  "192.0.2.21", "192.0.2.22",
                  "192.0.2.21", "192.0.2.22",
              }));
    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({"learned-a.internal", "learned-b.internal"}));
}

TEST(AlternatorLiveNodes, MultiAddressSeedCannotStarveLaterSeed) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::seconds{2};
    cfg.discovery_cycle_timeout = std::chrono::milliseconds{240};

    std::mutex state_mutex;
    std::string first_seed;
    std::vector<std::string> resolved_seeds;
    std::vector<std::chrono::milliseconds> first_seed_timeouts;
    auto http = std::make_shared<DeadlineAwareFakeHttpClient>(
        [&](const Url& url) {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (first_seed.empty()) {
                first_seed = url.host;
            }
            resolved_seeds.push_back(url.host);
            if (url.host == first_seed) {
                return std::vector<std::string>{
                    "192.0.2.211",
                    "192.0.2.212",
                    "192.0.2.213",
                    "192.0.2.214",
                };
            }
            return std::vector<std::string>{"192.0.2.215"};
        },
        [&](const Url& url,
            const std::string&,
            std::chrono::milliseconds timeout) -> HttpResponse {
            bool is_first_seed = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                is_first_seed = url.host == first_seed;
                if (is_first_seed) {
                    first_seed_timeouts.push_back(timeout);
                }
            }
            if (!is_first_seed) {
                return {200, R"(["dc1-node.internal"])"};
            }
            const auto delay = timeout > std::chrono::milliseconds::zero()
                ? std::min(timeout, std::chrono::milliseconds{100})
                : std::chrono::milliseconds{100};
            std::this_thread::sleep_for(delay);
            throw std::runtime_error("address unavailable");
        });

    AlternatorLiveNodes nodes(
        {"seed-a.example", "seed-b.example"},
        cfg,
        http);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"dc1-node.internal"}));
    ASSERT_EQ(resolved_seeds.size(), 2U);
    EXPECT_NE(resolved_seeds[0], resolved_seeds[1]);
    ASSERT_FALSE(first_seed_timeouts.empty());
    for (const auto timeout : first_seed_timeouts) {
        EXPECT_GT(timeout, std::chrono::milliseconds::zero());
        EXPECT_LT(timeout, std::chrono::milliseconds{100});
    }
    EXPECT_LT(elapsed, std::chrono::milliseconds{350});
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

TEST(AlternatorLiveNodes, StalledResolverReturnsByDiscoveryTimeout) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds{60};

    auto gate = std::make_shared<ResolverGate>();
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [gate](const Url&) { return gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    const auto started_at = std::chrono::steady_clock::now();
    auto update = std::async(std::launch::async, [&] {
        try {
            nodes.UpdateLiveNodes();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    ResolverReleaseGuard release_on_exit(gate);
    const bool resolver_started = gate->WaitForCalls(1);
    const auto status = update.wait_for(std::chrono::seconds{1});
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    gate->Release();
    update.wait();
    const auto error = update.get();

    EXPECT_TRUE(resolver_started);
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_NE(error.find("DNS resolution timed out"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::milliseconds{30});
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
    EXPECT_TRUE(gate->WaitForFinished(1));
}

TEST(AlternatorLiveNodes, DiscoveryCycleTimeoutBoundsStalledResolver) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_cycle_timeout = std::chrono::milliseconds{60};

    auto gate = std::make_shared<ResolverGate>();
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [gate](const Url&) { return gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    const auto started_at = std::chrono::steady_clock::now();
    auto update = std::async(std::launch::async, [&] {
        try {
            nodes.UpdateLiveNodes();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    ResolverReleaseGuard release_on_exit(gate);
    const bool resolver_started = gate->WaitForCalls(1);
    const auto status = update.wait_for(std::chrono::seconds{1});
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    gate->Release();
    update.wait();
    const auto error = update.get();

    EXPECT_TRUE(resolver_started);
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_NE(error.find("DNS resolution timed out"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::milliseconds{30});
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
    EXPECT_TRUE(gate->WaitForFinished(1));
}

TEST(AlternatorLiveNodes, DiscoveryDeadlineBoundsNonCooperativeLegacyTransport) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds{60};
    cfg.discovery_cycle_timeout = std::chrono::milliseconds::zero();

    auto gate = std::make_shared<ResolverGate>();
    ResolverReleaseGuard release_on_exit(gate);
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) -> std::vector<std::string> {
            ADD_FAILURE() << "canonical literal unexpectedly used the resolver";
            return {};
        },
        [gate](const Url&, const std::string&) {
            (void)gate->Resolve();
            return HttpResponse{200, R"(["late.internal"])"};
        });
    AlternatorLiveNodes nodes({"192.0.2.200"}, cfg, http);

    const auto started_at = std::chrono::steady_clock::now();
    std::string error;
    try {
        nodes.UpdateLiveNodes();
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_at;

    EXPECT_TRUE(gate->WaitForCalls(1));
    EXPECT_NE(error.find("discovery request timed out"), std::string::npos);
    EXPECT_GE(elapsed, std::chrono::milliseconds{30});
    EXPECT_LT(elapsed, std::chrono::milliseconds{500});
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"192.0.2.200"}));

    gate->Release();
    EXPECT_TRUE(gate->WaitForFinished(1));
}

TEST(AlternatorLiveNodes, ZeroDiscoveryTimeoutWaitsForResolverCompletion) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_cycle_timeout = std::chrono::milliseconds::zero();

    auto gate = std::make_shared<ResolverGate>();
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [gate](const Url&) { return gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["resolved.internal"])"};
        });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    auto update = std::async(std::launch::async, [&] {
        nodes.UpdateLiveNodes();
    });
    ResolverReleaseGuard release_on_exit(gate);
    const bool resolver_started = gate->WaitForCalls(1);
    const auto status_while_blocked = update.wait_for(std::chrono::milliseconds{50});
    gate->Release();
    update.wait();

    EXPECT_TRUE(resolver_started);
    EXPECT_EQ(status_while_blocked, std::future_status::timeout);
    EXPECT_NO_THROW(update.get());
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"resolved.internal"}));
}

TEST(AlternatorLiveNodes, ConcurrentStalledResolutionsAreCoalesced) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds{100};

    auto gate = std::make_shared<ResolverGate>();
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [gate](const Url&) { return gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });
    AlternatorLiveNodes first({"seed.example"}, cfg, http);
    AlternatorLiveNodes second({"seed.example"}, cfg, http);
    std::promise<void> start_updates;
    const auto start_signal = start_updates.get_future().share();
    auto first_update = std::async(std::launch::async, [&] {
        start_signal.wait();
        try {
            first.UpdateLiveNodes();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    auto second_update = std::async(std::launch::async, [&] {
        start_signal.wait();
        try {
            second.UpdateLiveNodes();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    ResolverReleaseGuard release_on_exit(gate);

    start_updates.set_value();
    const bool resolver_started = gate->WaitForCalls(1);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    const auto resolver_calls = gate->Calls();
    const auto first_status = first_update.wait_for(std::chrono::seconds{1});
    const auto second_status = second_update.wait_for(std::chrono::seconds{1});
    gate->Release();
    first_update.wait();
    second_update.wait();
    const auto first_error = first_update.get();
    const auto second_error = second_update.get();

    EXPECT_TRUE(resolver_started);
    EXPECT_EQ(resolver_calls, 1U);
    EXPECT_EQ(first_status, std::future_status::ready);
    EXPECT_EQ(second_status, std::future_status::ready);
    EXPECT_NE(first_error.find("DNS resolution timed out"), std::string::npos);
    EXPECT_NE(second_error.find("DNS resolution timed out"), std::string::npos);
    EXPECT_TRUE(gate->WaitForFinished(1));
}

TEST(AlternatorLiveNodes, ReservedSeedResolversRecoverFromStalledLearnedPoolAndSeed) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.node_health.disabled = true;
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds{40};
    cfg.discovery_cycle_timeout = std::chrono::milliseconds{400};

    auto first_learned_gate = std::make_shared<ResolverGate>();
    auto second_learned_gate = std::make_shared<ResolverGate>();
    auto queued_learned_gate = std::make_shared<ResolverGate>();
    auto first_seed_gate = std::make_shared<ResolverGate>();
    ResolverReleaseGuard release_first_learned(first_learned_gate);
    ResolverReleaseGuard release_second_learned(second_learned_gate);
    ResolverReleaseGuard release_queued_learned(queued_learned_gate);
    ResolverReleaseGuard release_first_seed(first_seed_gate);
    std::atomic<int> active_resolvers{0};
    std::atomic<int> maximum_active_resolvers{0};
    std::atomic<int> healthy_seed_resolutions{0};

    const auto update_maximum = [&](int active) {
        auto maximum = maximum_active_resolvers.load(std::memory_order_relaxed);
        while (maximum < active &&
               !maximum_active_resolvers.compare_exchange_weak(
                   maximum,
                   active,
                   std::memory_order_relaxed)) {
        }
    };
    const auto resolve_through_gate = [&](const std::shared_ptr<ResolverGate>& gate) {
        const auto active =
            active_resolvers.fetch_add(1, std::memory_order_relaxed) + 1;
        update_maximum(active);
        auto addresses = gate->Resolve();
        active_resolvers.fetch_sub(1, std::memory_order_relaxed);
        return addresses;
    };

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url& url) {
            if (url.host == "stalled-learned-a.example") {
                return resolve_through_gate(first_learned_gate);
            }
            if (url.host == "stalled-learned-b.example") {
                return resolve_through_gate(second_learned_gate);
            }
            if (url.host == "queued-learned.example") {
                return resolve_through_gate(queued_learned_gate);
            }
            if (url.host == "stalled-seed.example") {
                return resolve_through_gate(first_seed_gate);
            }
            EXPECT_EQ(url.host, "healthy-seed.example");
            const auto active =
                active_resolvers.fetch_add(1, std::memory_order_relaxed) + 1;
            update_maximum(active);
            ++healthy_seed_resolutions;
            active_resolvers.fetch_sub(1, std::memory_order_relaxed);
            return std::vector<std::string>{"192.0.2.220"};
        },
        [](const Url& url, const std::string& resolved_address) {
            if (url.host == "192.0.2.11") {
                EXPECT_EQ(resolved_address, url.host);
                return HttpResponse{200, R"(["stalled-learned-a.example"])"};
            }
            if (url.host == "192.0.2.12") {
                EXPECT_EQ(resolved_address, url.host);
                return HttpResponse{200, R"(["stalled-learned-b.example"])"};
            }
            if (url.host == "192.0.2.13") {
                EXPECT_EQ(resolved_address, url.host);
                return HttpResponse{200, R"(["literal-recovery.example"])"};
            }
            if (url.host == "192.0.2.14") {
                EXPECT_EQ(resolved_address, url.host);
                return HttpResponse{200, R"(["queued-learned.example"])"};
            }
            EXPECT_EQ(url.host, "healthy-seed.example");
            EXPECT_EQ(resolved_address, "192.0.2.220");
            return HttpResponse{200, R"(["dns-seed-recovery.example"])"};
        });

    AlternatorLiveNodes first_learned(
        {"192.0.2.11"}, cfg, http);
    AlternatorLiveNodes second_learned(
        {"192.0.2.12"}, cfg, http);
    AlternatorLiveNodes queued_learned(
        {"192.0.2.14"}, cfg, http);
    first_learned.UpdateLiveNodes();
    second_learned.UpdateLiveNodes();
    queued_learned.UpdateLiveNodes();

    const auto first_started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(first_learned.UpdateLiveNodes());
    EXPECT_LT(
        std::chrono::steady_clock::now() - first_started,
        std::chrono::milliseconds{300});
    EXPECT_TRUE(first_learned_gate->WaitForCalls(1));
    const auto second_started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(second_learned.UpdateLiveNodes());
    EXPECT_LT(
        std::chrono::steady_clock::now() - second_started,
        std::chrono::milliseconds{300});
    EXPECT_TRUE(second_learned_gate->WaitForCalls(1));
    EXPECT_EQ(active_resolvers.load(std::memory_order_relaxed), 2);

    EXPECT_NO_THROW(queued_learned.UpdateLiveNodes());
    EXPECT_EQ(queued_learned_gate->Calls(), 0U);

    // A later waiter for the same learned endpoint must coalesce with the
    // still-running platform lookup rather than consuming another worker.
    EXPECT_NO_THROW(first_learned.UpdateLiveNodes());
    EXPECT_EQ(first_learned_gate->Calls(), 1U);

    AlternatorLiveNodes stalled_seed(
        {"stalled-seed.example"}, cfg, http);
    EXPECT_THROW(stalled_seed.UpdateLiveNodes(), std::runtime_error);
    EXPECT_TRUE(first_seed_gate->WaitForCalls(1));
    EXPECT_EQ(active_resolvers.load(std::memory_order_relaxed), 3);

    AlternatorLiveNodes healthy_seed(
        {"healthy-seed.example"}, cfg, http);
    EXPECT_NO_THROW(healthy_seed.UpdateLiveNodes());
    EXPECT_EQ(
        Hosts(healthy_seed.GetNodes()),
        std::vector<std::string>({"dns-seed-recovery.example"}));
    EXPECT_EQ(healthy_seed_resolutions.load(std::memory_order_relaxed), 1);

    // Canonical literals bypass both resolver lanes entirely and remain a
    // recovery path even while learned and seed DNS calls are non-cooperative.
    AlternatorLiveNodes literal_seed({"192.0.2.13"}, cfg, http);
    EXPECT_NO_THROW(literal_seed.UpdateLiveNodes());
    EXPECT_EQ(
        Hosts(literal_seed.GetNodes()),
        std::vector<std::string>({"literal-recovery.example"}));

    EXPECT_EQ(maximum_active_resolvers.load(std::memory_order_relaxed), 4);
    EXPECT_LE(maximum_active_resolvers.load(std::memory_order_relaxed), 4);
    EXPECT_EQ(active_resolvers.load(std::memory_order_relaxed), 3);

    first_learned_gate->Release();
    second_learned_gate->Release();
    queued_learned_gate->Release();
    first_seed_gate->Release();
    EXPECT_TRUE(first_learned_gate->WaitForFinished(1));
    EXPECT_TRUE(second_learned_gate->WaitForFinished(1));
    EXPECT_TRUE(first_seed_gate->WaitForFinished(1));
    for (int attempt = 0;
         attempt < 100 &&
         active_resolvers.load(std::memory_order_relaxed) != 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_EQ(active_resolvers.load(std::memory_order_relaxed), 0);
    EXPECT_FALSE(queued_learned_gate->WaitForCalls(
        1,
        std::chrono::milliseconds{100}));
}

TEST(AlternatorLiveNodes, ReservedSeedTransportsRecoverFromNonCooperativeLearnedCalls) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.node_health.disabled = true;
    cfg.http_client_timeout = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds{40};
    cfg.discovery_cycle_timeout = std::chrono::milliseconds{400};

    std::vector<std::shared_ptr<ResolverGate>> learned_gates;
    for (int index = 0; index < 4; ++index) {
        learned_gates.push_back(std::make_shared<ResolverGate>());
    }
    auto first_seed_gate = std::make_shared<ResolverGate>();
    ResolverReleaseGuard release_learned_0(learned_gates[0]);
    ResolverReleaseGuard release_learned_1(learned_gates[1]);
    ResolverReleaseGuard release_learned_2(learned_gates[2]);
    ResolverReleaseGuard release_learned_3(learned_gates[3]);
    ResolverReleaseGuard release_first_seed(first_seed_gate);
    std::atomic<int> active_transports{0};
    std::atomic<int> maximum_active_transports{0};
    std::atomic<int> healthy_seed_requests{0};

    const auto update_maximum = [&](int active) {
        auto maximum = maximum_active_transports.load(std::memory_order_relaxed);
        while (maximum < active &&
               !maximum_active_transports.compare_exchange_weak(
                   maximum,
                   active,
                   std::memory_order_relaxed)) {
        }
    };
    const auto block_transport = [&](const std::shared_ptr<ResolverGate>& gate) {
        const auto active =
            active_transports.fetch_add(1, std::memory_order_relaxed) + 1;
        update_maximum(active);
        (void)gate->Resolve();
        active_transports.fetch_sub(1, std::memory_order_relaxed);
        return HttpResponse{500, ""};
    };

    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{"192.0.2.230"};
        },
        [&](const Url& url, const std::string& resolved_address) {
            for (std::size_t index = 0; index < learned_gates.size(); ++index) {
                const auto literal_host =
                    "198.51.100." + std::to_string(21U + index);
                const auto learned_host =
                    "stalled-transport-" + std::to_string(index) + ".example";
                if (url.host == literal_host) {
                    EXPECT_EQ(resolved_address, literal_host);
                    return HttpResponse{
                        200,
                        "[\"" + learned_host + "\"]"};
                }
                if (url.host == learned_host) {
                    return block_transport(learned_gates[index]);
                }
            }
            if (url.host == "stalled-transport-seed.example") {
                return block_transport(first_seed_gate);
            }
            if (url.host == "healthy-transport-seed.example") {
                const auto active =
                    active_transports.fetch_add(1, std::memory_order_relaxed) + 1;
                update_maximum(active);
                ++healthy_seed_requests;
                active_transports.fetch_sub(1, std::memory_order_relaxed);
                return HttpResponse{200, R"(["transport-recovery.example"])"};
            }
            ADD_FAILURE() << "unexpected transport endpoint " << url.host;
            return HttpResponse{500, ""};
        });

    std::vector<std::unique_ptr<AlternatorLiveNodes>> learned_nodes;
    for (int index = 0; index < 4; ++index) {
        learned_nodes.push_back(std::make_unique<AlternatorLiveNodes>(
            std::vector<std::string>{
                "198.51.100." + std::to_string(21 + index)},
            cfg,
            http));
        learned_nodes.back()->UpdateLiveNodes();
    }
    for (std::size_t index = 0; index < learned_nodes.size(); ++index) {
        EXPECT_NO_THROW(learned_nodes[index]->UpdateLiveNodes());
        EXPECT_TRUE(learned_gates[index]->WaitForCalls(1));
    }
    EXPECT_EQ(active_transports.load(std::memory_order_relaxed), 4);

    // A fifth general attempt must time out in the bounded queue and be
    // removed rather than running after the caller continues through its seed.
    EXPECT_NO_THROW(learned_nodes.front()->UpdateLiveNodes());
    EXPECT_EQ(learned_gates.front()->Calls(), 1U);

    AlternatorLiveNodes stalled_seed(
        {"stalled-transport-seed.example"}, cfg, http);
    EXPECT_THROW(stalled_seed.UpdateLiveNodes(), std::runtime_error);
    EXPECT_TRUE(first_seed_gate->WaitForCalls(1));
    EXPECT_EQ(active_transports.load(std::memory_order_relaxed), 5);

    AlternatorLiveNodes healthy_seed(
        {"healthy-transport-seed.example"}, cfg, http);
    EXPECT_NO_THROW(healthy_seed.UpdateLiveNodes());
    EXPECT_EQ(
        Hosts(healthy_seed.GetNodes()),
        std::vector<std::string>({"transport-recovery.example"}));
    EXPECT_EQ(healthy_seed_requests.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(maximum_active_transports.load(std::memory_order_relaxed), 6);
    EXPECT_LE(maximum_active_transports.load(std::memory_order_relaxed), 6);

    for (const auto& gate : learned_gates) {
        gate->Release();
    }
    first_seed_gate->Release();
    for (const auto& gate : learned_gates) {
        EXPECT_TRUE(gate->WaitForFinished(1));
    }
    EXPECT_TRUE(first_seed_gate->WaitForFinished(1));
    for (int attempt = 0;
         attempt < 100 &&
         active_transports.load(std::memory_order_relaxed) != 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    EXPECT_EQ(active_transports.load(std::memory_order_relaxed), 0);
    EXPECT_FALSE(learned_gates.front()->WaitForCalls(
        2,
        std::chrono::milliseconds{100}));
}

TEST(AlternatorLiveNodes, TimedOutQueuedResolutionIsRemovedBeforeWorkerRuns) {
    Config blocking_cfg;
    blocking_cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    blocking_cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    blocking_cfg.discovery_attempt_timeout = std::chrono::seconds{5};
    Config queued_cfg = blocking_cfg;
    queued_cfg.discovery_attempt_timeout = std::chrono::milliseconds{60};

    auto first_gate = std::make_shared<ResolverGate>();
    auto second_gate = std::make_shared<ResolverGate>();
    auto queued_gate = std::make_shared<ResolverGate>();
    auto first_http = std::make_shared<ResolvedFakeHttpClient>(
        [first_gate](const Url&) { return first_gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["first.internal"])"};
        });
    auto second_http = std::make_shared<ResolvedFakeHttpClient>(
        [second_gate](const Url&) { return second_gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["second.internal"])"};
        });
    auto queued_http = std::make_shared<ResolvedFakeHttpClient>(
        [queued_gate](const Url&) { return queued_gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["queued.internal"])"};
        });
    AlternatorLiveNodes first({"first.example"}, blocking_cfg, first_http);
    AlternatorLiveNodes second({"second.example"}, blocking_cfg, second_http);
    AlternatorLiveNodes queued({"queued.example"}, queued_cfg, queued_http);

    auto first_update = std::async(std::launch::async, [&] {
        EXPECT_NO_THROW(first.UpdateLiveNodes());
    });
    auto second_update = std::async(std::launch::async, [&] {
        EXPECT_NO_THROW(second.UpdateLiveNodes());
    });
    ResolverReleaseGuard release_first(first_gate);
    ResolverReleaseGuard release_second(second_gate);
    ResolverReleaseGuard release_queued(queued_gate);
    const bool first_started = first_gate->WaitForCalls(1);
    const bool second_started = second_gate->WaitForCalls(1);

    std::string queued_error;
    try {
        queued.UpdateLiveNodes();
    } catch (const std::exception& error) {
        queued_error = error.what();
    }
    const auto queued_calls_at_timeout = queued_gate->Calls();
    first_gate->Release();
    second_gate->Release();
    first_update.wait();
    second_update.wait();
    first_update.get();
    second_update.get();
    const bool queued_ran_later = queued_gate->WaitForCalls(
        1,
        std::chrono::milliseconds{100});

    EXPECT_TRUE(first_started);
    EXPECT_TRUE(second_started);
    EXPECT_NE(queued_error.find("DNS resolution timed out"), std::string::npos);
    EXPECT_EQ(queued_calls_at_timeout, 0U);
    EXPECT_FALSE(queued_ran_later);
}

TEST(AlternatorLiveNodes, StopCancelsStalledBackgroundResolverPromptly) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{1};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds::zero();

    auto gate = std::make_shared<ResolverGate>();
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [gate](const Url&) { return gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.Start();
    const bool resolver_started = gate->WaitForCalls(1);

    const auto started_at = std::chrono::steady_clock::now();
    nodes.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    gate->Release();

    EXPECT_TRUE(resolver_started);
    EXPECT_LT(elapsed, std::chrono::milliseconds{250});
    EXPECT_TRUE(gate->WaitForFinished(1));
}

TEST(AlternatorLiveNodes, StopCancelsRemainingResolvedAddressAttempts) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{1};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::mutex request_mutex;
    std::condition_variable request_cv;
    bool first_request_entered = false;
    bool release_requests = false;
    std::atomic<int> attempts{0};
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) {
            return std::vector<std::string>{
                "192.0.2.101",
                "192.0.2.102",
                "192.0.2.103",
                "192.0.2.104",
            };
        },
        [&](const Url&, const std::string&) -> HttpResponse {
            ++attempts;
            std::unique_lock<std::mutex> lock(request_mutex);
            first_request_entered = true;
            request_cv.notify_all();
            request_cv.wait(lock, [&] { return release_requests; });
            throw std::runtime_error("address unavailable");
        });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.Start();

    bool request_entered = false;
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        request_entered = request_cv.wait_for(
            lock,
            std::chrono::seconds{1},
            [&] { return first_request_entered; });
        if (!request_entered) {
            release_requests = true;
        }
    }
    if (!request_entered) {
        request_cv.notify_all();
        nodes.Stop();
        FAIL() << "background address attempt did not start";
        return;
    }
    auto stop = std::async(std::launch::async, [&] { nodes.Stop(); });
    const auto stop_status = stop.wait_for(std::chrono::milliseconds{250});
    {
        std::lock_guard<std::mutex> lock(request_mutex);
        release_requests = true;
    }
    request_cv.notify_all();

    ASSERT_EQ(stop_status, std::future_status::ready);
    stop.get();
    EXPECT_EQ(attempts.load(), 1);
}

TEST(AlternatorLiveNodes, DestructionCancelsResolverWithoutClientUseAfterFree) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{1};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.discovery_attempt_timeout = std::chrono::milliseconds::zero();

    auto gate = std::make_shared<ResolverGate>();
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [gate](const Url&) { return gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });
    std::weak_ptr<HttpClient> client_lifetime = http;
    auto nodes = std::make_unique<AlternatorLiveNodes>(
        std::vector<std::string>{"seed.example"},
        cfg,
        http);
    nodes->Start();
    const bool resolver_started = gate->WaitForCalls(1);

    const auto started_at = std::chrono::steady_clock::now();
    nodes.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    http.reset();
    const bool worker_kept_client_alive = !client_lifetime.expired();
    gate->Release();
    const bool resolver_finished = gate->WaitForFinished(1);
    for (int attempt = 0; attempt < 50 && !client_lifetime.expired(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    EXPECT_TRUE(resolver_started);
    EXPECT_LT(elapsed, std::chrono::milliseconds{250});
    EXPECT_TRUE(worker_kept_client_alive);
    EXPECT_TRUE(resolver_finished);
    EXPECT_TRUE(client_lifetime.expired());
}

TEST(AlternatorLiveNodes, BlockingClientDestructorDoesNotHoldResolverPoolMutex) {
    Config blocking_cfg;
    blocking_cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    blocking_cfg.idle_nodes_list_update_period = std::chrono::milliseconds{1};
    blocking_cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    blocking_cfg.http_client_timeout = std::chrono::milliseconds::zero();
    blocking_cfg.discovery_attempt_timeout = std::chrono::milliseconds::zero();

    auto resolver_gate = std::make_shared<ResolverGate>();
    auto destructor_gate = std::make_shared<DestructorGate>();
    DestructorReleaseGuard release_destructor_on_exit(destructor_gate);
    auto blocking_http = std::make_shared<BlockingDestructorHttpClient>(
        resolver_gate,
        destructor_gate);
    auto blocked_nodes = std::make_unique<AlternatorLiveNodes>(
        std::vector<std::string>{"blocked.example"},
        blocking_cfg,
        blocking_http);
    blocked_nodes->Start();
    const bool blocked_resolver_started = resolver_gate->WaitForCalls(1);
    if (!blocked_resolver_started) {
        resolver_gate->Release();
        destructor_gate->Release();
        blocked_nodes.reset();
        blocking_http.reset();
        FAIL() << "blocking resolver did not start";
        return;
    }

    blocked_nodes.reset();
    blocking_http.reset();
    resolver_gate->Release();
    const bool destructor_entered = destructor_gate->WaitForEntered();
    if (!destructor_entered) {
        destructor_gate->Release();
        FAIL() << "client destructor did not run on resolver completion";
        return;
    }

    Config foreground_cfg = blocking_cfg;
    foreground_cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    foreground_cfg.discovery_attempt_timeout = std::chrono::seconds{5};
    auto foreground_http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) { return std::vector<std::string>{"127.0.0.1"}; },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["foreground.internal"])"};
        });
    AlternatorLiveNodes foreground_nodes(
        {"foreground.example"},
        foreground_cfg,
        foreground_http);
    auto foreground_update = std::async(std::launch::async, [&] {
        try {
            foreground_nodes.UpdateLiveNodes();
            return std::string{};
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    const auto foreground_status = foreground_update.wait_for(std::chrono::seconds{1});

    auto stop_resolver_gate = std::make_shared<ResolverGate>();
    ResolverReleaseGuard release_stop_resolver_on_exit(stop_resolver_gate);
    auto stop_http = std::make_shared<ResolvedFakeHttpClient>(
        [stop_resolver_gate](const Url&) { return stop_resolver_gate->Resolve(); },
        [](const Url&, const std::string&) {
            return HttpResponse{200, R"(["unexpected.internal"])"};
        });
    AlternatorLiveNodes stopping_nodes({"stopping.example"}, blocking_cfg, stop_http);
    stopping_nodes.Start();
    const bool stop_resolver_started = stop_resolver_gate->WaitForCalls(
        1,
        std::chrono::seconds{1});
    auto stop = std::async(std::launch::async, [&] { stopping_nodes.Stop(); });
    const auto stop_status = stop.wait_for(std::chrono::seconds{1});

    destructor_gate->Release();
    stop_resolver_gate->Release();
    foreground_update.wait();
    stop.wait();
    const auto foreground_error = foreground_update.get();
    stop.get();

    EXPECT_EQ(foreground_status, std::future_status::ready);
    EXPECT_TRUE(foreground_error.empty()) << foreground_error;
    EXPECT_TRUE(stop_resolver_started);
    EXPECT_EQ(stop_status, std::future_status::ready);
    EXPECT_TRUE(destructor_gate->WaitForFinished());
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

TEST(AlternatorLiveNodes, ConcurrentFailedRecoveryIsSingleFlight) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> resolutions{0};
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [&](const Url&) -> std::vector<std::string> {
            ++resolutions;
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            throw std::runtime_error("SERVFAIL");
        },
        [](const Url&, const std::string&) -> HttpResponse {
            throw std::runtime_error("unexpected HTTP request");
        });
    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);

    constexpr int caller_count = 8;
    std::promise<void> start;
    const auto start_signal = start.get_future().share();
    std::vector<std::future<Url>> results;
    for (int caller = 0; caller < caller_count; ++caller) {
        results.push_back(std::async(std::launch::async, [&] {
            start_signal.wait();
            return nodes.NextNode();
        }));
    }
    start.set_value();
    for (auto& result : results) {
        EXPECT_TRUE(result.get().Empty());
    }

    EXPECT_EQ(resolutions.load(), 1);
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
              std::vector<std::string>({
                  "192.0.2.31", "192.0.2.32", "192.0.2.33",
                  "192.0.2.31", "192.0.2.32", "192.0.2.33",
              }));
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"learned.internal"}));
}

TEST(AlternatorLiveNodes, RejectsInvalidConfiguredSeedAuthorities) {
    Config cfg;
    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, R"(["unused.internal"])"};
    });
    const std::vector<std::string> invalid_hosts{
        "",
        "bad..name",
        "-bad.example",
        "bad-.example",
        "bad_name.example",
        "https://bad.example/path",
        "bad name.example",
        "127.0.0.999",
        "127.1",
        "2130706433",
        "0177.0.0.1",
        "0x7f.0.0.1",
        "0x7f000001",
        "127.0.0.1.",
        "1.2.3.4.5",
        u8"１２７.０.０.１",
        "b\xc3\xbc" "cher.example",
        std::string(64U, 'a') + ".example",
        std::string{"bad"} + static_cast<char>(0xc0) + static_cast<char>(0xaf) + ".example",
    };

    for (const auto& host : invalid_hosts) {
        EXPECT_THROW(
            (AlternatorLiveNodes(std::vector<std::string>{host}, cfg, http)),
            std::invalid_argument)
            << "host byte length " << host.size();
    }
}

TEST(AlternatorLiveNodes, AcceptsValidConfiguredSeedAuthorities) {
    Config cfg;
    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{200, R"(["unused.internal"])"};
    });
    const std::vector<std::string> valid_hosts{
        "localhost",
        "node-1.example",
        "node123",
        "123.example",
        "deadbeef",
        "0xg.example",
        "XN--BCHER-KVA.example",
        "127.0.0.1",
        "2001:db8::10",
    };

    for (const auto& host : valid_hosts) {
        EXPECT_NO_THROW(
            (AlternatorLiveNodes(std::vector<std::string>{host}, cfg, http)))
            << host;
    }
}

TEST(AlternatorLiveNodes, LocalNodesRejectsLegacyNumericIpAliases) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{
            200,
            R"(["127.1","2130706433","0177.0.0.1","0x7f.0.0.1",)"
            R"("0x7f000001","127.0.0.1.","1.2.3.4.5",)"
            R"("\uff11\uff12\uff17.\uff10.\uff10.\uff11",)"
            R"("127.0.0.1","2001:db8::1","123.example","deadbeef"])"};
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_NO_THROW(nodes.UpdateLiveNodes());
    EXPECT_EQ(
        Hosts(nodes.GetNodes()),
        std::vector<std::string>({
            "123.example",
            "127.0.0.1",
            "2001:db8::1",
            "deadbeef",
        }));
}

TEST(AlternatorLiveNodes, InvalidConfiguredSeedsDoNotBlockAndRemainRecoverable) {
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
    EXPECT_EQ(Hosts(nodes.GetNodes()),
              std::vector<std::string>({
                  "learned.internal",
                  "seed-a.example",
                  "seed-b.example",
                  "seed-c.example",
              }));
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

TEST(AlternatorLiveNodes, ActiveRecoveryTriesRetainedSeedBeforeDownLearnedNodes) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    int phase = 0;
    std::vector<std::string> recovery_hosts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url& url) {
            return std::vector<std::string>{url.host};
        },
        [&](const Url& url, const std::string&) -> HttpResponse {
            if (phase == 0) {
                EXPECT_EQ(url.host, "seed.example");
                return {200, R"(["old-a.internal","old-b.internal"])"};
            }
            recovery_hosts.push_back(url.host);
            if (url.host == "seed.example") {
                return {200, R"(["new.internal","old-a.internal","old-b.internal"])"};
            }
            throw std::runtime_error("learned node remains unavailable");
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    nodes.ReportNodeResult(
        Url("http", "old-a.internal", 8080),
        NodeHealthObservation::ConnectionFailure);
    nodes.ReportNodeResult(
        Url("http", "old-b.internal", 8080),
        NodeHealthObservation::ConnectionFailure);
    phase = 1;

    EXPECT_EQ(nodes.NextNode(), Url("http", "new.internal", 8080));
    EXPECT_EQ(recovery_hosts, std::vector<std::string>({"seed.example"}));
}

TEST(AlternatorLiveNodes, ActiveRecoveryProbesLearnedNodesAfterSeedFailure) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.node_health.quarantine_success_threshold = 2;

    int phase = 0;
    std::vector<std::string> recovery_hosts;
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url& url) {
            return std::vector<std::string>{url.host};
        },
        [&](const Url& url, const std::string&) -> HttpResponse {
            if (phase == 0) {
                EXPECT_EQ(url.host, "seed.example");
                return {200, R"(["old-a.internal","old-b.internal"])"};
            }
            recovery_hosts.push_back(url.host);
            if (url.host == "old-a.internal") {
                return {200, R"(["old-a.internal","old-b.internal"])"};
            }
            throw std::runtime_error("endpoint unavailable");
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    nodes.ReportNodeResult(
        Url("http", "old-a.internal", 8080),
        NodeHealthObservation::ConnectionFailure);
    nodes.ReportNodeResult(
        Url("http", "old-b.internal", 8080),
        NodeHealthObservation::ConnectionFailure);
    phase = 1;

    EXPECT_EQ(nodes.NextNode(), Url("http", "old-a.internal", 8080));
    ASSERT_FALSE(recovery_hosts.empty());
    EXPECT_EQ(recovery_hosts.front(), "seed.example");
    EXPECT_NE(
        std::find(recovery_hosts.begin(), recovery_hosts.end(), "old-a.internal"),
        recovery_hosts.end());
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
    auto http = std::make_shared<ResolvedFakeHttpClient>(
        [](const Url&) -> std::vector<std::string> {
            ADD_FAILURE() << "canonical IPv6 literal unexpectedly used the resolver";
            return {};
        },
        [&](const Url& url, const std::string& resolved_address) {
            requested_urls.push_back(url.ToString());
            EXPECT_EQ(resolved_address, url.host);
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
    EXPECT_EQ(requested_hosts.size(), 8U);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "seed-dc1.local"), 2);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "seed-dc2.local"), 2);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "dc1-node1.local"), 1);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "dc1-node2.local"), 1);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "dc2-node1.local"), 1);
    EXPECT_EQ(std::count(requested_hosts.begin(), requested_hosts.end(), "dc2-node2.local"), 1);
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

TEST(AlternatorLiveNodes, FeatureSupportProbeRetriesLearnedThenSeedCandidates) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<bool> feature_probe{false};
    std::vector<std::string> feature_hosts;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        if (!feature_probe.load()) {
            return HttpResponse{200, R"(["learned.internal"])"};
        }
        feature_hosts.push_back(url.host);
        if (url.host == "learned.internal") {
            throw std::runtime_error("learned node unavailable");
        }
        if (url.query == "rack=fakeRack") {
            return HttpResponse{200, "[]"};
        }
        return HttpResponse{200, R"(["cluster-node.internal"])"};
    });

    AlternatorLiveNodes nodes({"seed-a.example", "seed-b.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    ASSERT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"learned.internal"}));

    feature_probe.store(true);
    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());

    ASSERT_GE(feature_hosts.size(), 3U);
    EXPECT_EQ(feature_hosts.front(), "learned.internal");
    EXPECT_NE(feature_hosts[1], "learned.internal");
    EXPECT_EQ(feature_hosts[1], feature_hosts[2]);
}

TEST(AlternatorLiveNodes, FeatureSupportProbeFairlySlicesCandidateDeadline) {
    Config cfg;
    cfg.routing_scope = NewClusterScope();
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();
    cfg.discovery_cycle_timeout = std::chrono::milliseconds{400};

    std::atomic<bool> feature_probe{false};
    std::vector<std::chrono::milliseconds> learned_timeouts;
    std::vector<std::string> feature_hosts;
    auto http = std::make_shared<DeadlineAwareFakeHttpClient>(
        [](const Url& url) {
            return std::vector<std::string>{url.host};
        },
        [&](const Url& url,
            const std::string&,
            std::chrono::milliseconds timeout) -> HttpResponse {
            if (!feature_probe.load()) {
                return HttpResponse{200, R"(["learned.internal"])"};
            }
            feature_hosts.push_back(url.host);
            if (url.host == "learned.internal") {
                learned_timeouts.push_back(timeout);
                throw std::runtime_error("learned node unavailable");
            }
            if (url.query == "rack=fakeRack") {
                return HttpResponse{200, "[]"};
            }
            return HttpResponse{200, R"(["cluster-node.internal"])"};
        });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    nodes.UpdateLiveNodes();
    feature_probe.store(true);

    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());

    ASSERT_EQ(learned_timeouts.size(), 1U);
    EXPECT_GT(learned_timeouts.front(), std::chrono::milliseconds::zero());
    EXPECT_LE(learned_timeouts.front(), std::chrono::milliseconds{110});
    ASSERT_GE(feature_hosts.size(), 3U);
    EXPECT_EQ(feature_hosts.front(), "learned.internal");
    EXPECT_EQ(feature_hosts[1], "seed.example");
    EXPECT_EQ(feature_hosts[2], "seed.example");
}

TEST(AlternatorLiveNodes, FeatureSupportProbeRetainsDiscoverySeedAfterScopedEmpty) {
    Config cfg;
    cfg.routing_scope = NewDCScope("dc1");
    cfg.nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::vector<std::string> requests;
    auto http = std::make_shared<FakeHttpClient>([&](const Url& url) {
        requests.push_back(url.host + "?" + url.query);
        EXPECT_EQ(url.host, "seed.example");
        if (url.query == "dc=dc1" || url.query == "rack=fakeRack") {
            return HttpResponse{200, "[]"};
        }
        return HttpResponse{200, R"(["cluster-node.internal"])"};
    });

    AlternatorLiveNodes nodes({"seed.example"}, cfg, http);
    EXPECT_THROW(nodes.UpdateLiveNodes(), std::runtime_error);
    ASSERT_TRUE(nodes.GetNodes().empty());

    EXPECT_TRUE(nodes.CheckIfRackDatacenterFeatureIsSupported());
    EXPECT_EQ(requests,
              std::vector<std::string>({
                  "seed.example?dc=dc1",
                  "seed.example?rack=fakeRack",
                  "seed.example?",
              }));
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

TEST(AlternatorLiveNodes, ProbeDownNodesTreats4xxAsResponsive) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{0};
    cfg.node_health.quarantine_success_threshold = 2;

    auto http = std::make_shared<FakeHttpClient>([](const Url&) {
        return HttpResponse{404, ""};
    });

    AlternatorLiveNodes nodes({"recovering.local"}, cfg, http);
    const Url recovering("http", "recovering.local", 8080);
    nodes.ReportNodeResult(recovering, NodeHealthObservation::ConnectionFailure);

    const auto responsive = nodes.ProbeDownNodes();

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

TEST(AlternatorLiveNodes, ActivityStartsRefreshWhenIdlePeriodIsDisabled) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{10};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds::zero();
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds::zero();

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        ++requests;
        return HttpResponse{200, R"(["node2.local"])"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    nodes.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    EXPECT_EQ(requests.load(), 0);

    EXPECT_EQ(nodes.NextNode().host, "node1.local");
    for (int attempt = 0; attempt < 50 && requests.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    nodes.Stop();

    EXPECT_GT(requests.load(), 0);
    EXPECT_EQ(Hosts(nodes.GetNodes()), std::vector<std::string>({"node2.local"}));
}

TEST(AlternatorLiveNodes, PublicHealthReportMarksApplicationActivity) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{10};
    cfg.idle_nodes_list_update_period = std::chrono::hours{1};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::atomic<int> requests{0};
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        ++requests;
        return HttpResponse{200, "[\"node1.local\"]"};
    });
    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    nodes.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    EXPECT_EQ(requests.load(), 0);

    nodes.ReportNodeResult(
        Url("http", "node1.local", 8080),
        NodeHealthObservation::Success);
    for (int attempt = 0; attempt < 50 && requests.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    nodes.Stop();

    EXPECT_GT(requests.load(), 0);
}

TEST(AlternatorLiveNodes, BackgroundDiscoveryHealthKeepsIdleRefreshCadence) {
    Config cfg;
    cfg.nodes_list_update_period = std::chrono::milliseconds{5};
    cfg.idle_nodes_list_update_period = std::chrono::milliseconds{80};
    cfg.node_health.down_node_probe_period = std::chrono::milliseconds{0};

    std::mutex request_mutex;
    std::condition_variable request_cv;
    std::vector<std::chrono::steady_clock::time_point> request_times;
    auto http = std::make_shared<FakeHttpClient>([&](const Url&) {
        {
            std::lock_guard<std::mutex> lock(request_mutex);
            request_times.push_back(std::chrono::steady_clock::now());
        }
        request_cv.notify_all();
        return HttpResponse{200, "[\"node1.local\"]"};
    });

    AlternatorLiveNodes nodes({"node1.local"}, cfg, http);
    nodes.Start();
    bool observed_two_refreshes = false;
    {
        std::unique_lock<std::mutex> lock(request_mutex);
        observed_two_refreshes = request_cv.wait_for(
            lock,
            std::chrono::seconds{1},
            [&] { return request_times.size() >= 2; });
    }
    nodes.Stop();

    ASSERT_TRUE(observed_two_refreshes);
    ASSERT_GE(request_times.size(), 2U);
    EXPECT_GE(request_times[1] - request_times[0], std::chrono::milliseconds{55});
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
