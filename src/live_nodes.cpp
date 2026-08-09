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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace scylladb::alternator {
namespace {

constexpr std::size_t kResolverWorkerCount = 2;
constexpr std::size_t kResolverQueueCapacity = 64;

class ResolutionCanceledError final : public std::runtime_error {
public:
    explicit ResolutionCanceledError(const std::string& host)
        : std::runtime_error("DNS resolution canceled for " + host) {}
};

struct ResolverKey {
    const HttpClient* client = nullptr;
    std::string endpoint;

    bool operator<(const ResolverKey& other) const {
        const auto pointer_less = std::less<const HttpClient*>{};
        if (client != other.client) {
            return pointer_less(client, other.client);
        }
        return endpoint < other.endpoint;
    }
};

enum class ResolverOperationState {
    Queued,
    Running,
    Completed,
    Abandoned,
};

struct ResolverOperation {
    ResolverKey key;
    std::shared_ptr<HttpClient> client;
    Url endpoint;
    ResolverOperationState state = ResolverOperationState::Queued;
    std::size_t waiters = 0;
    std::vector<std::string> addresses;
    std::exception_ptr error;
};

class DiscoveryResolverPool {
public:
    DiscoveryResolverPool() {
        for (std::size_t index = 0; index < kResolverWorkerCount; ++index) {
            try {
                std::thread worker([this] { WorkerLoop(); });
                worker.detach();
                ++worker_count_;
            } catch (...) {
                // A smaller fixed pool remains safe. Resolve() fails fast if
                // the platform cannot create any resolver worker.
            }
        }
    }

    [[nodiscard]] std::vector<std::string> Resolve(
        std::shared_ptr<HttpClient> client,
        const Url& endpoint,
        std::chrono::milliseconds timeout,
        const std::atomic<bool>* cancellation) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cancellation != nullptr && cancellation->load(std::memory_order_acquire)) {
            throw ResolutionCanceledError(endpoint.host);
        }
        if (worker_count_ == 0) {
            throw std::runtime_error("DNS resolver worker pool is unavailable");
        }

        ResolverKey key{client.get(), endpoint.ToString()};
        std::shared_ptr<ResolverOperation> operation;
        const auto existing = operations_.find(key);
        if (existing != operations_.end()) {
            operation = existing->second;
        } else {
            if (queue_.size() >= kResolverQueueCapacity) {
                throw std::runtime_error("DNS resolver queue is full");
            }
            operation = std::make_shared<ResolverOperation>();
            operation->key = std::move(key);
            operation->client = std::move(client);
            operation->endpoint = endpoint;
            operations_.emplace(operation->key, operation);
            try {
                queue_.push_back(operation);
            } catch (...) {
                operations_.erase(operation->key);
                throw;
            }
            work_cv_.notify_one();
        }
        ++operation->waiters;

        const auto finished = [&] {
            return operation->state == ResolverOperationState::Completed ||
                   (cancellation != nullptr &&
                    cancellation->load(std::memory_order_acquire));
        };
        bool woke_before_timeout = true;
        if (timeout > std::chrono::milliseconds::zero()) {
            woke_before_timeout = completion_cv_.wait_for(lock, timeout, finished);
        } else {
            completion_cv_.wait(lock, finished);
        }

        const bool completed = operation->state == ResolverOperationState::Completed;
        const bool canceled =
            !completed && cancellation != nullptr &&
            cancellation->load(std::memory_order_acquire);
        const bool timed_out = !completed && !canceled && !woke_before_timeout;
        --operation->waiters;
        if ((canceled || timed_out) && operation->waiters == 0 &&
            operation->state == ResolverOperationState::Queued) {
            AbandonQueuedOperation(operation);
        }

        if (canceled) {
            throw ResolutionCanceledError(endpoint.host);
        }
        if (timed_out) {
            throw std::runtime_error("DNS resolution timed out for " + endpoint.host);
        }
        if (!completed) {
            throw std::runtime_error("DNS resolution interrupted for " + endpoint.host);
        }

        auto addresses = operation->addresses;
        auto error = operation->error;
        lock.unlock();
        if (error) {
            std::rethrow_exception(error);
        }
        return addresses;
    }

    void NotifyCancellation() {
        // Synchronize with the wait transition so a cancellation notification
        // cannot be lost between the predicate check and sleeping.
        std::lock_guard<std::mutex> lock(mutex_);
        completion_cv_.notify_all();
    }

private:
    void AbandonQueuedOperation(const std::shared_ptr<ResolverOperation>& operation) {
        const auto queued = std::find(queue_.begin(), queue_.end(), operation);
        if (queued != queue_.end()) {
            queue_.erase(queued);
        }
        const auto active = operations_.find(operation->key);
        if (active != operations_.end() && active->second == operation) {
            operations_.erase(active);
        }
        operation->state = ResolverOperationState::Abandoned;
        operation->client.reset();
    }

    void WorkerLoop() {
        while (true) {
            std::shared_ptr<ResolverOperation> operation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [this] { return !queue_.empty(); });
                operation = queue_.front();
                queue_.pop_front();
                if (operation->state != ResolverOperationState::Queued) {
                    continue;
                }
                operation->state = ResolverOperationState::Running;
            }

            std::vector<std::string> addresses;
            std::exception_ptr error;
            try {
                addresses = operation->client->Resolve(operation->endpoint);
            } catch (...) {
                error = std::current_exception();
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                operation->addresses = std::move(addresses);
                operation->error = std::move(error);
                operation->state = ResolverOperationState::Completed;
                const auto active = operations_.find(operation->key);
                if (active != operations_.end() && active->second == operation) {
                    operations_.erase(active);
                }
                operation->client.reset();
            }
            completion_cv_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable completion_cv_;
    std::deque<std::shared_ptr<ResolverOperation>> queue_;
    std::map<ResolverKey, std::shared_ptr<ResolverOperation>> operations_;
    std::size_t worker_count_ = 0;
};

