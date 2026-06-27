#include <scylladb/alternator/routing_scope.h>

#include <sstream>
#include <utility>

namespace scylladb::alternator {

std::string ClusterScope::Name() const {
    return "Cluster";
}

std::string ClusterScope::ToString() const {
    return "Cluster()";
}

std::string ClusterScope::LocalNodesQuery() const {
    return {};
}

RoutingScopePtr ClusterScope::Fallback() const {
    return nullptr;
}

bool ClusterScope::IsCluster() const {
    return true;
}

DatacenterScope::DatacenterScope(std::string datacenter, RoutingScopePtr fallback)
    : datacenter_(std::move(datacenter))
    , fallback_(std::move(fallback)) {}

std::string DatacenterScope::Name() const {
    return "Datacenter";
}

std::string DatacenterScope::ToString() const {
    return "Datacenter(dc=" + datacenter_ + ")";
}

std::string DatacenterScope::LocalNodesQuery() const {
    return "dc=" + datacenter_;
}

RoutingScopePtr DatacenterScope::Fallback() const {
    return fallback_;
}

RackScope::RackScope(std::string datacenter, std::string rack, RoutingScopePtr fallback)
    : datacenter_(std::move(datacenter))
    , rack_(std::move(rack))
    , fallback_(std::move(fallback)) {}

std::string RackScope::Name() const {
    return "Rack";
}

std::string RackScope::ToString() const {
    return "Rack(dc=" + datacenter_ + ", rack=" + rack_ + ")";
}

std::string RackScope::LocalNodesQuery() const {
    return "dc=" + datacenter_ + "&rack=" + rack_;
}

RoutingScopePtr RackScope::Fallback() const {
    return fallback_;
}

RoutingScopePtr NewClusterScope() {
    return std::make_shared<ClusterScope>();
}

RoutingScopePtr NewDCScope(std::string datacenter, RoutingScopePtr fallback) {
    return std::make_shared<DatacenterScope>(std::move(datacenter), std::move(fallback));
}

RoutingScopePtr NewRackScope(std::string datacenter, std::string rack, RoutingScopePtr fallback) {
    return std::make_shared<RackScope>(std::move(datacenter), std::move(rack), std::move(fallback));
}

} // namespace scylladb::alternator
