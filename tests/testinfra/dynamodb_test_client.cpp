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

#include "dynamodb_test_client.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace scylladb::alternator::testinfra {
namespace {

constexpr char kRegion[] = "us-east-1";
constexpr char kService[] = "dynamodb";
constexpr char kContentType[] = "application/x-amz-json-1.0";
constexpr std::size_t kMaximumResponseBytes = 4U * 1024U * 1024U;
constexpr auto kPollInterval = std::chrono::milliseconds(100);

using Clock = std::chrono::steady_clock;

struct HttpResponse {
    long status_code = 0;
    std::string body;
};

struct ListTablesPage {
    std::vector<std::string> table_names;
    std::optional<std::string> last_evaluated_table_name;
};

class CurlHandle final {
public:
    CurlHandle()
        : handle_(curl_easy_init()) {
        if (handle_ == nullptr) {
            throw std::runtime_error("curl_easy_init failed for DynamoDB cleanup client");
        }
    }

    ~CurlHandle() {
        curl_easy_cleanup(handle_);
    }

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    [[nodiscard]] CURL* Get() const noexcept {
        return handle_;
    }

private:
    CURL* handle_;
};

class CurlHeaders final {
public:
    ~CurlHeaders() {
        curl_slist_free_all(headers_);
    }

    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;
    CurlHeaders() = default;

    void Add(const std::string& value) {
        auto* appended = curl_slist_append(headers_, value.c_str());
        if (appended == nullptr) {
            throw std::runtime_error("failed to allocate DynamoDB HTTP headers");
        }
        headers_ = appended;
    }

    [[nodiscard]] curl_slist* Get() const noexcept {
        return headers_;
    }

private:
    curl_slist* headers_ = nullptr;
};

struct ResponseBuffer {
    std::string value;
    bool exceeded_limit = false;
};

void EnsureCurlInitialized() {
    static std::once_flag once;
    static CURLcode result = CURLE_OK;
    std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string("curl_global_init failed for DynamoDB cleanup client: ") +
            curl_easy_strerror(result));
    }
}

void CheckCurlOption(CURLcode result, const char* option) {
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string("failed to configure ") + option + ": " + curl_easy_strerror(result));
    }
}

std::size_t WriteResponse(char* data, std::size_t size, std::size_t count, void* context) {
    auto* buffer = static_cast<ResponseBuffer*>(context);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        buffer->exceeded_limit = true;
        return 0;
    }
    const auto bytes = size * count;
    if (bytes > kMaximumResponseBytes - std::min(buffer->value.size(), kMaximumResponseBytes)) {
        buffer->exceeded_limit = true;
        return 0;
    }
    buffer->value.append(data, bytes);
    return bytes;
}

std::string Hex(const unsigned char* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2] = digits[data[index] >> 4U];
        result[index * 2 + 1] = digits[data[index] & 0x0fU];
    }
    return result;
}

std::array<unsigned char, 32> Sha256(const std::string& value) {
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_Digest(
            value.data(),
            value.size(),
            digest.data(),
            &size,
            EVP_sha256(),
            nullptr) != 1 ||
        size != digest.size()) {
        throw std::runtime_error("OpenSSL failed to compute a SHA-256 digest");
    }
    return digest;
}

std::string Sha256Hex(const std::string& value) {
    const auto digest = Sha256(value);
    return Hex(digest.data(), digest.size());
}

std::vector<unsigned char> HmacSha256(
    const unsigned char* key,
    std::size_t key_size,
    const std::string& value) {
    if (key_size > static_cast<std::size_t>(INT_MAX)) {
        throw std::runtime_error("SigV4 key is too large");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (HMAC(
            EVP_sha256(),
            key,
            static_cast<int>(key_size),
            reinterpret_cast<const unsigned char*>(value.data()),
            value.size(),
            digest.data(),
            &size) == nullptr) {
        throw std::runtime_error("OpenSSL failed to compute a SigV4 HMAC");
    }
    return {digest.begin(), digest.begin() + size};
}

std::vector<unsigned char> HmacSha256(const std::string& key, const std::string& value) {
    return HmacSha256(
        reinterpret_cast<const unsigned char*>(key.data()),
        key.size(),
        value);
}

std::vector<unsigned char> HmacSha256(
    const std::vector<unsigned char>& key,
    const std::string& value) {
    return HmacSha256(key.data(), key.size(), value);
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            } else {
                output << byte;
            }
        }
    }
    output << '"';
    return output.str();
}