DiscoveryResolverPool& ResolverPool() {
    // POSIX getaddrinfo() has no portable cancellation API. A deliberately
    // process-lifetime pool keeps stuck resolver calls and their resources
    // strictly bounded without making process shutdown join those calls.
    static auto* pool = new DiscoveryResolverPool();
    return *pool;
}

std::chrono::milliseconds EffectiveDiscoveryTimeout(const Config& config) {
    if (config.http_client_timeout <= std::chrono::milliseconds::zero()) {
        return config.discovery_attempt_timeout;
    }
    if (config.discovery_attempt_timeout <= std::chrono::milliseconds::zero()) {
        return config.http_client_timeout;
    }
    return std::min(config.http_client_timeout, config.discovery_attempt_timeout);
}

class HttpStatusError final : public std::runtime_error {
public:
    HttpStatusError(const Url& endpoint, const std::string& address, long status_code)
        : std::runtime_error(
              "HTTP " + std::to_string(status_code) + " from " + endpoint.ToString() +
              " via " + address)
        , status_code(status_code) {}

    long status_code = 0;
};

class InvalidHttpResponseError final : public std::runtime_error {
public:
    InvalidHttpResponseError(
        const Url& endpoint,
        const std::string& address,
        const std::string& message)
        : std::runtime_error(
              "invalid /localnodes response from " + endpoint.ToString() +
              " via " + address + ": " + message) {}
};

