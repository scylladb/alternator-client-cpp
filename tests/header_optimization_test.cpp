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
#include <vector>

using namespace scylladb::alternator;

namespace {

class TestHeaderOptimization final : public HeaderOptimization {
public:
    std::vector<std::string> AllowedHeaders(const HeaderOptimizationContext& context) const override {
        std::vector<std::string> headers = {"Host"};
        if (context.credentials_configured) {
            headers.push_back("Authorization");
        }
        if (context.user_agent_configured) {
            headers.push_back("User-Agent");
        }
        return headers;
    }
};

} // namespace

TEST(HeaderOptimization, DefaultAllowlistUsesContext) {
    const DefaultHeaderOptimization optimization;

    EXPECT_EQ(
        optimization.AllowedHeaders({false, false}),
        std::vector<std::string>({
            "Host",
            "X-Amz-Target",
            "Content-Length",
            "Accept-Encoding",
            "Content-Encoding",
        }));
    EXPECT_EQ(
        optimization.AllowedHeaders({true, true}),
        std::vector<std::string>({
            "Host",
            "X-Amz-Target",
            "Content-Length",
            "Accept-Encoding",
            "Content-Encoding",
            "Authorization",
            "X-Amz-Date",
            "User-Agent",
        }));
}

TEST(HeaderOptimization, FixedAllowlistIgnoresContext) {
    const HeaderAllowlistOptimization optimization({"Host", "X-Amz-Target"});

    EXPECT_EQ(
        optimization.AllowedHeaders({true, true}),
        std::vector<std::string>({
            "Host",
            "X-Amz-Target",
        }));
}

TEST(HeaderOptimization, SupportsCustomImplementation) {
    const TestHeaderOptimization optimization;

    EXPECT_EQ(
        optimization.AllowedHeaders({true, false}),
        std::vector<std::string>({
            "Host",
            "Authorization",
        }));
    EXPECT_EQ(
        optimization.AllowedHeaders({false, true}),
        std::vector<std::string>({
            "Host",
            "User-Agent",
        }));
}