class JsonReader final {
public:
    explicit JsonReader(const std::string& input)
        : input_(input) {}

    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool AtEnd() {
        SkipWhitespace();
        return position_ == input_.size();
    }

    [[nodiscard]] bool Consume(char expected) {
        SkipWhitespace();
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void Expect(char expected) {
        if (!Consume(expected)) {
            throw Error(std::string("expected '") + expected + "'");
        }
    }

    [[nodiscard]] std::string ReadString() {
        SkipWhitespace();
        if (position_ >= input_.size() || input_[position_] != '"') {
            throw Error("expected a JSON string");
        }
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character < 0x20U) {
                throw Error("unescaped control in JSON string");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ == input_.size()) {
                throw Error("unterminated JSON escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                result.push_back(escaped);
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                auto code_point = ReadHexCodeUnit();
                if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') {
                        throw Error("high surrogate lacks a low surrogate");
                    }
                    position_ += 2;
                    const auto low = ReadHexCodeUnit();
                    if (low < 0xdc00U || low > 0xdfffU) {
                        throw Error("invalid low surrogate");
                    }
                    code_point = 0x10000U + ((code_point - 0xd800U) << 10U) + (low - 0xdc00U);
                } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
                    throw Error("unexpected low surrogate");
                }
                AppendUtf8(result, code_point);
                break;
            }
            default:
                throw Error("unsupported JSON escape");
            }
        }
        throw Error("unterminated JSON string");
    }

    [[nodiscard]] bool ConsumeNull() {
        SkipWhitespace();
        if (input_.compare(position_, 4, "null") != 0) {
            return false;
        }
        position_ += 4;
        return true;
    }

    void SkipValue() {
        SkipWhitespace();
        if (position_ == input_.size()) {
            throw Error("expected a JSON value");
        }
        if (input_[position_] == '"') {
            (void)ReadString();
            return;
        }
        if (Consume('{')) {
            if (Consume('}')) {
                return;
            }
            while (true) {
                (void)ReadString();
                Expect(':');
                SkipValue();
                if (Consume('}')) {
                    return;
                }
                Expect(',');
            }
        }
        if (Consume('[')) {
            if (Consume(']')) {
                return;
            }
            while (true) {
                SkipValue();
                if (Consume(']')) {
                    return;
                }
                Expect(',');
            }
        }
        const auto start = position_;
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (character == ',' || character == ']' || character == '}' ||
                character == ' ' || character == '\t' || character == '\r' || character == '\n') {
                break;
            }
            ++position_;
        }
        if (position_ == start) {
            throw Error("invalid JSON value");
        }
        const auto token = input_.substr(start, position_ - start);
        if (token != "null" && token != "true" && token != "false" && !IsJsonNumber(token)) {
            throw Error("invalid JSON literal");
        }
    }

private:
    [[nodiscard]] std::runtime_error Error(const std::string& message) const {
        return std::runtime_error(
            "invalid DynamoDB JSON response at byte " + std::to_string(position_) + ": " + message);
    }

    [[nodiscard]] std::uint32_t ReadHexCodeUnit() {
        if (position_ + 4 > input_.size()) {
            throw Error("short JSON Unicode escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = input_[position_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                throw Error("invalid JSON Unicode escape");
            }
        }
        return value;
    }

    static void AppendUtf8(std::string& output, std::uint32_t code_point) {
        if (code_point <= 0x7fU) {
            output.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else if (code_point <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
    }

    static bool IsJsonNumber(const std::string& token) {
        std::size_t position = 0;
        if (position < token.size() && token[position] == '-') {
            ++position;
        }
        if (position == token.size()) {
            return false;
        }
        if (token[position] == '0') {
            ++position;
        } else {
            if (token[position] < '1' || token[position] > '9') {
                return false;
            }
            while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
                ++position;
            }
        }
        if (position < token.size() && token[position] == '.') {
            ++position;
            const auto fraction_start = position;
            while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
                ++position;
            }
            if (position == fraction_start) {
                return false;
            }
        }
        if (position < token.size() && (token[position] == 'e' || token[position] == 'E')) {
            ++position;
            if (position < token.size() && (token[position] == '+' || token[position] == '-')) {
                ++position;
            }
            const auto exponent_start = position;
            while (position < token.size() && token[position] >= '0' && token[position] <= '9') {
                ++position;
            }
            if (position == exponent_start) {
                return false;
            }
        }
        return position == token.size();
    }

    const std::string& input_;
    std::size_t position_ = 0;
};

