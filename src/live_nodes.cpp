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
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

namespace scylladb::alternator {
namespace {

constexpr std::size_t kResolverWorkerCount = 2;
constexpr std::size_t kSeedResolverWorkerCount = 2;
constexpr std::size_t kResolverQueueCapacity = 64;
constexpr std::size_t kTransportWorkerCount = 4;
constexpr std::size_t kSeedTransportWorkerCount = 2;
constexpr std::size_t kTransportQueueCapacity = 64;
constexpr std::size_t kMaxRoutingScopeFallbackDepth = 64;
using DiscoveryClock = std::chrono::steady_clock;
using DiscoveryDeadline = DiscoveryClock::time_point;

class ResolutionCanceledError final : public std::runtime_error {
public:
    explicit ResolutionCanceledError(const std::string& host)
        : std::runtime_error("DNS resolution canceled for " + host) {}
};

void ThrowIfResolutionCanceled(
    const std::atomic<bool>* cancellation,
    const std::string& host) {
    if (cancellation != nullptr &&
        cancellation->load(std::memory_order_acquire)) {
        throw ResolutionCanceledError(host);
    }
}

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
    explicit DiscoveryResolverPool(std::size_t maximum_workers) {
        for (std::size_t index = 0; index < maximum_workers; ++index) {
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
        std::shared_ptr<HttpClient> client_to_release;
        if ((canceled || timed_out) && operation->waiters == 0 &&
            operation->state == ResolverOperationState::Queued) {
            client_to_release = AbandonQueuedOperation(operation);
        }

        if (canceled) {
            lock.unlock();
            client_to_release.reset();
            throw ResolutionCanceledError(endpoint.host);
        }
        if (timed_out) {
            lock.unlock();
            client_to_release.reset();
            throw std::runtime_error("DNS resolution timed out for " + endpoint.host);
        }
        if (!completed) {
            lock.unlock();
            client_to_release.reset();
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
    [[nodiscard]] std::shared_ptr<HttpClient> AbandonQueuedOperation(
        const std::shared_ptr<ResolverOperation>& operation) {
        const auto queued = std::find(queue_.begin(), queue_.end(), operation);
        if (queued != queue_.end()) {
            queue_.erase(queued);
        }
        const auto active = operations_.find(operation->key);
        if (active != operations_.end() && active->second == operation) {
            operations_.erase(active);
        }
        operation->state = ResolverOperationState::Abandoned;
        return std::move(operation->client);
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

            std::shared_ptr<HttpClient> client_to_release;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                operation->addresses = std::move(addresses);
                operation->error = std::move(error);
                operation->state = ResolverOperationState::Completed;
                const auto active = operations_.find(operation->key);
                if (active != operations_.end() && active->second == operation) {
                    operations_.erase(active);
                }
                client_to_release = std::move(operation->client);
            }
            completion_cv_.notify_all();
            client_to_release.reset();
        }
    }

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable completion_cv_;
    std::deque<std::shared_ptr<ResolverOperation>> queue_;
    std::map<ResolverKey, std::shared_ptr<ResolverOperation>> operations_;
    std::size_t worker_count_ = 0;
};

DiscoveryResolverPool& ResolverPool(bool seed_candidate) {
    // POSIX getaddrinfo() has no portable cancellation API. A deliberately
    // process-lifetime pool keeps stuck resolver calls and their resources
    // strictly bounded without making process shutdown join those calls. Seed
    // candidates use an independent two-worker lane so learned-node stalls,
    // and one stalled seed, cannot consume all recovery capacity.
    if (seed_candidate) {
        static auto* seed_pool =
            new DiscoveryResolverPool(kSeedResolverWorkerCount);
        return *seed_pool;
    }
    static auto* pool = new DiscoveryResolverPool(kResolverWorkerCount);
    return *pool;
}

enum class TransportOperationState {
    Queued,
    Running,
    Completed,
    Abandoned,
};

struct TransportOperation {
    std::shared_ptr<HttpClient> client;
    Url endpoint;
    std::string resolved_address;
    std::chrono::milliseconds timeout{};
    TransportOperationState state = TransportOperationState::Queued;
    HttpResponse response;
    std::exception_ptr error;
};

class DiscoveryTransportPool {
public:
    explicit DiscoveryTransportPool(std::size_t maximum_workers) {
        for (std::size_t index = 0; index < maximum_workers; ++index) {
            try {
                std::thread worker([this] { WorkerLoop(); });
                worker.detach();
                ++worker_count_;
            } catch (...) {
                // A smaller fixed pool remains safe. Execute() fails fast if
                // the platform cannot create any transport worker.
            }
        }
    }

    [[nodiscard]] HttpResponse Execute(
        std::shared_ptr<HttpClient> client,
        const Url& endpoint,
        std::string resolved_address,
        std::chrono::milliseconds timeout,
        const std::atomic<bool>* cancellation) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cancellation != nullptr &&
            cancellation->load(std::memory_order_acquire)) {
            throw ResolutionCanceledError(endpoint.host);
        }
        if (worker_count_ == 0) {
            throw std::runtime_error("discovery transport worker pool is unavailable");
        }
        if (queue_.size() >= kTransportQueueCapacity) {
            throw std::runtime_error("discovery transport queue is full");
        }

