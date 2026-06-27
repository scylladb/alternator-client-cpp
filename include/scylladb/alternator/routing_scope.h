#pragma once

#include <memory>
#include <string>

namespace scylladb::alternator {

class RoutingScope {
public:
    virtual ~RoutingScope() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    [[nodiscard]] virtual std::string ToString() const = 0;
    [[nodiscard]] virtual std::string LocalNodesQuery() const = 0;
    [[nodiscard]] virtual std::shared_ptr<const RoutingScope> Fallback() const = 0;
    [[nodiscard]] virtual bool IsCluster() const { return false; }
};

using RoutingScopePtr = std::shared_ptr<const RoutingScope>;

class ClusterScope final : public RoutingScope {
public:
    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] std::string ToString() const override;
    [[nodiscard]] std::string LocalNodesQuery() const override;
    [[nodiscard]] RoutingScopePtr Fallback() const override;
    [[nodiscard]] bool IsCluster() const override;
};

class DatacenterScope final : public RoutingScope {
public:
    DatacenterScope(std::string datacenter, RoutingScopePtr fallback);

    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] std::string ToString() const override;
    [[nodiscard]] std::string LocalNodesQuery() const override;
    [[nodiscard]] RoutingScopePtr Fallback() const override;

private:
    std::string datacenter_;
    RoutingScopePtr fallback_;
};

class RackScope final : public RoutingScope {
public:
    RackScope(std::string datacenter, std::string rack, RoutingScopePtr fallback);

    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] std::string ToString() const override;
    [[nodiscard]] std::string LocalNodesQuery() const override;
    [[nodiscard]] RoutingScopePtr Fallback() const override;

private:
    std::string datacenter_;
    std::string rack_;
    RoutingScopePtr fallback_;
};

RoutingScopePtr NewClusterScope();
RoutingScopePtr NewDCScope(std::string datacenter, RoutingScopePtr fallback = nullptr);
RoutingScopePtr NewRackScope(std::string datacenter, std::string rack, RoutingScopePtr fallback = nullptr);

} // namespace scylladb::alternator