std::vector<std::string> ParseJsonStringArray(const std::string& body) {
    std::vector<std::string> out;
    std::size_t pos = 0;

    auto skip_ws = [&] {
        while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
            ++pos;
        }
    };

    auto expect = [&](char ch) {
        skip_ws();
        if (pos >= body.size() || body[pos] != ch) {
            throw std::runtime_error("invalid /localnodes JSON response");
        }
        ++pos;
    };

    expect('[');
    skip_ws();
    if (pos < body.size() && body[pos] == ']') {
        ++pos;
        skip_ws();
        if (pos != body.size()) {
            throw std::runtime_error("invalid trailing data in /localnodes JSON response");
        }
        return out;
    }

    while (true) {
        expect('"');
        std::string value;
        while (pos < body.size()) {
            const char ch = body[pos++];
            if (ch == '"') {
                break;
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (pos >= body.size()) {
                throw std::runtime_error("invalid escape in /localnodes JSON response");
            }
            const char escaped = body[pos++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                throw std::runtime_error("unsupported escape in /localnodes JSON response");
            }
        }
        out.push_back(std::move(value));

        skip_ws();
        if (pos >= body.size()) {
            throw std::runtime_error("unterminated /localnodes JSON response");
        }
        if (body[pos] == ']') {
            ++pos;
            break;
        }
        expect(',');
    }

    skip_ws();
    if (pos != body.size()) {
        throw std::runtime_error("invalid trailing data in /localnodes JSON response");
    }
    return out;
}

bool IsUsableNodeHost(const std::string& node) {
    if (node.empty() || std::any_of(node.begin(), node.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0 || std::iscntrl(ch) != 0;
        })) {
        return false;
    }
    if (node.find(':') != std::string::npos) {
        in6_addr address{};
        return inet_pton(AF_INET6, node.c_str(), &address) == 1;
    }
    return node.find_first_of("/?#[]@!$&'()*+,;=%\\") == std::string::npos;
}

std::vector<Url> ToUrls(const std::vector<std::string>& nodes, const Config& config) {
    std::vector<Url> urls;
    urls.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (!IsUsableNodeHost(node)) {
            continue;
        }
        urls.emplace_back(config.scheme, node, config.port);
    }
    return SortAndDedupeNodes(std::move(urls));
}

void AppendUniqueNodes(std::vector<Url>& out, std::vector<Url> nodes) {
    for (auto& node : nodes) {
        if (!node.Empty() && std::find(out.begin(), out.end(), node) == out.end()) {
            out.push_back(std::move(node));
        }
    }
}

void AppendRandomizedPlan(std::vector<Url>& out, std::vector<Url> nodes) {
    QueryPlan plan(std::move(nodes));
    for (auto node = plan.Next(); !node.Empty(); node = plan.Next()) {
        if (std::find(out.begin(), out.end(), node) == out.end()) {
            out.push_back(std::move(node));
        }
    }
}

bool QuarantinedNodeIsFirstForHash(const Url& node,
                                   const std::vector<Url>& active_nodes,
                                   std::int64_t hash) {
    auto candidate_nodes = active_nodes;
    candidate_nodes.push_back(node);
    return FirstNodeWithSeed(std::move(candidate_nodes), hash) == node;
}

} // namespace

AlternatorLiveNodes::AlternatorLiveNodes(std::vector<std::string> initial_nodes,
                                         Config config,
                                         std::shared_ptr<HttpClient> http_client)
    : config_(std::move(config))
    , http_client_(std::move(http_client))
    , health_store_(nullptr) {
    ValidateConfig(config_);
    if (initial_nodes.empty()) {
        throw std::invalid_argument("initial_nodes cannot be empty");
    }
    if (!http_client_) {
        http_client_ = NewDefaultHttpClient(config_);
    }

    for (const auto& node : initial_nodes) {
        initial_nodes_.emplace_back(config_.scheme, node, config_.port);
    }
    initial_nodes_ = SortAndDedupeNodes(std::move(initial_nodes_));
    live_nodes_ = initial_nodes_;
    health_store_ = std::make_unique<NodeHealthStore>(config_.node_health, initial_nodes_);
    const auto now = std::chrono::steady_clock::now();
    last_activity_ = std::chrono::steady_clock::time_point::min();
    if (config_.idle_nodes_list_update_period > std::chrono::milliseconds::zero()) {
        next_update_ = now + config_.idle_nodes_list_update_period;
    } else {
        next_update_ = std::chrono::steady_clock::time_point::max();
    }
}

