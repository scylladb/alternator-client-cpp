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

#include <iostream>

#include <scylladb/alternator/live_nodes.h>

int main() {
    scylladb::alternator::Config cfg;
    cfg.port = 8080;

    scylladb::alternator::AlternatorLiveNodes nodes({"127.0.0.1"}, cfg);
    const auto node = nodes.NextNode();
    std::cout << node.ToString() << '\n';
}
