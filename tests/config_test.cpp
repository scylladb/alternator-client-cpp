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

#include <chrono>
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

TEST(Config, DefaultDiscoveryTimeoutsAreFiniteWithoutChangingHttpTimeoutCompatibility) {
    const Config config;

    EXPECT_EQ(config.http_client_timeout, std::chrono::milliseconds::zero());
    EXPECT_EQ(config.connect_timeout, std::chrono::seconds{1});
    EXPECT_EQ(config.discovery_attempt_timeout, std::chrono::seconds{5});
    EXPECT_EQ(config.max_discovery_response_bytes, 1024U * 1024U);
    EXPECT_EQ(config.discovery_cycle_timeout, std::chrono::seconds{5});
}

TEST(Config, DiscoveryTimeoutRemainsAfterLegacyAggregateMembers) {
    const Config config{
        8043,
        "https",
        NewClusterScope(),
        "aggregate-region",
        Credentials{"access", "secret"},
        std::chrono::milliseconds{11},
        std::chrono::milliseconds{12},
        std::chrono::milliseconds{13},
        std::chrono::milliseconds{14},
        false,
        "ca.pem",
        "client.pem",
        "client.key",
        false,
        15,
        std::chrono::seconds{16},
        17,
        false,
        nullptr,
        {},
        "aggregate-agent",
        nullptr,
        {},
        {},
    };

    EXPECT_EQ(config.connect_timeout, std::chrono::milliseconds{14});
    EXPECT_FALSE(config.verify_ssl);
    EXPECT_EQ(config.max_connections, 17U);
    EXPECT_EQ(config.user_agent, "aggregate-agent");
    EXPECT_EQ(config.discovery_attempt_timeout, std::chrono::seconds{5});
    EXPECT_EQ(config.max_discovery_response_bytes, 1024U * 1024U);
    EXPECT_EQ(config.discovery_cycle_timeout, std::chrono::seconds{5});
}

TEST(Config, RejectsZeroDiscoveryResponseLimit) {
    Config config;
    config.max_discovery_response_bytes = 0;

    EXPECT_THROW(ValidateConfig(config), std::invalid_argument);
}
