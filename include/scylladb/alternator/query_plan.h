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

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <scylladb/alternator/detail/go_random.h>
#include <scylladb/alternator/uri.h>

namespace scylladb::alternator {

class NodesSource {
public:
    virtual ~NodesSource() = default;

    [[nodiscard]] virtual std::vector<Url> GetActiveNodes() const = 0;
    [[nodiscard]] virtual std::vector<Url> GetQueryPlanNodes() const {
        return GetActiveNodes();
    }
    [[nodiscard]] virtual std::vector<Url> GetQueryPlanNodesForHash(std::int64_t hash) const {
        (void)hash;
        return GetQueryPlanNodes();
    }
    [[nodiscard]] virtual std::vector<Url> GetDownNodes() const = 0;
};

class QueryPlan {
public:
    explicit QueryPlan(std::vector<Url> nodes);

    static QueryPlan WithSeed(std::vector<Url> nodes, std::int64_t seed);
    static QueryPlan WithSortedSeed(std::vector<Url> nodes, std::int64_t seed);
    static QueryPlan WithPreferredNode(std::vector<Url> nodes, Url preferred_node);
    static QueryPlan WithPreferredNodes(std::vector<Url> nodes, std::vector<Url> preferred_nodes);
    static QueryPlan FromOrderedNodes(std::vector<Url> nodes);
    static QueryPlan FromNodesSource(const NodesSource& source);

    [[nodiscard]] Url Next();

private:
    QueryPlan(std::vector<Url> nodes,
              std::uint64_t seed,
              bool deterministic_order,
              bool sort_nodes,
              std::vector<Url> preferred_nodes);

    [[nodiscard]] Url PickAndRemove(std::vector<Url>& nodes);
    void Prepare();

    std::vector<Url> nodes_;
    std::int64_t seed_ = 0;
    std::unique_ptr<detail::GoRandom> go_rng_;
    bool deterministic_order_ = false;
    bool sort_nodes_ = false;
    bool prepared_ = false;
    std::vector<Url> preferred_nodes_;
};

Url FirstNodeWithSeed(std::vector<Url> nodes, std::int64_t seed);
std::vector<Url> SortAndDedupeNodes(std::vector<Url> nodes);

} // namespace scylladb::alternator