AlternatorLiveNodes::~AlternatorLiveNodes() {
    Stop();
}

Url AlternatorLiveNodes::NextNode() {
    MarkActivity();

    auto candidates = GetActiveNodes();
    if (candidates.empty()) {
        try {
            RecoverLiveNodesIfNeeded();
        } catch (...) {
            // Endpoint selection keeps its existing empty-result contract when
            // recovery fails. Background refresh will retry later.
        }
        candidates = GetActiveNodes();
    }

    if (ShouldTryQuarantinedNode(candidates.empty())) {
        auto quarantined = NextQuarantinedNode();
        if (!quarantined.Empty()) {
            return quarantined;
        }
    }

    if (candidates.empty()) {
        return {};
    }

    const auto idx = next_node_index_.fetch_add(1, std::memory_order_relaxed) % candidates.size();
    return candidates[idx];
}

std::vector<Url> AlternatorLiveNodes::GetNodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (live_nodes_.empty()) {
        return initial_nodes_;
    }
    return live_nodes_;
}

std::vector<Url> AlternatorLiveNodes::GetActiveNodes() const {
    return health_store_->GetActiveNodes();
}

std::vector<Url> AlternatorLiveNodes::GetQueryPlanNodes() const {
    auto candidates = GetActiveNodes();
    if (ShouldTryQuarantinedNode(candidates.empty())) {
        auto quarantined = NextQuarantinedNode();
        if (!quarantined.Empty()) {
            candidates.push_back(std::move(quarantined));
        }
    }
    return SortAndDedupeNodes(std::move(candidates));
}

std::vector<Url> AlternatorLiveNodes::GetQueryPlanNodesForHash(std::int64_t hash) const {
    std::vector<Url> candidates;
    auto active_nodes = SortAndDedupeNodes(GetActiveNodes());
    auto quarantined = StickyQuarantinedNodeForHash(hash, active_nodes);
    if (!quarantined.Empty()) {
        candidates.push_back(std::move(quarantined));
    }
    AppendUniqueNodes(candidates, std::move(active_nodes));
    return candidates;
}

std::vector<Url> AlternatorLiveNodes::GetQuarantinedNodes() const {
    return health_store_->GetQuarantinedNodes();
}

std::vector<Url> AlternatorLiveNodes::GetDownNodes() const {
    return health_store_->GetDownNodes();
}

void AlternatorLiveNodes::UpdateLiveNodes() {
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    UpdateLiveNodesLocked();
}

void AlternatorLiveNodes::RecoverLiveNodesIfNeeded() {
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    if (!GetActiveNodes().empty()) {
        return;
    }

    ProbeDownNodes();
    if (!GetActiveNodes().empty()) {
        return;
    }
    UpdateLiveNodesLocked();
}

void AlternatorLiveNodes::UpdateLiveNodesLocked(
    const std::atomic<bool>* resolution_cancellation) {
    auto new_nodes = FetchLiveNodes(resolution_cancellation);
    if (new_nodes.empty()) {
        ProbeDownNodesInternal(resolution_cancellation);
        return;
    }

    std::vector<Url> removed_nodes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& node : live_nodes_) {
            if (std::find(new_nodes.begin(), new_nodes.end(), node) == new_nodes.end()) {
                removed_nodes.push_back(node);
            }
        }
        live_nodes_ = SortAndDedupeNodes(std::move(new_nodes));
        health_store_->ReplaceNodes(live_nodes_);
    }
    for (const auto& node : removed_nodes) {
        RemoveQuarantineHashAssignmentsForNode(node);
    }

    ProbeDownNodesInternal(resolution_cancellation);
}

void AlternatorLiveNodes::Start() {
    std::lock_guard<std::mutex> lock(background_mutex_);
    if (background_started_) {
        return;
    }
    cancel_background_resolutions_.store(false, std::memory_order_release);
    stopping_ = false;
    background_started_ = true;
    background_thread_ = std::thread(&AlternatorLiveNodes::BackgroundLoop, this);
}

