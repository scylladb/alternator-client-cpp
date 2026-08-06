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

#include <scylladb/alternator/config.h>

#include <gtest/gtest.h>

#include <string>

using namespace scylladb::alternator;

TEST(Config, DefaultUserAgentUsesProjectVersion) {
    const Config config;

    EXPECT_EQ(
        config.user_agent,
        std::string("scylladb-alternator-client-cpp/") + SCYLLADB_ALTERNATOR_CLIENT_CPP_VERSION);
    EXPECT_NE(config.user_agent, "scylladb-alternator-client-cpp/devel");
}

TEST(Config, UserAgentCanBeCleared) {
    Config config;

    config.user_agent.clear();

    EXPECT_TRUE(config.user_agent.empty());
}