ListTablesPage ParseListTablesPage(const std::string& body) {
    JsonReader json(body);
    ListTablesPage page;
    bool saw_table_names = false;
    bool saw_last_name = false;

    json.Expect('{');
    if (!json.Consume('}')) {
        while (true) {
            const auto key = json.ReadString();
            json.Expect(':');
            if (key == "TableNames") {
                if (saw_table_names) {
                    throw std::runtime_error("DynamoDB ListTables response repeats TableNames");
                }
                saw_table_names = true;
                json.Expect('[');
                if (!json.Consume(']')) {
                    while (true) {
                        page.table_names.push_back(json.ReadString());
                        if (json.Consume(']')) {
                            break;
                        }
                        json.Expect(',');
                    }
                }
            } else if (key == "LastEvaluatedTableName") {
                if (saw_last_name) {
                    throw std::runtime_error(
                        "DynamoDB ListTables response repeats LastEvaluatedTableName");
                }
                saw_last_name = true;
                if (!json.ConsumeNull()) {
                    page.last_evaluated_table_name = json.ReadString();
                }
            } else {
                json.SkipValue();
            }
            if (json.Consume('}')) {
                break;
            }
            json.Expect(',');
        }
    }
    if (!json.AtEnd()) {
        throw std::runtime_error("DynamoDB ListTables response has trailing JSON data");
    }
    if (!saw_table_names) {
        throw std::runtime_error("DynamoDB ListTables response lacks TableNames");
    }
    return page;
}

std::optional<std::string> ParseErrorType(const std::string& body) {
    try {
        JsonReader json(body);
        std::optional<std::string> type;
        json.Expect('{');
        if (!json.Consume('}')) {
            while (true) {
                const auto key = json.ReadString();
                json.Expect(':');
                if (key == "__type" || key == "code" || key == "Code") {
                    if (json.ConsumeNull()) {
                        type.reset();
                    } else {
                        type = json.ReadString();
                    }
                } else {
                    json.SkipValue();
                }
                if (json.Consume('}')) {
                    break;
                }
                json.Expect(',');
            }
        }
        if (!json.AtEnd()) {
            return std::nullopt;
        }
        return type;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool IsResourceNotFound(const HttpResponse& response) {
    const auto type = ParseErrorType(response.body);
    if (type && type->find("ResourceNotFoundException") != std::string::npos) {
        return true;
    }
    return response.body.find("ResourceNotFoundException") != std::string::npos;
}

std::string AbbreviateBody(const std::string& body) {
    constexpr std::size_t limit = 1024;
    if (body.size() <= limit) {
        return body;
    }
    return body.substr(0, limit) + "...[truncated]";
}

long RemainingMilliseconds(Clock::time_point deadline, const std::string& operation) {
    const auto now = Clock::now();
    if (now >= deadline) {
        throw std::runtime_error("timed out " + operation);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (remaining < 1) {
        remaining = 1;
    }
    return static_cast<long>(std::min<std::int64_t>(remaining, std::numeric_limits<long>::max()));
}

Clock::time_point DeadlineAfter(std::chrono::seconds timeout) {
    const auto now = Clock::now();
    const auto maximum_timeout =
        std::chrono::duration_cast<std::chrono::seconds>(Clock::time_point::max() - now);
    if (timeout >= maximum_timeout) {
        return Clock::time_point::max();
    }
    return now + std::chrono::duration_cast<Clock::duration>(timeout);
}

class DynamoDbHttpClient final {
public:
    explicit DynamoDbHttpClient(const AlternatorConnection& connection)
        : endpoint_(connection.seed_endpoint)
        , credentials_(connection.credentials.value_or(Credentials{"test", "test"}))
        , ca_certificate_path_(connection.ca_certificate_path.string()) {
        EnsureCurlInitialized();
        if (endpoint_.Empty()) {
            throw std::invalid_argument("DynamoDB cleanup connection has no seed endpoint");
        }
        if (endpoint_.scheme != "http" && endpoint_.scheme != "https") {
            throw std::invalid_argument("DynamoDB cleanup endpoint scheme must be http or https");
        }
        if (credentials_.access_key_id.empty() || credentials_.secret_access_key.empty()) {
            throw std::invalid_argument("DynamoDB cleanup credentials must be non-empty");
        }
    }

    [[nodiscard]] HttpResponse Post(
        const std::string& operation,
        const std::string& body,
        Clock::time_point deadline) const {
        const std::string target = "DynamoDB_20120810." + operation;
        const auto signed_headers = Sign(target, body);
        const auto timeout_ms = RemainingMilliseconds(deadline, "during DynamoDB " + operation);

        CurlHandle handle;
        CURL* curl = handle.Get();
        ResponseBuffer response_body;
        std::array<char, CURL_ERROR_SIZE> error_buffer{};
        CurlHeaders headers;
        headers.Add("Content-Type: " + std::string(kContentType));
        headers.Add("X-Amz-Target: " + target);
        headers.Add("X-Amz-Date: " + signed_headers.amz_date);
        headers.Add("X-Amz-Content-Sha256: " + signed_headers.payload_hash);
        headers.Add("Authorization: " + signed_headers.authorization);
        headers.Add("Host: " + endpoint_.Authority());
        headers.Add("Accept: application/x-amz-json-1.0");

        auto request_url = endpoint_;
        request_url.path = "/";
        request_url.query.clear();
        const auto url = request_url.ToString();

        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_URL, url.c_str()), "DynamoDB URL");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_POST, 1L), "DynamoDB POST method");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data()), "DynamoDB request body");
        CheckCurlOption(
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())),
            "DynamoDB request body size");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.Get()), "DynamoDB headers");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L), "DynamoDB signal policy");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_NOPROXY, "*"), "DynamoDB proxy bypass");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L), "DynamoDB TCP keepalive");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteResponse), "DynamoDB response writer");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body), "DynamoDB response buffer");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.data()), "DynamoDB error buffer");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms), "DynamoDB timeout");
        CheckCurlOption(
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 5000L)),
            "DynamoDB connect timeout");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L), "DynamoDB TLS peer verification");
        CheckCurlOption(curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L), "DynamoDB TLS host verification");
        if (endpoint_.scheme == "https" && !ca_certificate_path_.empty()) {
            CheckCurlOption(
                curl_easy_setopt(curl, CURLOPT_CAINFO, ca_certificate_path_.c_str()),
                "DynamoDB CA certificate");
        }

        const auto result = curl_easy_perform(curl);
        if (result != CURLE_OK) {
            if (response_body.exceeded_limit) {
                throw std::runtime_error("DynamoDB " + operation + " response exceeded 4 MiB");
            }
            const auto detail = error_buffer.front() == '\0'
                ? std::string(curl_easy_strerror(result))
                : std::string(error_buffer.data());
            throw std::runtime_error(
                "DynamoDB " + operation + " request to " + url + " failed: " + detail);
        }

        long status_code = 0;
        const auto info_result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        if (info_result != CURLE_OK) {
            throw std::runtime_error(
                "failed to read DynamoDB " + operation + " HTTP status: " +
                curl_easy_strerror(info_result));
        }
        return {status_code, std::move(response_body.value)};
    }

