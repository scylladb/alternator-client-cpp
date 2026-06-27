#pragma once

#include <cstdint>
#include <string>

namespace scylladb::alternator {

class Url {
public:
    Url() = default;
    Url(std::string scheme, std::string host, std::uint16_t port);

    static Url FromHostPort(std::string scheme, std::string host, std::uint16_t port);

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] std::string Authority() const;
    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] Url WithPathAndQuery(std::string path, std::string query = {}) const;

    std::string scheme;
    std::string host;
    std::uint16_t port = 0;
    std::string path;
    std::string query;
};

bool operator==(const Url& lhs, const Url& rhs);
bool operator!=(const Url& lhs, const Url& rhs);
bool operator<(const Url& lhs, const Url& rhs);

} // namespace scylladb::alternator
