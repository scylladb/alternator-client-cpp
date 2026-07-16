#include "http_compression.h"

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
#include <zlib.h>
#endif

#include <algorithm>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
#include <array>
#endif
#include <cctype>
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
#include <limits>
#endif
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace scylladb::alternator::detail {
namespace {

struct ContentEncodingDecoderEntry {
    std::string encoding;
    std::shared_ptr<HttpContentEncodingDecoder> decoder;
};

[[nodiscard]] std::string TrimAscii(std::string value) {
    const auto first = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    });
    const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base();

    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
[[nodiscard]] uInt CheckedZlibSize(std::size_t size) {
    if (size > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("HTTP content is too large for zlib");
    }
    return static_cast<uInt>(size);
}

void DeflateBody(std::istream& input, std::ostream& output, int window_bits) {
    z_stream stream{};
    const auto init_code = deflateInit2(
        &stream,
        Z_DEFAULT_COMPRESSION,
        Z_DEFLATED,
        window_bits,
        8,
        Z_DEFAULT_STRATEGY);
    if (init_code != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    struct DeflateGuard {
        z_stream* stream;
        ~DeflateGuard() {
            deflateEnd(stream);
        }
    } guard{&stream};

    std::array<char, 8192> input_buffer{};
    std::array<char, 8192> buffer{};
    while (true) {
        input.read(input_buffer.data(), static_cast<std::streamsize>(input_buffer.size()));
        const auto read_size = input.gcount();
        if (input.bad() || (input.fail() && !input.eof())) {
            throw std::runtime_error("failed to read HTTP request body");
        }

        stream.next_in = reinterpret_cast<Bytef*>(input_buffer.data());
        stream.avail_in = CheckedZlibSize(static_cast<std::size_t>(read_size));
        const auto flush = input.eof() ? Z_FINISH : Z_NO_FLUSH;

        do {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());

            const auto code = deflate(&stream, flush);
            const auto produced = buffer.size() - stream.avail_out;
            if (produced > 0) {
                output.write(buffer.data(), static_cast<std::streamsize>(produced));
                if (!output) {
                    throw std::runtime_error("failed to write compressed HTTP request body");
                }
            }

            if (code == Z_STREAM_END) {
                return;
            }
            if (code != Z_OK) {
                throw std::runtime_error("failed to deflate HTTP request");
            }
        } while (stream.avail_in != 0 || stream.avail_out == 0);

        if (flush == Z_FINISH) {
            throw std::runtime_error("failed to finish deflating HTTP request");
        }
    }
}

[[nodiscard]] std::string InflateBody(const std::string& body, int window_bits) {
    z_stream stream{};
    const auto init_code = inflateInit2(&stream, window_bits);
    if (init_code != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }

    struct InflateGuard {
        z_stream* stream;
        ~InflateGuard() {
            inflateEnd(stream);
        }
    } guard{&stream};

    auto* input = reinterpret_cast<const Bytef*>(body.data());
    stream.next_in = const_cast<Bytef*>(input);
    stream.avail_in = CheckedZlibSize(body.size());

    std::array<char, 8192> buffer{};
    std::string output;

    while (true) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());

        const auto code = inflate(&stream, Z_NO_FLUSH);
        const auto produced = buffer.size() - stream.avail_out;
        output.append(buffer.data(), produced);

        if (code == Z_STREAM_END) {
            return output;
        }
        if (code != Z_OK) {
            throw std::runtime_error("failed to inflate compressed HTTP response");
        }
        if (stream.avail_in == 0 && produced == 0) {
            throw std::runtime_error("truncated compressed HTTP response");
        }
    }
}

[[nodiscard]] std::string InflateDeflateBody(const std::string& body) {
    try {
        return InflateBody(body, MAX_WBITS);
    } catch (const std::runtime_error&) {
        return InflateBody(body, -MAX_WBITS);
    }
}
#endif

[[nodiscard]] std::vector<std::string> ParseContentEncodings(const std::string& content_encoding) {
    std::vector<std::string> encodings;
    std::size_t start = 0;
    while (start <= content_encoding.size()) {
        const auto comma = content_encoding.find(',', start);
        auto token = content_encoding.substr(
            start,
            comma == std::string::npos ? std::string::npos : comma - start);
        token = ToLowerAscii(TrimAscii(std::move(token)));
        if (!token.empty()) {
            encodings.push_back(std::move(token));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return encodings;
}

[[nodiscard]] std::string NormalizeRequestEncoding(
    const std::shared_ptr<HttpRequestCompressor>& request_compressor) {
    if (!request_compressor) {
        return {};
    }

    auto encoding = NormalizeResponseEncoding(request_compressor->ContentEncoding());
    if (encoding.empty()) {
        throw std::invalid_argument("request_compressor must not advertise an empty encoding");
    }
    if (encoding.find(',') != std::string::npos) {
        throw std::invalid_argument("request_compressor must advertise exactly one encoding");
    }
    return encoding;
}

[[nodiscard]] std::vector<ContentEncodingDecoderEntry> BuildDecoderEntries(
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders) {
    std::vector<ContentEncodingDecoderEntry> entries;
    for (const auto& decoder : content_encoding_decoders) {
        if (!decoder) {
            throw std::invalid_argument("content_encoding_decoders must not contain null values");
        }

        const auto accepted_encodings = decoder->AcceptedResponseEncodings();
        if (accepted_encodings.empty()) {
            throw std::invalid_argument("content_encoding_decoders must advertise at least one encoding");
        }

        for (const auto& encoding : accepted_encodings) {
            const auto normalized = NormalizeResponseEncoding(encoding);
            if (normalized.empty()) {
                throw std::invalid_argument("content_encoding_decoders must not advertise empty encodings");
            }
            const auto duplicate = std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.encoding == normalized;
            });
            if (duplicate) {
                throw std::invalid_argument(
                    "content_encoding_decoders must not advertise duplicate response encoding: " + normalized);
            }
            entries.push_back(ContentEncodingDecoderEntry{normalized, decoder});
        }
    }
    return entries;
}

