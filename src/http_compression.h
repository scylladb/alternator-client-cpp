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