void AlternatorLiveNodes::Stop() {
    {
        std::lock_guard<std::mutex> lock(background_mutex_);
        if (!background_started_) {
            return;
        }
        stopping_ = true;
        cancel_background_resolutions_.store(true, std::memory_order_release);
    }
    background_cv_.notify_all();
    ResolverPool().NotifyCancellation();
    if (background_thread_.joinable()) {
        background_thread_.join();
    }
    std::lock_guard<std::mutex> lock(background_mutex_);
    background_started_ = false;
    stopping_ = false;
}

void AlternatorLiveNodes::ReportNodeResult(const Url& node, NodeHealthObservation observation) {
    MarkActivity();
    ObserveNodeResult(node, observation);
}

void AlternatorLiveNodes::ObserveNodeResult(
    const Url& node,
    NodeHealthObservation observation) {
    health_store_->ReportNodeResult(node, observation);
    auto status = health_store_->GetNodeStatus(node);
    if (!status || status->state != NodeHealthState::Quarantined) {
        RemoveQuarantineHashAssignmentsForNode(node);
    }
}

std::vector<Url> AlternatorLiveNodes::ProbeDownNodes() {
    return ProbeDownNodesInternal(nullptr);
}

std::vector<Url> AlternatorLiveNodes::ProbeDownNodesInternal(
    const std::atomic<bool>* resolution_cancellation) {
    return health_store_->ProbeDownNodes([this, resolution_cancellation](
                                            const Url& node,
                                            const NodeHealthStatus&) {
        try {
            const auto discovered = GetNodesFromEndpoint(
                node.WithPathAndQuery("/localnodes"),
                resolution_cancellation);
            return discovered.empty()
                ? NodeHealthObservation::ConnectionFailure
                : NodeHealthObservation::Success;
        } catch (const HttpStatusError& error) {
            return error.status_code >= 500
                ? NodeHealthObservation::ServerError
                : NodeHealthObservation::Success;
        } catch (const ResolutionCanceledError&) {
            throw;
        } catch (...) {
            return NodeHealthObservation::ConnectionFailure;
        }
    });
}

void AlternatorLiveNodes::CheckIfRackAndDatacenterSetCorrectly() {
    auto scope = config_.routing_scope;
    std::vector<std::string> misses;
    while (scope) {
        if (scope->IsCluster()) {
            return;
        }
        auto nodes = GetNodesForScope(*scope);
        if (!nodes.empty()) {
            return;
        }
        misses.push_back(scope->ToString());
        scope = scope->Fallback();
    }

    if (!misses.empty()) {
        std::string message = "routing scope has no nodes:";
        for (const auto& miss : misses) {
            message += " " + miss;
        }
        throw std::runtime_error(message);
    }
}

bool AlternatorLiveNodes::CheckIfRackDatacenterFeatureIsSupported() {
    const auto node = NextKnownNode();
    if (node.Empty()) {
        throw std::runtime_error("no known nodes are available");
    }

    const auto base_uri = node.WithPathAndQuery("/localnodes");
    const auto fake_rack_uri = node.WithPathAndQuery("/localnodes", "rack=fakeRack");

    auto hosts_with_fake_rack = GetNodesFromEndpoint(fake_rack_uri);
    auto hosts_without_rack = GetNodesFromEndpoint(base_uri);
    if (hosts_without_rack.empty()) {
        throw std::runtime_error("host returned empty /localnodes list");
    }
    return hosts_with_fake_rack.size() != hosts_without_rack.size();
}

const Config& AlternatorLiveNodes::GetConfig() const {
    return config_;
}

std::vector<Url> AlternatorLiveNodes::FetchLiveNodes(
    const std::atomic<bool>* resolution_cancellation) {
    auto scope = config_.routing_scope;
    while (scope) {
        auto nodes = GetNodesForScope(*scope, resolution_cancellation);
        if (!nodes.empty()) {
            return nodes;
        }
        scope = scope->Fallback();
    }
    return {};
}

