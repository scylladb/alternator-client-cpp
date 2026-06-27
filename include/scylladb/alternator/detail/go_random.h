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