[[nodiscard]] const ContentEncodingDecoderEntry* FindDecoderEntry(
    const std::vector<ContentEncodingDecoderEntry>& entries,
    const std::string& encoding) {
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.encoding == encoding;
    });
    return it == entries.end() ? nullptr : &*it;
}

[[nodiscard]] std::vector<std::string> NormalizeZlibResponseEncodings(std::vector<std::string> encodings) {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    for (auto& encoding : encodings) {
        encoding = NormalizeResponseEncoding(std::move(encoding));
        if (encoding != "gzip" && encoding != "deflate") {
            throw std::invalid_argument("ZlibContentEncodingDecoder supports only gzip and deflate");
        }
    }
    return encodings;
#else
    (void)encodings;
    throw std::invalid_argument("zlib response decoding is not available");
#endif
}

} // namespace

std::string BuildRequestContentEncodingValue(
    const std::shared_ptr<HttpRequestCompressor>& request_compressor) {
    return NormalizeRequestEncoding(request_compressor);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string NormalizeResponseEncoding(std::string encoding) {
    return ToLowerAscii(TrimAscii(std::move(encoding)));
}

std::string BuildAcceptEncodingValue(
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders) {
    std::string value;
    for (const auto& entry : BuildDecoderEntries(content_encoding_decoders)) {
        if (!value.empty()) {
            value += ", ";
        }
        value += entry.encoding;
    }
    return value;
}

std::string FindHttpHeaderValue(const std::string& headers, const std::string& name) {
    const auto wanted = ToLowerAscii(name);
    std::string value;

    std::istringstream lines(headers);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("HTTP/", 0) == 0) {
            value.clear();
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        auto header_name = line.substr(0, colon);
        header_name = ToLowerAscii(TrimAscii(std::move(header_name)));
        if (header_name == wanted) {
            value = TrimAscii(line.substr(colon + 1));
        }
    }

    return value;
}

} // namespace scylladb::alternator::detail

namespace scylladb::alternator {

GzipRequestCompressor::GzipRequestCompressor(std::uint64_t min_size_bytes)
    : min_size_bytes_(min_size_bytes) {
#if !SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    throw std::invalid_argument("zlib request encoding is not available");
#endif
}

std::string GzipRequestCompressor::ContentEncoding() const {
    return "gzip";
}

bool GzipRequestCompressor::Compress(
    std::istream& input,
    std::uint64_t input_size,
    std::ostream& output) const {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    if (input_size < min_size_bytes_) {
        return false;
    }
    detail::DeflateBody(input, output, MAX_WBITS + 16);
    return true;
#else
    (void)input;
    (void)input_size;
    (void)output;
    throw std::runtime_error("zlib request encoding is not available");
#endif
}

ZlibContentEncodingDecoder::ZlibContentEncodingDecoder(std::vector<std::string> accepted_response_encodings)
    : accepted_response_encodings_(detail::NormalizeZlibResponseEncodings(std::move(accepted_response_encodings))) {}

std::vector<std::string> ZlibContentEncodingDecoder::AcceptedResponseEncodings() const {
    return accepted_response_encodings_;
}

std::string ZlibContentEncodingDecoder::Decode(std::string body, const std::string& content_encoding) const {
#if SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB
    const auto normalized = detail::NormalizeResponseEncoding(content_encoding);
    if (normalized == "gzip") {
        return detail::InflateBody(body, MAX_WBITS + 16);
    }
    if (normalized == "deflate") {
        return detail::InflateDeflateBody(body);
    }
    throw std::runtime_error("unsupported HTTP content encoding: " + normalized);
#else
    (void)body;
    (void)content_encoding;
    throw std::runtime_error("zlib response decoding is not available");
#endif
}

} // namespace scylladb::alternator

namespace scylladb::alternator::detail {

std::string DecodeHttpResponseBody(
    std::string body,
    const std::string& content_encoding,
    const std::vector<std::shared_ptr<HttpContentEncodingDecoder>>& content_encoding_decoders) {
    auto encodings = ParseContentEncodings(content_encoding);
    if (encodings.empty()) {
        return body;
    }

    const auto decoder_entries = BuildDecoderEntries(content_encoding_decoders);
    for (auto it = encodings.rbegin(); it != encodings.rend(); ++it) {
        if (*it == "identity") {
            continue;
        }
        const auto* decoder_entry = FindDecoderEntry(decoder_entries, *it);
        if (decoder_entry == nullptr) {
            throw std::runtime_error("unexpected HTTP content encoding: " + *it);
        }
        body = decoder_entry->decoder->Decode(std::move(body), *it);
    }

    return body;
}

} // namespace scylladb::alternator::detail