std::vector<Url> AlternatorLiveNodes::GetNodesForScope(
    const RoutingScope& scope,
    const std::atomic<bool>* resolution_cancellation) {
    const bool cluster_scope = scope.IsCluster();
    auto preferred_nodes = cluster_scope ? initial_nodes_ : GetQueryPlanNodes();
    std::vector<Url> fallback_nodes;
    if (!cluster_scope) {
        for (const auto& node : initial_nodes_) {
            if (std::find(preferred_nodes.begin(), preferred_nodes.end(), node) == preferred_nodes.end()) {
                fallback_nodes.push_back(node);
            }
        }
    }
    std::vector<Url> discovery_order;
    AppendRandomizedPlan(discovery_order, std::move(preferred_nodes));
    AppendRandomizedPlan(discovery_order, std::move(fallback_nodes));
    QueryPlan plan = QueryPlan::FromOrderedNodes(std::move(discovery_order));
    std::vector<Url> discovered;
    std::exception_ptr last_error;
    bool saw_empty_response = false;

    for (Url node = plan.Next(); !node.Empty(); node = plan.Next()) {
        auto endpoint = node.WithPathAndQuery("/localnodes", scope.LocalNodesQuery());
        try {
            auto nodes = GetNodesFromEndpoint(endpoint, resolution_cancellation);
            ObserveNodeResult(node, NodeHealthObservation::Success);
            if (nodes.empty()) {
                saw_empty_response = true;
                continue;
            }
            if (!cluster_scope) {
                return nodes;
            }
            discovered.insert(discovered.end(), nodes.begin(), nodes.end());
        } catch (const ResolutionCanceledError&) {
            throw;
        } catch (const HttpStatusError& error) {
            ObserveNodeResult(
                node,
                error.status_code >= 500 ? NodeHealthObservation::ServerError : NodeHealthObservation::Success);
            last_error = std::current_exception();
        } catch (const InvalidHttpResponseError&) {
            ObserveNodeResult(node, NodeHealthObservation::Success);
            last_error = std::current_exception();
        } catch (...) {
            ObserveNodeResult(node, NodeHealthObservation::ConnectionFailure);
            last_error = std::current_exception();
        }
    }

    if (!discovered.empty()) {
        return SortAndDedupeNodes(std::move(discovered));
    }
    if (saw_empty_response && !cluster_scope) {
        return {};
    }
    if (last_error) {
        std::rethrow_exception(last_error);
    }
    if (saw_empty_response) {
        throw std::runtime_error(
            "all /localnodes responses for routing scope " + scope.ToString() +
            " were empty or unusable");
    }
    return {};
}

