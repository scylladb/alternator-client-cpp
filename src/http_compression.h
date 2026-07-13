#pragma once

#include <string>
#include <vector>

#include <scylladb/alternator/config.h>

namespace scylladb::alternator::detail {

[[nodiscard]] std::string BuildRequestContentEncodingValue(
    const std::shared_ptr<HttpRequestCompressor>& request_compressor);
[[nodiscard]] std::string BuildAcceptEncodingValue(
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders);
[[nodiscard]] std::string DecodeHttpResponseBody(
    std::string body,
    const std::string& content_encoding,
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders);
[[nodiscard]] std::string FindHttpHeaderValue(const std::string& headers, const std::string& name);
[[nodiscard]] std::string NormalizeResponseEncoding(std::string encoding);
[[nodiscard]] std::string ToLowerAscii(std::string value);

} // namespace scylladb::alternator::detail