private:
    struct SignedHeaders {
        std::string amz_date;
        std::string payload_hash;
        std::string authorization;
    };

    [[nodiscard]] SignedHeaders Sign(const std::string& target, const std::string& body) const {
        const auto timestamp = std::time(nullptr);
        std::tm utc{};
        if (gmtime_r(&timestamp, &utc) == nullptr) {
            throw std::runtime_error("failed to obtain UTC time for DynamoDB SigV4");
        }
        std::array<char, 17> date_time{};
        std::array<char, 9> date{};
        if (std::strftime(date_time.data(), date_time.size(), "%Y%m%dT%H%M%SZ", &utc) == 0 ||
            std::strftime(date.data(), date.size(), "%Y%m%d", &utc) == 0) {
            throw std::runtime_error("failed to format UTC time for DynamoDB SigV4");
        }

        const std::string payload_hash = Sha256Hex(body);
        const std::string canonical_headers =
            "content-type:" + std::string(kContentType) + "\n" +
            "host:" + endpoint_.Authority() + "\n" +
            "x-amz-content-sha256:" + payload_hash + "\n" +
            "x-amz-date:" + date_time.data() + "\n" +
            "x-amz-target:" + target + "\n";
        const std::string signed_header_names =
            "content-type;host;x-amz-content-sha256;x-amz-date;x-amz-target";
        const std::string canonical_request =
            "POST\n/\n\n" + canonical_headers + "\n" + signed_header_names + "\n" + payload_hash;
        const std::string credential_scope =
            std::string(date.data()) + "/" + kRegion + "/" + kService + "/aws4_request";
        const std::string string_to_sign =
            "AWS4-HMAC-SHA256\n" + std::string(date_time.data()) + "\n" + credential_scope + "\n" +
            Sha256Hex(canonical_request);

        const auto date_key = HmacSha256("AWS4" + credentials_.secret_access_key, date.data());
        const auto region_key = HmacSha256(date_key, kRegion);
        const auto service_key = HmacSha256(region_key, kService);
        const auto signing_key = HmacSha256(service_key, "aws4_request");
        const auto signature = HmacSha256(signing_key, string_to_sign);
        const std::string authorization =
            "AWS4-HMAC-SHA256 Credential=" + credentials_.access_key_id + "/" + credential_scope +
            ", SignedHeaders=" + signed_header_names + ", Signature=" +
            Hex(signature.data(), signature.size());
        return {date_time.data(), payload_hash, authorization};
    }

    Url endpoint_;
    Credentials credentials_;
    std::string ca_certificate_path_;
};