std::vector<Url> AlternatorLiveNodes::GetNodesFromEndpoint(
    const Url& endpoint,
    const std::atomic<bool>* resolution_cancellation) const {
    auto addresses = ResolverPool().Resolve(
        http_client_,
        endpoint,
        EffectiveDiscoveryTimeout(config_),
        resolution_cancellation);
    std::vector<std::string> unique_addresses;
    unique_addresses.reserve(addresses.size());
    for (auto& address : addresses) {
        if (!address.empty() &&
            std::find(unique_addresses.begin(), unique_addresses.end(), address) == unique_addresses.end()) {
            unique_addresses.push_back(std::move(address));
        }
    }
    if (unique_addresses.empty()) {
        throw std::runtime_error("DNS resolution returned no usable addresses for " + endpoint.host);
    }

    std::exception_ptr last_error;
    std::exception_ptr last_invalid_response_error;
    bool saw_empty_response = false;
    for (const auto& address : unique_addresses) {
        try {
            const auto resp = http_client_->GetResolved(endpoint, address);
            if (resp.status_code != 200) {
                throw HttpStatusError(endpoint, address, resp.status_code);
            }

            std::vector<std::string> parsed_nodes;
            try {
                parsed_nodes = ParseJsonStringArray(resp.body);
            } catch (const std::exception& error) {
                throw InvalidHttpResponseError(endpoint, address, error.what());
            }
            if (parsed_nodes.empty()) {
                saw_empty_response = true;
                continue;
            }
            auto nodes = ToUrls(parsed_nodes, config_);
            if (nodes.empty()) {
                throw InvalidHttpResponseError(
                    endpoint,
                    address,
                    "non-empty list contained no usable node hosts");
            }
            return nodes;
        } catch (const HttpStatusError&) {
            last_error = std::current_exception();
        } catch (const InvalidHttpResponseError&) {
            last_invalid_response_error = std::current_exception();
        } catch (const std::exception& error) {
            last_error = std::make_exception_ptr(std::runtime_error(
                "request to " + endpoint.ToString() + " via " + address +
                " failed: " + error.what()));
        } catch (...) {
            last_error = std::make_exception_ptr(std::runtime_error(
                "request to " + endpoint.ToString() + " via " + address +
                " failed with an unknown error"));
        }
    }

    if (saw_empty_response && !endpoint.query.empty()) {
        return {};
    }
    if (last_invalid_response_error) {
        std::rethrow_exception(last_invalid_response_error);
    }
    if (last_error) {
        std::rethrow_exception(last_error);
    }
    if (saw_empty_response) {
        return {};
    }
    throw std::runtime_error("all resolved addresses failed for " + endpoint.host);
}

Url AlternatorLiveNodes::NextKnownNode() {
    auto nodes = GetNodes();
    if (nodes.empty()) {
        return {};
    }
    const auto idx = next_node_index_.fetch_add(1, std::memory_order_relaxed) % nodes.size();
    return nodes[idx];
}

bool AlternatorLiveNodes::ShouldTryQuarantinedNode(bool active_nodes_empty) const {
    const auto quarantined_nodes = GetQuarantinedNodes();
    if (quarantined_nodes.empty()) {
        return false;
    }
    if (active_nodes_empty) {
        return true;
    }

    auto interval = config_.node_health.quarantine_traffic_interval;
    if (interval == 0) {
        interval = 1;
    }
    const auto attempt = quarantine_plan_index_.fetch_add(1, std::memory_order_relaxed) + 1;
    return attempt % interval == 0;
}

Url AlternatorLiveNodes::NextQuarantinedNode() const {
    auto nodes = GetQuarantinedNodes();
    if (nodes.empty()) {
        return {};
    }
    const auto idx = quarantine_node_index_.fetch_add(1, std::memory_order_relaxed) % nodes.size();
    return nodes[idx];
}

Url AlternatorLiveNodes::StickyQuarantinedNodeForHash(std::int64_t hash, const std::vector<Url>& active_nodes) const {
    auto quarantined_nodes = GetQuarantinedNodes();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = quarantine_by_hash_.find(hash);
        if (it != quarantine_by_hash_.end()) {
            if (std::find(quarantined_nodes.begin(), quarantined_nodes.end(), it->second) != quarantined_nodes.end()) {
                if (!QuarantinedNodeIsFirstForHash(it->second, active_nodes, hash)) {
                    quarantine_by_hash_.erase(it);
                    return {};
                }
                return it->second;
            }
            quarantine_by_hash_.erase(it);
        }
    }

    if (!ShouldTryQuarantinedNode(active_nodes.empty())) {
        return {};
    }

    quarantined_nodes = GetQuarantinedNodes();
    if (quarantined_nodes.empty()) {
        return {};
    }

    const auto idx = quarantine_node_index_.fetch_add(1, std::memory_order_relaxed) % quarantined_nodes.size();
    auto node = quarantined_nodes[idx];
    if (!QuarantinedNodeIsFirstForHash(node, active_nodes, hash)) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quarantine_by_hash_[hash] = node;
    }
    return node;
}

