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

#include "testinfra/cluster_spec.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace scylladb::alternator::testinfra;

namespace {

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(const char* name)
        : name_(name) {
        const char* value = std::getenv(name_.c_str());
        if (value != nullptr) {
            original_ = value;
        }
    }

    ~ScopedEnvironmentVariable() {
        if (original_) {
            (void)setenv(name_.c_str(), original_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

    void Set(const char* value) {
        ASSERT_EQ(setenv(name_.c_str(), value, 1), 0);
    }

    void Unset() {
        ASSERT_EQ(unsetenv(name_.c_str()), 0);
    }

private:
    std::string name_;
    std::optional<std::string> original_;
};

bool SecurityCombinationIsValid(bool enforced,
                                AuthenticationMode authentication,
                                AuthorizationMode authorization) {
    if (authentication == AuthenticationMode::AllowAll && authorization != AuthorizationMode::AllowAll) {
        return false;
    }
    return !enforced ||
           (authentication == AuthenticationMode::Password && authorization == AuthorizationMode::Cassandra);
}

} // namespace

TEST(ClusterSpec, DefaultsMatchCcmContract) {
    const ClusterSpec spec;

    EXPECT_EQ(spec.ScyllaVersion(), "release:2025.2.5");
    ASSERT_EQ(spec.Topology().Datacenters().size(), 1U);
    ASSERT_EQ(spec.Topology().Datacenters().front().Racks().size(), 1U);
    EXPECT_EQ(spec.Topology().Datacenters().front().Racks().front().NodeCount(), 3);
    EXPECT_EQ(spec.Topology().NodeCount(), 3);
    EXPECT_EQ(
        spec.Transports(),
        (std::set<AlternatorTransport>{AlternatorTransport::Http, AlternatorTransport::Https}));
    EXPECT_EQ(spec.Security(), ClusterSecuritySpec::Disabled());
    EXPECT_EQ(spec.Resources(), NodeResources(2, 1024));
    EXPECT_TRUE(spec.ScyllaYamlOverrides().empty());
}

TEST(ClusterSpec, DefaultSpecReadsScyllaVersionFromEnvironment) {
    ScopedEnvironmentVariable version("SCYLLA_VERSION");

    version.Unset();
    EXPECT_EQ(ClusterSpecs::DefaultSpec().ScyllaVersion(), ClusterSpec::kDefaultScyllaVersion);

    version.Set("release:2026.1.6");
    EXPECT_EQ(ClusterSpecs::DefaultSpec().ScyllaVersion(), "release:2026.1.6");

    version.Set("");
    EXPECT_THROW((void)ClusterSpecs::DefaultSpec(), std::invalid_argument);
}

TEST(ClusterSpec, WithMethodsReturnValidatedCopies) {
    const ClusterSpec original;
    const ClusterTopology topology({DatacenterSpec::Create({1, 2}), DatacenterSpec::Create({3})});
    const ClusterSecuritySpec security(
        AuthenticationMode::Transitional,
        AuthorizationMode::Transitional,
        false);

    const auto modified = original
        .WithScyllaVersion("release:2026.1.6")
        .WithTopology(topology)
        .WithTransports(std::vector<AlternatorTransport>{AlternatorTransport::Https})
        .WithSecurity(security)
        .WithResources(NodeResources(4, 2048))
        .WithYamlOverride("custom_option", "true");

    EXPECT_EQ(original, ClusterSpec());
    EXPECT_EQ(modified.ScyllaVersion(), "release:2026.1.6");
    EXPECT_EQ(modified.Topology(), topology);
    EXPECT_EQ(modified.Transports(), (std::set<AlternatorTransport>{AlternatorTransport::Https}));
    EXPECT_EQ(modified.Security(), security);
    EXPECT_EQ(modified.Resources(), NodeResources(4, 2048));
    EXPECT_EQ(modified.ScyllaYamlOverrides().at("custom_option"), "true");

    const auto initializer_list_copy = original.WithTransports({AlternatorTransport::Http});
    EXPECT_EQ(initializer_list_copy.Transports(), (std::set<AlternatorTransport>{AlternatorTransport::Http}));
}

TEST(ClusterSpec, ValidatesTopologyResourcesAndTransports) {
    EXPECT_THROW((void)RackSpec(0), std::invalid_argument);
    EXPECT_THROW((void)DatacenterSpec(std::vector<RackSpec>{}), std::invalid_argument);
    EXPECT_THROW((void)ClusterTopology(std::vector<DatacenterSpec>{}), std::invalid_argument);
    EXPECT_THROW((void)NodeResources(0, 1024), std::invalid_argument);
    EXPECT_THROW((void)NodeResources(2, 0), std::invalid_argument);
    EXPECT_THROW(
        (void)ClusterSpec().WithTransports(std::vector<AlternatorTransport>{}),
        std::invalid_argument);

    EXPECT_NO_THROW((void)ClusterSpec().WithTopology(ClusterTopology::SingleDatacenter({4, 5})));
    EXPECT_THROW(
        (void)ClusterSpec().WithTopology(ClusterTopology::SingleDatacenter({5, 5})),
        std::invalid_argument);

    const auto overflowing = ClusterTopology::SingleDatacenter(
        {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()});
    EXPECT_THROW((void)overflowing.NodeCount(), std::invalid_argument);
    EXPECT_THROW((void)ClusterSpec().WithTopology(overflowing), std::invalid_argument);
}

TEST(ClusterSpec, ValidatesEverySecurityCombination) {
    const std::array<AuthenticationMode, 3> authentication_modes = {
        AuthenticationMode::AllowAll,
        AuthenticationMode::Password,
        AuthenticationMode::Transitional,
    };
    const std::array<AuthorizationMode, 3> authorization_modes = {
        AuthorizationMode::AllowAll,
        AuthorizationMode::Cassandra,
        AuthorizationMode::Transitional,
    };

    for (const bool enforced : {false, true}) {
        for (const auto authentication : authentication_modes) {
            for (const auto authorization : authorization_modes) {
                if (SecurityCombinationIsValid(enforced, authentication, authorization)) {
                    EXPECT_NO_THROW((void)ClusterSecuritySpec(authentication, authorization, enforced));
                } else {
                    EXPECT_THROW(
                        (void)ClusterSecuritySpec(authentication, authorization, enforced),
                        std::invalid_argument);
                }
            }
        }
    }
    EXPECT_EQ(ClusterSecuritySpec::Enforced(),
              ClusterSecuritySpec(AuthenticationMode::Password, AuthorizationMode::Cassandra, true));
}

TEST(ClusterSpec, CanonicalizesOuterUnicodeWhitespace) {
    const auto canonical = ClusterSpec().WithYamlOverride("custom_option", "true");
    const auto padded = ClusterSpec().WithYamlOverride(
        u8"\u2003\u00a0custom_option\u3000",
        "true");

    EXPECT_EQ(padded.ScyllaYamlOverrides(), canonical.ScyllaYamlOverrides());
    EXPECT_EQ(padded.ReuseKey(), canonical.ReuseKey());

    const auto replaced = padded.WithYamlOverride(u8"\u2003custom_option\u00a0", "second");
    ASSERT_EQ(replaced.ScyllaYamlOverrides().size(), 1U);
    EXPECT_EQ(replaced.ScyllaYamlOverrides().at("custom_option"), "second");
}

TEST(ClusterSpec, RejectsReservedYamlRootsAndAliases) {
    const std::vector<std::string> reserved = {
        "alternator_enforce_authorization ",
        u8"\u00a0alternator_encryption_options.enabled\u2003",
        "api_address",
        "auto_bootstrap",
        "native_transport_port",
        "endpoint_snitch",
        "overprovisioned",
        "kernel_page_cache",
        "max_networking_io_control_blocks",
        "unsafe_bypass_fsync",
        "blocked_reactor_notify_ms",
        "smp",
        "memory",
        "workdir",
        "datadir",
        "cql_port",
    };
    for (const auto& key : reserved) {
        EXPECT_THROW((void)ClusterSpec().WithYamlOverride(key, "false"), std::invalid_argument)
            << "accepted reserved key " << key;
    }
}

TEST(ClusterSpec, ValidatesYamlKeyShapeAndValueSyntax) {
    const std::vector<std::string> invalid_keys = {
        "",
        " ",
        u8"\u200b",
        ".option",
        "option.",
        "parent..child",
        "parent.child.grandchild",
        "option:value",
        "option\ninjected",
        "\toption",
        "option-name",
        "option name",
        u8"na\u00efve",
        "1option",
    };
    for (const auto& key : invalid_keys) {
        EXPECT_THROW((void)ClusterSpec().WithYamlOverride(key, "true"), std::invalid_argument)
            << "accepted unsafe key " << key;
    }

    for (const auto& value : {std::string(""), std::string(u8"\u2003\u00a0"), std::string("["),
                              std::string("{key: value"), std::string("'unterminated"),
                              std::string("\"bad\\q\""), std::string("foo: bar: baz"),
                              std::string("a: 1\na: 2"), std::string("{one: 1, one: 2}"),
                              std::string("outer: {one: 1, one: 2}"),
                              std::string("one\n---\ntwo")}) {
        EXPECT_THROW((void)ClusterSpec().WithYamlOverride("custom_option", value), std::invalid_argument)
            << "accepted invalid value " << value;
    }
    EXPECT_THROW(
        (void)ClusterSpec().WithYamlOverride("custom_option", std::string("true\0false", 10)),
        std::invalid_argument);

    EXPECT_NO_THROW((void)ClusterSpec()
                        .WithYamlOverride("custom_null", "null")
                        .WithYamlOverride("custom_list", "[one, two]")
                        .WithYamlOverride("custom_mapping", "{one: 1, two: false}"));
    EXPECT_NO_THROW((void)ClusterSpec()
                        .WithYamlOverride("_custom2", "true")
                        .WithYamlOverride("custom_group.child_2", "42"));
}

TEST(ClusterSpec, RejectsNestedOverrideBelowScalarRoot) {
    EXPECT_THROW(
        (void)ClusterSpec()
            .WithYamlOverride("custom_group", "5")
            .WithYamlOverride("custom_group.child", "42"),
        std::invalid_argument);

    EXPECT_NO_THROW((void)ClusterSpec()
                        .WithYamlOverride("custom_group", "{existing: true}")
                        .WithYamlOverride("custom_group.child", "42"));
}

TEST(ClusterSpec, ReuseKeyIsCanonicalCompleteAndDelimiterSafe) {
    const auto first = ClusterSpec()
        .WithYamlOverride("z_option", "two")
        .WithYamlOverride("a_option", "one");
    const auto second = ClusterSpec()
        .WithYamlOverride("a_option", "one")
        .WithYamlOverride("z_option", "two");
    EXPECT_EQ(first.ReuseKey(), second.ReuseKey());

    EXPECT_NE(first.ReuseKey(), first.WithResources(NodeResources(3, 1024)).ReuseKey());
    EXPECT_NE(first.ReuseKey(), first.WithTransports({AlternatorTransport::Http}).ReuseKey());
    EXPECT_NE(first.ReuseKey(), first.WithTopology(ClusterTopology::SingleDatacenter({1, 2})).ReuseKey());
    EXPECT_NE(first.ReuseKey(), first.WithSecurity(ClusterSecuritySpec::Enforced()).ReuseKey());

    const auto delimiter_version = ClusterSpec().WithScyllaVersion(
        "X|transports:2:HTTP:HTTPS|security:ALLOW_ALL:ALLOW_ALL:0|resources:2:1024");
    const auto delimiter_value = ClusterSpec()
        .WithScyllaVersion("X")
        .WithYamlOverride(
            "logger_log_level",
            "info # |transports:2:HTTP:HTTPS|security:ALLOW_ALL:ALLOW_ALL:0|resources:2:1024");
    EXPECT_NE(delimiter_version.ReuseKey(), delimiter_value.ReuseKey());
}