void RequireSuccess(const HttpResponse& response, const std::string& operation) {
    if (response.status_code >= 200 && response.status_code < 300) {
        return;
    }
    throw std::runtime_error(
        "DynamoDB " + operation + " returned HTTP " + std::to_string(response.status_code) +
        ": " + AbbreviateBody(response.body));
}

std::vector<std::string> ListOwnedTables(
    const DynamoDbHttpClient& client,
    const std::string& prefix,
    Clock::time_point deadline) {
    std::vector<std::string> tables;
    std::optional<std::string> start_name;
    std::set<std::string> pagination_tokens;
    while (true) {
        const std::string request = start_name
            ? "{\"ExclusiveStartTableName\":" + JsonEscape(*start_name) + "}"
            : "{}";
        const auto response = client.Post("ListTables", request, deadline);
        RequireSuccess(response, "ListTables");
        auto page = ParseListTablesPage(response.body);
        for (auto& table_name : page.table_names) {
            if (table_name.compare(0, prefix.size(), prefix) == 0) {
                tables.push_back(std::move(table_name));
            }
        }
        if (!page.last_evaluated_table_name || page.last_evaluated_table_name->empty()) {
            break;
        }
        if (!pagination_tokens.insert(*page.last_evaluated_table_name).second) {
            throw std::runtime_error(
                "DynamoDB ListTables repeated pagination token " +
                JsonEscape(*page.last_evaluated_table_name));
        }
        start_name = std::move(page.last_evaluated_table_name);
    }
    return tables;
}

void DeleteTable(
    const DynamoDbHttpClient& client,
    const std::string& table_name,
    Clock::time_point deadline) {
    const auto response = client.Post(
        "DeleteTable",
        "{\"TableName\":" + JsonEscape(table_name) + "}",
        deadline);
    if (IsResourceNotFound(response)) {
        return;
    }
    RequireSuccess(response, "DeleteTable for " + JsonEscape(table_name));
}

void WaitForTableDeletion(
    const std::vector<DynamoDbHttpClient>& clients,
    const std::string& table_name,
    Clock::time_point deadline) {
    const std::string body = "{\"TableName\":" + JsonEscape(table_name) + "}";
    while (true) {
        bool absent_everywhere = true;
        for (const auto& client : clients) {
            const auto response = client.Post("DescribeTable", body, deadline);
            if (IsResourceNotFound(response)) {
                continue;
            }
            RequireSuccess(response, "DescribeTable for " + JsonEscape(table_name));
            absent_everywhere = false;
        }
        if (absent_everywhere) {
            return;
        }

        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) {
            throw std::runtime_error(
                "timed out waiting for DynamoDB table deletion: " + JsonEscape(table_name));
        }
        std::this_thread::sleep_for(std::min(remaining, Clock::duration(kPollInterval)));
    }
}

} // namespace

void CleanupDynamoDbTables(
    const AlternatorConnection& connection,
    const std::string& prefix,
    std::chrono::seconds timeout) {
    if (prefix.empty()) {
        throw std::invalid_argument("DynamoDB cleanup table prefix must not be empty");
    }
    if (timeout <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("DynamoDB cleanup timeout must be positive");
    }

    const auto deadline = DeadlineAfter(timeout);
    std::vector<DynamoDbHttpClient> clients;
    const auto& endpoints = connection.node_endpoints.empty()
        ? std::vector<Url>{connection.seed_endpoint}
        : connection.node_endpoints;
    clients.reserve(endpoints.size());
    for (const auto& endpoint : endpoints) {
        auto node_connection = connection;
        node_connection.seed_endpoint = endpoint;
        clients.emplace_back(node_connection);
    }
    const auto tables = ListOwnedTables(clients.front(), prefix, deadline);
    for (const auto& table_name : tables) {
        DeleteTable(clients.front(), table_name, deadline);
        WaitForTableDeletion(clients, table_name, deadline);
    }
}

} // namespace scylladb::alternator::testinfra