        auto operation = std::make_shared<TransportOperation>();
        operation->client = std::move(client);
        operation->endpoint = endpoint;
        operation->resolved_address = std::move(resolved_address);
        operation->timeout = timeout;
        try {
            queue_.push_back(operation);
        } catch (...) {
            auto error = std::current_exception();
            auto client_to_release = std::move(operation->client);
            lock.unlock();
            client_to_release.reset();
            std::rethrow_exception(error);
        }
        work_cv_.notify_one();

        const auto finished = [&] {
            return operation->state == TransportOperationState::Completed ||
                   (cancellation != nullptr &&
                    cancellation->load(std::memory_order_acquire));
        };
        bool woke_before_timeout = true;
        if (timeout > std::chrono::milliseconds::zero()) {
            woke_before_timeout = completion_cv_.wait_for(lock, timeout, finished);
        } else {
            completion_cv_.wait(lock, finished);
        }

        const bool completed =
            operation->state == TransportOperationState::Completed;
        const bool canceled =
            !completed && cancellation != nullptr &&
            cancellation->load(std::memory_order_acquire);
        const bool timed_out =
            !completed && !canceled && !woke_before_timeout;
        std::shared_ptr<HttpClient> client_to_release;
        if ((canceled || timed_out) &&
            operation->state == TransportOperationState::Queued) {
            const auto queued =
                std::find(queue_.begin(), queue_.end(), operation);
            if (queued != queue_.end()) {
                queue_.erase(queued);
            }
            operation->state = TransportOperationState::Abandoned;
            client_to_release = std::move(operation->client);
        }

        if (canceled) {
            lock.unlock();
            client_to_release.reset();
            throw ResolutionCanceledError(endpoint.host);
        }
        if (timed_out) {
            lock.unlock();
            client_to_release.reset();
            throw std::runtime_error(
                "discovery request timed out for " + endpoint.host +
                " via " + operation->resolved_address);
        }
        if (!completed) {
            lock.unlock();
            client_to_release.reset();
            throw std::runtime_error(
                "discovery request interrupted for " + endpoint.host);
        }

        auto response = std::move(operation->response);
        auto error = operation->error;
        lock.unlock();
        if (error) {
            std::rethrow_exception(error);
        }
        return response;
    }

    void NotifyCancellation() {
        // Synchronize with the wait transition so a cancellation notification
        // cannot be lost between the predicate check and sleeping.
        std::lock_guard<std::mutex> lock(mutex_);
        completion_cv_.notify_all();
    }

private:
    void WorkerLoop() {
        while (true) {
            std::shared_ptr<TransportOperation> operation;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [this] { return !queue_.empty(); });
                operation = queue_.front();
                queue_.pop_front();
                if (operation->state != TransportOperationState::Queued) {
                    continue;
                }
                operation->state = TransportOperationState::Running;
            }

            HttpResponse response;
            std::exception_ptr error;
            try {
                response = operation->client->GetResolvedWithTimeout(
                    operation->endpoint,
                    operation->resolved_address,
                    operation->timeout);
            } catch (...) {
                error = std::current_exception();
            }

            std::shared_ptr<HttpClient> client_to_release;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                operation->response = std::move(response);
                operation->error = std::move(error);
                operation->state = TransportOperationState::Completed;
                client_to_release = std::move(operation->client);
            }
            completion_cv_.notify_all();
            client_to_release.reset();
        }
    }

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable completion_cv_;
    std::deque<std::shared_ptr<TransportOperation>> queue_;
    std::size_t worker_count_ = 0;
};

DiscoveryTransportPool& TransportPool(bool seed_candidate) {
    // Custom transports can ignore their timeout argument. Process-lifetime,
    // detached workers let the caller enforce its deadline without allowing
    // those non-cooperative calls or shutdown joins to grow without bound.
    if (seed_candidate) {
        static auto* seed_pool =
            new DiscoveryTransportPool(kSeedTransportWorkerCount);
        return *seed_pool;
    }
    static auto* pool = new DiscoveryTransportPool(kTransportWorkerCount);
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

DiscoveryDeadline MakeDiscoveryDeadline(std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return DiscoveryDeadline::max();
    }
    const auto now = DiscoveryClock::now();
    const auto maximum_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        DiscoveryDeadline::max() - now);
    if (timeout >= maximum_timeout) {
        return DiscoveryDeadline::max();
    }
    return now + timeout;
}

void ThrowIfDiscoveryDeadlineExpired(
    DiscoveryDeadline deadline,
    const std::string& operation) {
    if (deadline != DiscoveryDeadline::max() &&
        DiscoveryClock::now() >= deadline) {
        throw std::runtime_error(operation + " timed out");
    }
}

std::chrono::milliseconds RemainingDiscoveryTimeout(
    DiscoveryDeadline deadline,
    const std::string& operation) {
    if (deadline == DiscoveryDeadline::max()) {
        return std::chrono::milliseconds::zero();
    }
    const auto remaining = deadline - DiscoveryClock::now();
    if (remaining <= DiscoveryClock::duration::zero()) {
        throw std::runtime_error(operation + " timed out");
    }
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    if (milliseconds < remaining) {
        milliseconds += std::chrono::milliseconds{1};
    }
    return milliseconds;
}

std::chrono::milliseconds AttemptTimeoutForDeadline(
    const Config& config,
    DiscoveryDeadline deadline,
    const std::string& operation) {
    const auto configured_timeout = EffectiveDiscoveryTimeout(config);
    if (deadline == DiscoveryDeadline::max()) {
        return configured_timeout;
    }
    const auto remaining_timeout = RemainingDiscoveryTimeout(deadline, operation);
    if (configured_timeout <= std::chrono::milliseconds::zero()) {
        return remaining_timeout;
    }
    return std::min(configured_timeout, remaining_timeout);
}

