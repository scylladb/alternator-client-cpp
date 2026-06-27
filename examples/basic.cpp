#include <iostream>

#include <scylladb/alternator/live_nodes.h>

int main() {
    scylladb::alternator::Config cfg;
    cfg.port = 8080;

    scylladb::alternator::AlternatorLiveNodes nodes({"127.0.0.1"}, cfg);
    const auto node = nodes.NextNode();
    std::cout << node.ToString() << '\n';
}
