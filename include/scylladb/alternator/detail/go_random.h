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

#include <array>
#include <cstdint>

namespace scylladb::alternator::detail {

class GoRandom {
public:
    explicit GoRandom(std::int64_t seed);

    [[nodiscard]] std::int32_t Int31n(std::int32_t n);

private:
    void Seed(std::int64_t seed);
    [[nodiscard]] std::uint64_t Uint64();
    [[nodiscard]] std::int64_t Int63();
    [[nodiscard]] std::int32_t Int31();

    int tap_ = 0;
    int feed_ = 0;
    std::array<std::int64_t, 607> vec_{};
};

} // namespace scylladb::alternator::detail