DiscoveryDeadline FairDiscoveryDeadline(
    DiscoveryDeadline outer_deadline,
    std::size_t remaining_candidates,
    const std::string& operation) {
    if (outer_deadline == DiscoveryDeadline::max()) {
        return outer_deadline;
    }
    if (remaining_candidates == 0U) {
        throw std::logic_error("cannot divide a discovery deadline among zero candidates");
    }
    const auto now = DiscoveryClock::now();
    if (now >= outer_deadline) {
        throw std::runtime_error(operation + " timed out");
    }
    auto slice = (outer_deadline - now) /
        static_cast<DiscoveryClock::duration::rep>(remaining_candidates);
    if (slice <= DiscoveryClock::duration::zero()) {
        slice = DiscoveryClock::duration{1};
    }
    return std::min(outer_deadline, now + slice);
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

class EmptyRoutingScopeError final : public std::runtime_error {
public:
    EmptyRoutingScopeError(
        std::string message,
        std::vector<RoutingScopePtr> authoritative_empty_scopes)
        : std::runtime_error(std::move(message))
        , authoritative_empty_scopes_(std::move(authoritative_empty_scopes)) {}

    [[nodiscard]] bool IsAuthoritativeFor(const RoutingScope* scope) const {
        return scope != nullptr &&
            std::find_if(
                authoritative_empty_scopes_.begin(),
                authoritative_empty_scopes_.end(),
                [scope](const RoutingScopePtr& empty_scope) {
                    return empty_scope.get() == scope;
                }) != authoritative_empty_scopes_.end();
    }

private:
    std::vector<RoutingScopePtr> authoritative_empty_scopes_;
};

class AggregateSnapshotSizeError final : public std::runtime_error {
public:
    AggregateSnapshotSizeError()
        : std::runtime_error(
              "aggregate cluster node snapshot exceeds "
              "max_discovery_response_bytes") {}
};

bool IsValidUtf8(std::string_view text);

std::vector<std::string> ParseJsonStringArray(const std::string& body) {
    std::vector<std::string> out;
    std::size_t pos = 0;

    auto skip_ws = [&] {
        while (pos < body.size() &&
               (body[pos] == ' ' || body[pos] == '\t' ||
                body[pos] == '\n' || body[pos] == '\r')) {
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
        bool terminated = false;
        while (pos < body.size()) {
            const char ch = body[pos++];
            if (ch == '"') {
                terminated = true;
                break;
            }
            if (ch != '\\') {
                if (static_cast<unsigned char>(ch) < 0x20U) {
                    throw std::runtime_error(
                        "unescaped control character in /localnodes JSON response");
                }
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
            case 'u': {
                const auto parse_hex_quad = [&]() -> std::uint32_t {
                    if (body.size() - pos < 4U) {
                        throw std::runtime_error(
                            "invalid unicode escape in /localnodes JSON response");
                    }
                    std::uint32_t code_unit = 0;
                    for (int digit_index = 0; digit_index < 4; ++digit_index) {
                        const unsigned char digit =
                            static_cast<unsigned char>(body[pos++]);
                        code_unit <<= 4U;
                        if (digit >= '0' && digit <= '9') {
                            code_unit += digit - '0';
                        } else if (digit >= 'a' && digit <= 'f') {
                            code_unit += digit - 'a' + 10U;
                        } else if (digit >= 'A' && digit <= 'F') {
                            code_unit += digit - 'A' + 10U;
                        } else {
                            throw std::runtime_error(
                                "invalid unicode escape in /localnodes JSON response");
                        }
                    }
                    return code_unit;
                };
                auto code_point = parse_hex_quad();
                if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                    if (body.size() - pos < 2U || body[pos] != '\\' ||
                        body[pos + 1] != 'u') {
                        throw std::runtime_error(
                            "unpaired unicode surrogate in /localnodes JSON response");
                    }
                    pos += 2U;
                    const auto low_surrogate = parse_hex_quad();
                    if (low_surrogate < 0xdc00U || low_surrogate > 0xdfffU) {
                        throw std::runtime_error(
                            "unpaired unicode surrogate in /localnodes JSON response");
                    }
                    code_point = 0x10000U +
                        ((code_point - 0xd800U) << 10U) +
                        (low_surrogate - 0xdc00U);
                } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
                    throw std::runtime_error(
                        "unpaired unicode surrogate in /localnodes JSON response");
                }

                if (code_point <= 0x7fU) {
                    value.push_back(static_cast<char>(code_point));
                } else if (code_point <= 0x7ffU) {
                    value.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
                    value.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
                } else if (code_point <= 0xffffU) {
                    value.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
                    value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
                    value.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
                } else {
                    value.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
                    value.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
                    value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
                    value.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
                }
                break;
            }
            default:
                throw std::runtime_error("unsupported escape in /localnodes JSON response");
            }
        }
        if (!terminated) {
            throw std::runtime_error("unterminated string in /localnodes JSON response");
        }
        if (!IsValidUtf8(value)) {
            throw std::runtime_error("invalid UTF-8 in /localnodes JSON response");
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

bool IsValidUtf8(std::string_view text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto first = static_cast<unsigned char>(text[pos++]);
        if (first <= 0x7fU) {
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum_code_point = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1;
            code_point = first & 0x1fU;
            minimum_code_point = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2;
            code_point = first & 0x0fU;
            minimum_code_point = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3;
            code_point = first & 0x07U;
            minimum_code_point = 0x10000U;
        } else {
            return false;
        }
        if (text.size() - pos < continuation_count) {
            return false;
        }
        for (std::size_t index = 0; index < continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(text[pos++]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        if (code_point < minimum_code_point || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

bool IsAsciiAlphanumeric(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

bool IsCanonicalIpLiteral(const std::string& host) {
    if (host.find(':') != std::string::npos) {
        in6_addr address{};
        return inet_pton(AF_INET6, host.c_str(), &address) == 1;
    }
    in_addr address{};
    return inet_pton(AF_INET, host.c_str(), &address) == 1;
}

bool LooksLikeLegacyIpv4Literal(std::string_view hostname) {
    std::size_t label_start = 0;
    while (label_start < hostname.size()) {
        const auto label_end = hostname.find('.', label_start);
        const auto end = label_end == std::string_view::npos
            ? hostname.size()
            : label_end;
        if (end == label_start) {
            return false;
        }
        const auto component = hostname.substr(label_start, end - label_start);
        const bool prefixed_hexadecimal =
            component.size() > 2U && component[0] == '0' &&
            (component[1] == 'x' || component[1] == 'X');
        const auto digit_start = prefixed_hexadecimal ? 2U : 0U;
        for (std::size_t index = digit_start;
             index < component.size();
             ++index) {
            const auto ch = static_cast<unsigned char>(component[index]);
            const bool decimal = ch >= '0' && ch <= '9';
            const bool hexadecimal = prefixed_hexadecimal &&
                ((ch >= 'a' && ch <= 'f') ||
                 (ch >= 'A' && ch <= 'F'));
            if (!decimal && !hexadecimal) {
                return false;
            }
        }
        if (label_end == std::string_view::npos) {
            return true;
        }
        label_start = label_end + 1U;
    }
    return false;
}

bool IsUsableNodeHost(const std::string& node) {
    if (node.empty() || !IsValidUtf8(node)) {
        return false;
    }
    if (node.find(':') != std::string::npos) {
        return IsCanonicalIpLiteral(node);
    }
    if (IsCanonicalIpLiteral(node)) {
        return true;
    }

    std::string_view hostname = node;
    if (hostname.back() == '.') {
        hostname.remove_suffix(1);
    }
    if (hostname.empty() || hostname.size() > 253U ||
        LooksLikeLegacyIpv4Literal(hostname)) {
        return false;
    }

    std::size_t label_start = 0;
    while (label_start < hostname.size()) {
        const auto label_end = hostname.find('.', label_start);
        const auto end = label_end == std::string_view::npos
            ? hostname.size()
            : label_end;
        const auto label_size = end - label_start;
        if (label_size == 0U || label_size > 63U ||
            !IsAsciiAlphanumeric(static_cast<unsigned char>(hostname[label_start])) ||
            !IsAsciiAlphanumeric(static_cast<unsigned char>(hostname[end - 1U]))) {
            return false;
        }
        for (std::size_t index = label_start; index < end; ++index) {
            const auto ch = static_cast<unsigned char>(hostname[index]);
            if (!IsAsciiAlphanumeric(ch) && ch != '-') {
                return false;
            }
        }
        if (label_end == std::string_view::npos) {
            return true;
        }
        label_start = label_end + 1U;
    }
    return false;
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

bool AppendNodeWithinSnapshotLimit(
    std::vector<Url>& out,
    std::unordered_set<std::string>& hosts,
    Url node,
    std::size_t maximum_size,
    std::size_t& current_size) {
    if (node.Empty() || hosts.find(node.host) != hosts.end()) {
        return true;
    }
    const auto contribution = node.host.size() + 2U +
        (current_size == 2U ? 0U : 1U);
    if (current_size > maximum_size ||
        contribution > maximum_size - current_size) {
        return false;
    }
    current_size += contribution;
    hosts.insert(node.host);
    out.push_back(std::move(node));
    return true;
}

bool AppendNodesWithinSnapshotLimit(
    std::vector<Url>& out,
    std::unordered_set<std::string>& hosts,
    std::vector<Url> nodes,
    std::size_t maximum_size,
    std::size_t& current_size) {
    bool complete = true;
    for (auto& node : nodes) {
        if (!AppendNodeWithinSnapshotLimit(
                out,
                hosts,
                std::move(node),
                maximum_size,
                current_size)) {
            complete = false;
        }
    }
    return complete;
}

bool AppendRandomizedPlanWithinSnapshotLimit(
    std::vector<Url>& out,
    std::unordered_set<std::string>& hosts,
    std::vector<Url> nodes,
    std::size_t maximum_size,
    std::size_t& current_size) {
    bool complete = true;
    QueryPlan plan(std::move(nodes));
    for (auto node = plan.Next(); !node.Empty(); node = plan.Next()) {
        if (!AppendNodeWithinSnapshotLimit(
                out,
                hosts,
                std::move(node),
                maximum_size,
                current_size)) {
            complete = false;
        }
    }
    return complete;
}

void AppendRandomizedPlan(std::vector<Url>& out, std::vector<Url> nodes) {
    QueryPlan plan(std::move(nodes));
    for (auto node = plan.Next(); !node.Empty(); node = plan.Next()) {
        if (std::find(out.begin(), out.end(), node) == out.end()) {
            out.push_back(std::move(node));
        }
    }
}

std::vector<RoutingScopePtr> CollectRoutingScopes(RoutingScopePtr scope) {
    std::vector<RoutingScopePtr> scopes;
    std::unordered_set<const RoutingScope*> visited_scopes;
    while (scope) {
        if (scopes.size() >= kMaxRoutingScopeFallbackDepth) {
            throw std::runtime_error("routing scope fallback chain exceeds limit");
        }
        if (!visited_scopes.insert(scope.get()).second) {
            throw std::runtime_error(
                "routing scope fallback cycle detected at " + scope->ToString());
        }
        scopes.push_back(scope);
        scope = scope->Fallback();
    }
    return scopes;
}

RoutingScopePtr TerminalClusterInValidFallbackChain(
    const RoutingScopePtr& scope) {
    try {
        const auto scopes = CollectRoutingScopes(scope);
        if (!scopes.empty() && scopes.back()->IsCluster()) {
            return scopes.back();
        }
    } catch (...) {
        // Invalid fallback chains remain discovery-only. UpdateLiveNodes()
        // reports the exact cycle/depth error without exposing their seeds to
        // application routing in the meantime.
    }
    return nullptr;
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
        if (!IsUsableNodeHost(node)) {
            throw std::invalid_argument("invalid initial node host");
        }
        initial_nodes_.emplace_back(config_.scheme, node, config_.port);
    }
    initial_nodes_ = SortAndDedupeNodes(std::move(initial_nodes_));
    // A terminal Cluster fallback explicitly authorizes cluster-wide routing,
    // so its entrypoints are valid application endpoints before the first
    // refresh. Strict-only and invalid fallback chains remain discovery-only
    // until /localnodes proves that returned nodes match a requested scope.
    // Routing strict-only entrypoints eagerly would silently escape the
    // configured datacenter or rack.
    auto initial_cluster_scope =
        TerminalClusterInValidFallbackChain(config_.routing_scope);
    auto initial_routing_nodes = initial_cluster_scope
        ? initial_nodes_
        : std::vector<Url>{};
    live_nodes_ = initial_routing_nodes;
    published_scope_ = std::move(initial_cluster_scope);
    health_store_ = std::make_unique<NodeHealthStore>(
        config_.node_health,
        std::move(initial_routing_nodes));
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
    const auto observed_recovery_generation =
        recovery_generation_.load(std::memory_order_acquire);
    MarkActivity();

    auto candidates = GetActiveNodes();
    if (candidates.empty()) {
        try {
            RecoverLiveNodesIfNeeded(observed_recovery_generation);
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

void AlternatorLiveNodes::RecoverLiveNodesIfNeeded(
    std::uint64_t observed_recovery_generation) {
    std::lock_guard<std::mutex> update_lock(update_mutex_);
    if (!GetActiveNodes().empty()) {
        return;
    }
    if (recovery_generation_.load(std::memory_order_acquire) !=
        observed_recovery_generation) {
        return;
    }

    std::exception_ptr discovery_error;
    try {
        // Retained seeds are the primary recovery path. Re-probing every
        // known-down node first can delay a healthy seed by one full timeout
        // per stale learned node.
        UpdateLiveNodesLocked(nullptr, false);
    } catch (const ResolutionCanceledError&) {
        recovery_generation_.fetch_add(1, std::memory_order_acq_rel);
        throw;
    } catch (...) {
        discovery_error = std::current_exception();
    }

    if (GetActiveNodes().empty() &&
        GetQuarantinedNodes().empty()) {
        ProbeDownNodes();
    }
    recovery_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (discovery_error && GetActiveNodes().empty() &&
        GetQuarantinedNodes().empty()) {
        std::rethrow_exception(discovery_error);
    }
}

void AlternatorLiveNodes::UpdateLiveNodesLocked(
    const std::atomic<bool>* resolution_cancellation,
    bool probe_down_nodes) {
    LiveNodesDiscovery discovery;
    const auto discovery_deadline =
        MakeDiscoveryDeadline(config_.discovery_cycle_timeout);
    try {
        discovery = FetchLiveNodes(
            resolution_cancellation,
            discovery_deadline);
    } catch (const EmptyRoutingScopeError& error) {
        bool clear_published_nodes = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clear_published_nodes =
                error.IsAuthoritativeFor(published_scope_.get());
        }
        if (clear_published_nodes) {
            // An authoritative empty strict scope only invalidates a snapshot
            // that exact scope published. A failed fallback must not erase a
            // fallback- or Cluster-origin last-known-good snapshot.
            PublishLiveNodes({}, nullptr, resolution_cancellation);
        }
        throw;
    }
    ThrowIfResolutionCanceled(
        resolution_cancellation,
        initial_nodes_.empty() ? std::string{} : initial_nodes_.front().host);
    if (discovery.nodes.empty()) {
        if (probe_down_nodes) {
            ProbeDownNodesInternal(resolution_cancellation);
        }
        return;
    }

    ThrowIfDiscoveryDeadlineExpired(discovery_deadline, "discovery cycle");
    PublishLiveNodes(
        std::move(discovery.nodes),
        std::move(discovery.scope),
        resolution_cancellation,
        discovery_deadline);

    if (probe_down_nodes) {
        ProbeDownNodesInternal(resolution_cancellation);
    }
}

void AlternatorLiveNodes::PublishLiveNodes(
    std::vector<Url> nodes,
    RoutingScopePtr published_scope,
    const std::atomic<bool>* resolution_cancellation,
    DiscoveryDeadline discovery_deadline) {
    nodes = SortAndDedupeNodes(std::move(nodes));
    std::unique_lock<std::mutex> background_lock;
    if (resolution_cancellation != nullptr) {
        // Serialize the cancellation check and publication with Stop(). If
        // Stop wins the lock, no stale background result is published; if
        // publication wins, it completes before Stop marks cancellation.
        background_lock = std::unique_lock<std::mutex>(background_mutex_);
        ThrowIfResolutionCanceled(
            resolution_cancellation,
            initial_nodes_.empty() ? std::string{} : initial_nodes_.front().host);
    }
    ThrowIfDiscoveryDeadlineExpired(discovery_deadline, "discovery publication");
    std::vector<Url> removed_nodes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ThrowIfDiscoveryDeadlineExpired(discovery_deadline, "discovery publication");
        for (const auto& node : live_nodes_) {
            if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
                removed_nodes.push_back(node);
            }
        }
        live_nodes_ = std::move(nodes);
        published_scope_ = std::move(published_scope);
        health_store_->ReplaceNodes(live_nodes_);
    }
    for (const auto& node : removed_nodes) {
        RemoveQuarantineHashAssignmentsForNode(node);
    }
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
    ResolverPool(false).NotifyCancellation();
    ResolverPool(true).NotifyCancellation();
    TransportPool(false).NotifyCancellation();
    TransportPool(true).NotifyCancellation();
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
            const bool seed_candidate =
                std::find(initial_nodes_.begin(), initial_nodes_.end(), node) !=
                initial_nodes_.end();
            const auto discovered = GetNodesFromEndpoint(
                node.WithPathAndQuery("/localnodes"),
                seed_candidate,
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
    const auto scopes = CollectRoutingScopes(config_.routing_scope);
    const auto discovery_deadline =
        MakeDiscoveryDeadline(config_.discovery_cycle_timeout);
    std::vector<std::string> misses;
    for (std::size_t scope_index = 0;
         scope_index < scopes.size();
         ++scope_index) {
        const auto& scope = scopes[scope_index];
        if (scope->IsCluster()) {
            return;
        }
        auto nodes = GetNodesForScope(
            *scope,
            nullptr,
            FairDiscoveryDeadline(
                discovery_deadline,
                scopes.size() - scope_index,
                "routing scope validation"));
        if (!nodes.empty()) {
            return;
        }
        misses.push_back(scope->ToString());
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
    std::vector<Url> candidates;
    std::unordered_set<std::string> candidate_hosts;
    std::size_t learned_candidate_size = 2U;
    AppendRandomizedPlanWithinSnapshotLimit(
        candidates,
        candidate_hosts,
        GetNodes(),
        config_.max_discovery_response_bytes,
        learned_candidate_size);
    std::size_t seed_candidate_size = 2U;
    AppendRandomizedPlanWithinSnapshotLimit(
        candidates,
        candidate_hosts,
        initial_nodes_,
        config_.max_discovery_response_bytes,
        seed_candidate_size);
    if (candidates.empty()) {
        throw std::runtime_error("no known nodes are available");
    }

    const auto discovery_deadline =
        MakeDiscoveryDeadline(config_.discovery_cycle_timeout);
    std::exception_ptr last_error;
    for (std::size_t candidate_index = 0;
         candidate_index < candidates.size();
         ++candidate_index) {
        const auto& node = candidates[candidate_index];
        const auto candidate_deadline = FairDiscoveryDeadline(
            discovery_deadline,
            candidates.size() - candidate_index,
            "feature discovery");
        const bool seed_candidate =
            std::find(initial_nodes_.begin(), initial_nodes_.end(), node) !=
            initial_nodes_.end();
        try {
            auto hosts_with_fake_rack = GetNodesFromEndpoint(
                node.WithPathAndQuery("/localnodes", "rack=fakeRack"),
                seed_candidate,
                nullptr,
                FairDiscoveryDeadline(
                    candidate_deadline,
                    2U,
                    "feature discovery"));
            auto hosts_without_rack = GetNodesFromEndpoint(
                node.WithPathAndQuery("/localnodes"),
                seed_candidate,
                nullptr,
                candidate_deadline);
            if (hosts_without_rack.empty()) {
                throw std::runtime_error("host returned empty /localnodes list");
            }
            return hosts_with_fake_rack.size() != hosts_without_rack.size();
        } catch (const ResolutionCanceledError&) {
            throw;
        } catch (...) {
            last_error = std::current_exception();
        }
    }
    if (last_error) {
        std::rethrow_exception(last_error);
    }
    throw std::runtime_error("no known nodes are available");
}

const Config& AlternatorLiveNodes::GetConfig() const {
    return config_;
}

AlternatorLiveNodes::LiveNodesDiscovery AlternatorLiveNodes::FetchLiveNodes(
    const std::atomic<bool>* resolution_cancellation,
    DiscoveryDeadline discovery_deadline) {
    const auto scopes = CollectRoutingScopes(config_.routing_scope);
    std::vector<std::string> empty_scopes;
    std::vector<RoutingScopePtr> authoritative_empty_scopes;
    for (std::size_t scope_index = 0;
         scope_index < scopes.size();
         ++scope_index) {
        const auto& scope = scopes[scope_index];
        std::vector<Url> nodes;
        try {
            nodes = GetNodesForScope(
                *scope,
                resolution_cancellation,
                FairDiscoveryDeadline(
                    discovery_deadline,
                    scopes.size() - scope_index,
                    "discovery cycle"));
        } catch (const ResolutionCanceledError&) {
            throw;
        } catch (const std::exception& error) {
            if (empty_scopes.empty()) {
                throw;
            }
            std::string message =
                "routing scope fallback failed after empty scopes:";
            for (const auto& empty_scope : empty_scopes) {
                message += " " + empty_scope;
            }
            message += "; " + scope->ToString() + " failed: " + error.what();
            throw EmptyRoutingScopeError(
                std::move(message),
                authoritative_empty_scopes);
        } catch (...) {
            if (empty_scopes.empty()) {
                throw;
            }
            std::string message =
                "routing scope fallback failed after empty scopes:";
            for (const auto& empty_scope : empty_scopes) {
                message += " " + empty_scope;
            }
            message += "; " + scope->ToString() + " failed";
            throw EmptyRoutingScopeError(
                std::move(message),
                authoritative_empty_scopes);
        }
        if (!nodes.empty()) {
            ThrowIfDiscoveryDeadlineExpired(
                discovery_deadline,
                "discovery cycle");
            return {
                std::move(nodes),
                scope,
            };
        }
        empty_scopes.push_back(scope->ToString());
        authoritative_empty_scopes.push_back(scope);
    }

    std::string message = "routing scope has no usable nodes:";
    for (const auto& empty_scope : empty_scopes) {
        message += " " + empty_scope;
    }
    throw EmptyRoutingScopeError(
        std::move(message),
        std::move(authoritative_empty_scopes));
}

std::vector<Url> AlternatorLiveNodes::GetNodesForScope(
    const RoutingScope& scope,
    const std::atomic<bool>* resolution_cancellation,
    DiscoveryDeadline discovery_deadline) {
    const bool cluster_scope = scope.IsCluster();
    std::vector<Url> discovery_order;
    bool complete = true;
    if (cluster_scope) {
        std::unordered_set<std::string> candidate_hosts;
        std::size_t learned_candidate_size = 2U;
        complete = AppendRandomizedPlanWithinSnapshotLimit(
            discovery_order,
            candidate_hosts,
            GetNodes(),
            config_.max_discovery_response_bytes,
            learned_candidate_size);
        std::size_t seed_candidate_size = 2U;
        complete = AppendRandomizedPlanWithinSnapshotLimit(
            discovery_order,
            candidate_hosts,
            initial_nodes_,
            config_.max_discovery_response_bytes,
            seed_candidate_size) && complete;
    } else {
        auto preferred_nodes = GetQueryPlanNodes();
        std::vector<Url> fallback_nodes;
        for (const auto& node : initial_nodes_) {
            if (std::find(preferred_nodes.begin(), preferred_nodes.end(), node) == preferred_nodes.end()) {
                fallback_nodes.push_back(node);
            }
        }
        AppendRandomizedPlan(discovery_order, std::move(preferred_nodes));
        AppendRandomizedPlan(discovery_order, std::move(fallback_nodes));
    }
    std::vector<Url> discovered;
    std::unordered_set<std::string> discovered_hosts;
    std::size_t aggregate_response_size = 2U;
    std::exception_ptr last_error;
    bool saw_empty_response = false;

    if (cluster_scope && discovery_order.empty()) {
        throw std::runtime_error("no bounded Cluster discovery candidates are available");
    }

    for (std::size_t node_index = 0;
         node_index < discovery_order.size();
         ++node_index) {
        auto node = discovery_order[node_index];
        auto endpoint = node.WithPathAndQuery("/localnodes", scope.LocalNodesQuery());
        const auto endpoint_deadline = FairDiscoveryDeadline(
            discovery_deadline,
            discovery_order.size() - node_index,
            "discovery cycle");
        const bool seed_candidate =
            std::find(initial_nodes_.begin(), initial_nodes_.end(), node) !=
            initial_nodes_.end();
        try {
            auto nodes = GetNodesFromEndpoint(
                endpoint,
                seed_candidate,
                resolution_cancellation,
                endpoint_deadline);
            ObserveNodeResult(node, NodeHealthObservation::Success);
            if (nodes.empty()) {
                saw_empty_response = true;
                if (cluster_scope) {
                    complete = false;
                }
                continue;
            }
            if (!cluster_scope) {
                ThrowIfDiscoveryDeadlineExpired(
                    endpoint_deadline,
                    "endpoint discovery");
                return nodes;
            }
            std::vector<Url> additions;
            std::unordered_set<std::string> addition_hosts;
            auto next_aggregate_response_size = aggregate_response_size;
            for (const auto& discovered_node : nodes) {
                ThrowIfDiscoveryDeadlineExpired(
                    endpoint_deadline,
                    "cluster snapshot aggregation");
                if (discovered_hosts.find(discovered_node.host) != discovered_hosts.end() ||
                    !addition_hosts.insert(discovered_node.host).second) {
                    continue;
                }
                const auto contribution = discovered_node.host.size() + 2U +
                    (discovered.empty() && additions.empty() ? 0U : 1U);
                if (next_aggregate_response_size > config_.max_discovery_response_bytes ||
                    contribution >
                        config_.max_discovery_response_bytes - next_aggregate_response_size) {
                    throw AggregateSnapshotSizeError();
                }
                next_aggregate_response_size += contribution;
                additions.push_back(discovered_node);
            }
            ThrowIfDiscoveryDeadlineExpired(
                endpoint_deadline,
                "cluster snapshot aggregation");
            for (auto& addition : additions) {
                discovered_hosts.insert(addition.host);
                discovered.push_back(std::move(addition));
            }
            aggregate_response_size = next_aggregate_response_size;
        } catch (const ResolutionCanceledError&) {
            throw;
        } catch (const AggregateSnapshotSizeError&) {
            throw;
        } catch (const HttpStatusError& error) {
            complete = false;
            ObserveNodeResult(
                node,
                error.status_code >= 500 ? NodeHealthObservation::ServerError : NodeHealthObservation::Success);
            last_error = std::current_exception();
        } catch (const InvalidHttpResponseError&) {
            complete = false;
            ObserveNodeResult(node, NodeHealthObservation::Success);
            last_error = std::current_exception();
        } catch (...) {
            complete = false;
            ObserveNodeResult(node, NodeHealthObservation::ConnectionFailure);
            last_error = std::current_exception();
        }
    }

    if (!discovered.empty()) {
        ThrowIfDiscoveryDeadlineExpired(
            discovery_deadline,
            "cluster snapshot aggregation");
        if (!complete) {
            auto retained_nodes = GetNodes();
            AppendNodesWithinSnapshotLimit(
                discovered,
                discovered_hosts,
                std::move(retained_nodes),
                config_.max_discovery_response_bytes,
                aggregate_response_size);
        }
        auto result = SortAndDedupeNodes(std::move(discovered));
        ThrowIfDiscoveryDeadlineExpired(
            discovery_deadline,
            "cluster snapshot aggregation");
        return result;
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
    bool seed_candidate,
    const std::atomic<bool>* resolution_cancellation,
    DiscoveryDeadline discovery_deadline) const {
    auto addresses = IsCanonicalIpLiteral(endpoint.host)
        ? std::vector<std::string>{endpoint.host}
        : ResolverPool(seed_candidate).Resolve(
              http_client_,
              endpoint,
              AttemptTimeoutForDeadline(
                  config_,
                  discovery_deadline,
                  "DNS resolution"),
              resolution_cancellation);
    ThrowIfResolutionCanceled(resolution_cancellation, endpoint.host);
    ThrowIfDiscoveryDeadlineExpired(discovery_deadline, "DNS resolution");
    std::vector<std::string> unique_addresses;
    unique_addresses.reserve(addresses.size());
    std::unordered_set<std::string> seen_addresses;
    seen_addresses.reserve(addresses.size());
    for (auto& address : addresses) {
        ThrowIfDiscoveryDeadlineExpired(discovery_deadline, "DNS address validation");
        if (!address.empty() && seen_addresses.insert(address).second) {
            unique_addresses.push_back(std::move(address));
        }
    }
    if (unique_addresses.empty()) {
        throw std::runtime_error("DNS resolution returned no usable addresses for " + endpoint.host);
    }

    std::exception_ptr last_error;
    std::exception_ptr last_invalid_response_error;
    bool saw_empty_response = false;
    for (std::size_t address_index = 0;
         address_index < unique_addresses.size();
         ++address_index) {
        const auto& address = unique_addresses[address_index];
        ThrowIfResolutionCanceled(resolution_cancellation, endpoint.host);
        const auto address_deadline = FairDiscoveryDeadline(
            discovery_deadline,
            unique_addresses.size() - address_index,
            "resolved-address discovery");
        try {
            const auto resp = TransportPool(seed_candidate).Execute(
                http_client_,
                endpoint,
                address,
                AttemptTimeoutForDeadline(
                    config_,
                    address_deadline,
                    "resolved-address discovery"),
                resolution_cancellation);
            ThrowIfResolutionCanceled(resolution_cancellation, endpoint.host);
            ThrowIfDiscoveryDeadlineExpired(
                address_deadline,
                "resolved-address discovery");
            if (resp.status_code != 200) {
                throw HttpStatusError(endpoint, address, resp.status_code);
            }
            if (resp.body.size() > config_.max_discovery_response_bytes) {
                throw InvalidHttpResponseError(
                    endpoint,
                    address,
                    "response body exceeds max_discovery_response_bytes");
            }

            std::vector<std::string> parsed_nodes;
            try {
                parsed_nodes = ParseJsonStringArray(resp.body);
            } catch (const std::exception& error) {
                throw InvalidHttpResponseError(endpoint, address, error.what());
            }
            ThrowIfDiscoveryDeadlineExpired(
                address_deadline,
                "discovery response validation");
            if (parsed_nodes.empty()) {
                saw_empty_response = true;
                continue;
            }
            auto nodes = ToUrls(parsed_nodes, config_);
            ThrowIfDiscoveryDeadlineExpired(
                address_deadline,
                "discovery response validation");
            if (nodes.empty()) {
                throw InvalidHttpResponseError(
                    endpoint,
                    address,
                    "non-empty list contained no usable node hosts");
            }
            return nodes;
        } catch (const ResolutionCanceledError&) {
            throw;
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

    ThrowIfResolutionCanceled(resolution_cancellation, endpoint.host);
    ThrowIfDiscoveryDeadlineExpired(discovery_deadline, "endpoint discovery");

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
            background_cv_.wait(lock, [this] {
                if (stopping_) {
                    return true;
                }
                std::lock_guard<std::mutex> state_lock(mutex_);
                return next_update_ != std::chrono::steady_clock::time_point::max();
            });
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