void AlternatorLiveNodes::RemoveQuarantineHashAssignmentsForNode(const Url& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = quarantine_by_hash_.begin(); it != quarantine_by_hash_.end();) {
        if (it->second == node) {
            it = quarantine_by_hash_.erase(it);
        } else {
            ++it;
        }
    }
}

void AlternatorLiveNodes::MarkActivity() {
    const auto now = std::chrono::steady_clock::now();
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_activity_ = now;
        if (config_.nodes_list_update_period > std::chrono::milliseconds::zero()) {
            const auto active_due = now + config_.nodes_list_update_period;
            if (next_update_ > active_due) {
                next_update_ = active_due;
                notify = true;
            } else if (now >= next_update_) {
                notify = true;
            }
        }
    }
    if (notify) {
        background_cv_.notify_all();
    }
}

std::chrono::milliseconds AlternatorLiveNodes::RefreshIntervalForNow(
    std::chrono::steady_clock::time_point now) const {
    const bool has_activity = last_activity_ != std::chrono::steady_clock::time_point::min();
    const bool active_enabled = config_.nodes_list_update_period > std::chrono::milliseconds::zero();
    const bool idle_enabled = config_.idle_nodes_list_update_period > std::chrono::milliseconds::zero();
    const bool recently_active =
        has_activity &&
        (!idle_enabled || now - last_activity_ < config_.idle_nodes_list_update_period);

    if (recently_active && active_enabled) {
        return config_.nodes_list_update_period;
    }
    if (idle_enabled) {
        return config_.idle_nodes_list_update_period;
    }
    return std::chrono::milliseconds::zero();
}

void AlternatorLiveNodes::ScheduleNextRefresh(std::chrono::steady_clock::time_point now) {
    const auto interval = RefreshIntervalForNow(now);
    if (interval <= std::chrono::milliseconds::zero()) {
        next_update_ = std::chrono::steady_clock::time_point::max();
        return;
    }
    next_update_ = now + interval;
}

void AlternatorLiveNodes::BackgroundLoop() {
    std::unique_lock<std::mutex> lock(background_mutex_);
    const auto probe_period = config_.node_health.down_node_probe_period;
    auto next_down_probe = std::chrono::steady_clock::now() + probe_period;

    while (!stopping_) {
        const bool probe_enabled = probe_period > std::chrono::milliseconds::zero();
        std::chrono::steady_clock::time_point next_update;
        {
            std::lock_guard<std::mutex> state_lock(mutex_);
            next_update = next_update_;
        }
        const bool refresh_enabled = next_update != std::chrono::steady_clock::time_point::max();

        if (!refresh_enabled && !probe_enabled) {
            background_cv_.wait(lock, [this] { return stopping_; });
            continue;
        }

        auto wake_at = refresh_enabled ? next_update : next_down_probe;
        if (probe_enabled && (!refresh_enabled || next_down_probe < wake_at)) {
            wake_at = next_down_probe;
        }

        const auto wait_result = background_cv_.wait_until(lock, wake_at);
        if (stopping_) {
            continue;
        }
        if (wait_result == std::cv_status::no_timeout) {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        bool should_update = false;
        {
            std::lock_guard<std::mutex> state_lock(mutex_);
            should_update = next_update_ != std::chrono::steady_clock::time_point::max() && now >= next_update_;
            if (should_update) {
                ScheduleNextRefresh(now);
            }
        }
        const bool should_probe_down = probe_enabled && now >= next_down_probe;
        if (should_probe_down) {
            next_down_probe = now + probe_period;
        }

        lock.unlock();
        try {
            if (should_update) {
                std::lock_guard<std::mutex> update_lock(update_mutex_);
                UpdateLiveNodesLocked(&cancel_background_resolutions_);
            }
            if (should_probe_down) {
                ProbeDownNodesInternal(&cancel_background_resolutions_);
            }
        } catch (...) {
            // Background refresh is best-effort.
        }
        lock.lock();
    }
}

} // namespace scylladb::alternator
