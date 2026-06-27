#include <scylladb/alternator/murmur3.h>

#include <cstring>

namespace scylladb::alternator {
namespace {

constexpr std::uint64_t c1 = 0x87c37b91114253d5ULL;
constexpr std::uint64_t c2 = 0x4cf5ad432745937fULL;
constexpr std::uint64_t fmix1 = 0xff51afd7ed558ccdULL;
constexpr std::uint64_t fmix2 = 0xc4ceb9fe1a85ec53ULL;

std::uint64_t Rotl(std::uint64_t x, std::uint8_t r) {
    return (x << r) | (x >> (64 - r));
}

std::uint64_t Fmix(std::uint64_t k) {
    k ^= k >> 33;
    k *= fmix1;
    k ^= k >> 33;
    k *= fmix2;
    k ^= k >> 33;
    return k;
}

std::uint64_t ReadLittleEndian64(const std::uint8_t* data) {
    std::uint64_t out = 0;
    for (int i = 0; i < 8; ++i) {
        out |= static_cast<std::uint64_t>(data[i]) << (8 * i);
    }
    return out;
}

std::int64_t Murmur3H1Bytes(const std::uint8_t* data, std::size_t length) {
    std::uint64_t h1 = 0;
    std::uint64_t h2 = 0;

    const std::size_t nblocks = length / 16;
    for (std::size_t i = 0; i < nblocks; ++i) {
        std::uint64_t k1 = ReadLittleEndian64(data + i * 16);
        std::uint64_t k2 = ReadLittleEndian64(data + i * 16 + 8);

        k1 *= c1;
        k1 = Rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;

        h1 = Rotl(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = Rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;

        h2 = Rotl(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    const auto* tail = data + nblocks * 16;
    std::uint64_t k1 = 0;
    std::uint64_t k2 = 0;

    switch (length & 15U) {
    case 15:
        k2 ^= static_cast<std::uint64_t>(tail[14]) << 48;
        [[fallthrough]];
    case 14:
        k2 ^= static_cast<std::uint64_t>(tail[13]) << 40;
        [[fallthrough]];
    case 13:
        k2 ^= static_cast<std::uint64_t>(tail[12]) << 32;
        [[fallthrough]];
    case 12:
        k2 ^= static_cast<std::uint64_t>(tail[11]) << 24;
        [[fallthrough]];
    case 11:
        k2 ^= static_cast<std::uint64_t>(tail[10]) << 16;
        [[fallthrough]];
    case 10:
        k2 ^= static_cast<std::uint64_t>(tail[9]) << 8;
        [[fallthrough]];
    case 9:
        k2 ^= static_cast<std::uint64_t>(tail[8]);
        k2 *= c2;
        k2 = Rotl(k2, 33);
        k2 *= c1;
        h2 ^= k2;
        [[fallthrough]];
    case 8:
        k1 ^= static_cast<std::uint64_t>(tail[7]) << 56;
        [[fallthrough]];
    case 7:
        k1 ^= static_cast<std::uint64_t>(tail[6]) << 48;
        [[fallthrough]];
    case 6:
        k1 ^= static_cast<std::uint64_t>(tail[5]) << 40;
        [[fallthrough]];
    case 5:
        k1 ^= static_cast<std::uint64_t>(tail[4]) << 32;
        [[fallthrough]];
    case 4:
        k1 ^= static_cast<std::uint64_t>(tail[3]) << 24;
        [[fallthrough]];
    case 3:
        k1 ^= static_cast<std::uint64_t>(tail[2]) << 16;
        [[fallthrough]];
    case 2:
        k1 ^= static_cast<std::uint64_t>(tail[1]) << 8;
        [[fallthrough]];
    case 1:
        k1 ^= static_cast<std::uint64_t>(tail[0]);
        k1 *= c1;
        k1 = Rotl(k1, 31);
        k1 *= c2;
        h1 ^= k1;
        break;
    default:
        break;
    }

    h1 ^= length;
    h2 ^= length;

    h1 += h2;
    h2 += h1;

    h1 = Fmix(h1);
    h2 = Fmix(h2);

    h1 += h2;
    return static_cast<std::int64_t>(h1);
}

} // namespace

std::int64_t Murmur3H1(const std::vector<std::uint8_t>& data) {
    return Murmur3H1Bytes(data.data(), data.size());
}

std::int64_t Murmur3H1(std::string_view data) {
    return Murmur3H1Bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

} // namespace scylladb::alternator
