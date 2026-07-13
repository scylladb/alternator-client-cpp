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
