// CRPC -- Content-Encoding negotiation and gzip/deflate tests.
//
// The gzip/deflate vectors below were produced by zlib (Python `zlib.compress`)
// and are embedded verbatim, so the decoder is checked against real, dynamic
// Huffman output rather than against our own encoder.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/http/content_coding.hpp>

#include "test_framework.hpp"

namespace {

using crpc::http::coding_limits;
using crpc::http::content_coding;
using crpc::http::content_decoder;
using crpc::http::content_encoder;
using crpc::http::negotiate_response;
using crpc::http::negotiation;
using crpc::http::plan_request_decoding;
using crpc::http::request_encoding_plan;
using crpc::rpc_code;
using crpc::span;
using crpc::status;

span<const std::byte> as_span(const std::string& text) {
    return span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

span<const std::byte> as_span(const unsigned char* data, std::size_t size) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
}

const std::string k_hello = "hello, crpc gzip round-trip! 0123456789";

std::string make_text() {
    std::string text;
    for (int i = 0; i < 100; ++i) {
        text += "The quick brown fox jumps over the lazy dog. ";
    }
    return text;
}

// zlib: gzip(39 bytes) of k_hello
static const unsigned char k_gzip_hello[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0xcb, 0x48,
    0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x48, 0x2e, 0x2a, 0x48, 0x56, 0x48, 0xaf,
    0xca, 0x2c, 0x50, 0x28, 0xca, 0x2f, 0xcd, 0x4b, 0xd1, 0x2d, 0x29, 0xca,
    0x2c, 0x50, 0x54, 0x30, 0x30, 0x34, 0x32, 0x36, 0x31, 0x35, 0x33, 0xb7,
    0xb0, 0x04, 0x00, 0x57, 0x99, 0xa3, 0x87, 0x27, 0x00, 0x00, 0x00};
static const std::size_t k_gzip_hello_len = sizeof(k_gzip_hello);

// zlib: zlib-wrapped deflate of k_hello
static const unsigned char k_deflate_hello[] = {
    0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x48, 0x2e, 0x2a,
    0x48, 0x56, 0x48, 0xaf, 0xca, 0x2c, 0x50, 0x28, 0xca, 0x2f, 0xcd, 0x4b,
    0xd1, 0x2d, 0x29, 0xca, 0x2c, 0x50, 0x54, 0x30, 0x30, 0x34, 0x32, 0x36,
    0x31, 0x35, 0x33, 0xb7, 0xb0, 0x04, 0x00, 0x11, 0x5b, 0x0c, 0x65};
static const std::size_t k_deflate_hello_len = sizeof(k_deflate_hello);

// zlib: gzip of make_text() (4500 bytes)
static const unsigned char k_gzip_text[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0xed, 0xca,
    0x47, 0x01, 0x80, 0x30, 0x10, 0x45, 0x41, 0x2b, 0x5f, 0x01, 0x6a, 0x62,
    0x80, 0x92, 0xd0, 0xd9, 0x10, 0x08, 0x4d, 0x3d, 0x88, 0xe0, 0xf8, 0xce,
    0x33, 0xae, 0xf3, 0x5a, 0x73, 0x5f, 0x8f, 0xaa, 0x92, 0x9d, 0x8b, 0x82,
    0x5d, 0x1a, 0xf2, 0x1c, 0x37, 0xd9, 0xe1, 0x93, 0xf6, 0x8f, 0xa7, 0xf2,
    0xb9, 0xd5, 0x58, 0x5b, 0xc8, 0x91, 0xc9, 0x64, 0x32, 0x99, 0x4c, 0x26,
    0x93, 0xc9, 0x64, 0x32, 0x99, 0x4c, 0x26, 0x93, 0xff, 0xc8, 0x2f, 0x38,
    0x39, 0x8b, 0x9b, 0x94, 0x11, 0x00, 0x00};
static const std::size_t k_gzip_text_len = sizeof(k_gzip_text);

// zlib: gzip of 400000 zero bytes (ratio 948:1)
static const unsigned char k_gzip_zeros[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0a, 0xed, 0xc1,
    0x31, 0x01, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0xf5, 0x4f, 0x6d, 0x0a, 0x3f,
    0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe0, 0x6f, 0x71, 0x3d, 0x59, 0x5e, 0x80, 0x1a,
    0x06, 0x00};
static const std::size_t k_gzip_zeros_len = sizeof(k_gzip_zeros);

} // namespace

CRPC_TEST(content_coding_negotiation) {
    negotiation gzip_then_deflate =
        negotiate_response("gzip;q=0.5, deflate;q=0.8");
    CRPC_CHECK(content_coding::deflate == gzip_then_deflate.coding);
    CRPC_CHECK(gzip_then_deflate.compressed);

    negotiation prefer_gzip = negotiate_response("gzip;q=0.5, deflate;q=0.4");
    CRPC_CHECK(content_coding::gzip == prefer_gzip.coding);

    negotiation identity_only = negotiate_response("identity");
    CRPC_CHECK(content_coding::identity == identity_only.coding);
    CRPC_CHECK(identity_only.acceptable);

    negotiation unspecified = negotiate_response("");
    CRPC_CHECK(content_coding::identity == unspecified.coding);

    negotiation unknown = negotiate_response("br");
    CRPC_CHECK(content_coding::identity == unknown.coding);
    CRPC_CHECK(unknown.acceptable);

    negotiation none = negotiate_response("identity;q=0, gzip;q=0, deflate;q=0");
    CRPC_CHECK_FALSE(none.acceptable);
    CRPC_CHECK_EQ(406, crpc::http::negotiation_http_status(none));

    coding_limits fallback;
    fallback.fallback_identity = true;
    negotiation with_fallback = negotiate_response(
        "identity;q=0, gzip;q=0, deflate;q=0", fallback);
    CRPC_CHECK(with_fallback.acceptable);
    CRPC_CHECK(with_fallback.fallback_used);
    CRPC_CHECK(content_coding::identity == with_fallback.coding);

    negotiation star_excluded = negotiate_response("*;q=0, identity;q=0");
    CRPC_CHECK_FALSE(star_excluded.acceptable);

    negotiation wildcard = negotiate_response("*");
    CRPC_CHECK(content_coding::gzip == wildcard.coding);

    negotiation wildcard_half = negotiate_response("deflate;q=0, *;q=0.5");
    CRPC_CHECK(content_coding::gzip == wildcard_half.coding);

    CRPC_CHECK(crpc::http::should_compress(prefer_gzip, 1024));
    CRPC_CHECK_FALSE(crpc::http::should_compress(prefer_gzip, 100));
    CRPC_CHECK_FALSE(crpc::http::should_compress(unspecified, 4096));
}

CRPC_TEST(content_coding_request_plan) {
    request_encoding_plan none = plan_request_decoding("");
    CRPC_CHECK(none.ok);
    CRPC_CHECK_FALSE(none.encoded);
    CRPC_CHECK_EQ(200, none.http_status);

    request_encoding_plan gzip = plan_request_decoding("gzip");
    CRPC_CHECK(gzip.ok);
    CRPC_CHECK(gzip.encoded);
    CRPC_CHECK(content_coding::gzip == gzip.coding);

    request_encoding_plan deflate = plan_request_decoding("deflate");
    CRPC_CHECK(deflate.ok);
    CRPC_CHECK(content_coding::deflate == deflate.coding);

    request_encoding_plan identity = plan_request_decoding("identity");
    CRPC_CHECK(identity.ok);
    CRPC_CHECK_FALSE(identity.encoded);

    request_encoding_plan unsupported = plan_request_decoding("br");
    CRPC_CHECK_FALSE(unsupported.ok);
    CRPC_CHECK_EQ(415, unsupported.http_status);

    request_encoding_plan layered = plan_request_decoding("gzip, deflate");
    CRPC_CHECK_FALSE(layered.ok);
    CRPC_CHECK_EQ(415, layered.http_status);

    request_encoding_plan parameter = plan_request_decoding("gzip;q=1");
    CRPC_CHECK_FALSE(parameter.ok);
    CRPC_CHECK_EQ(415, parameter.http_status);
}

CRPC_TEST(content_coding_gzip_round_trip) {
    const std::string text = make_text();
    content_encoder encoder(content_coding::gzip);
    std::string encoded;
    CRPC_CHECK(encoder.start(encoded).ok());
    CRPC_CHECK(encoder.write(as_span(text.substr(0, 1000)), encoded).ok());
    CRPC_CHECK(encoder.write(as_span(text.substr(1000)), encoded).ok());
    CRPC_CHECK(encoder.finish(encoded).ok());
    CRPC_CHECK(encoded.size() >= 2);
    CRPC_CHECK_EQ(0x1f, static_cast<int>(static_cast<unsigned char>(encoded[0])));
    CRPC_CHECK_EQ(0x8b, static_cast<int>(static_cast<unsigned char>(encoded[1])));

    content_decoder decoder(content_coding::gzip);
    std::string decoded;
    for (char c : encoded) {
        CRPC_CHECK(decoder
                       .feed(span<const std::byte>(
                                 reinterpret_cast<const std::byte*>(&c), 1),
                             decoded)
                       .ok());
    }
    CRPC_CHECK(decoder.finish(decoded).ok());
    CRPC_CHECK_EQ(text, decoded);
}

CRPC_TEST(content_coding_deflate_round_trip) {
    const std::string text = "deflate round trip payload";
    std::string encoded;
    CRPC_CHECK(crpc::http::compress_all(content_coding::deflate, as_span(text),
                                        encoded)
                   .ok());
    CRPC_CHECK(encoded.size() >= 2);
    auto decoded =
        crpc::http::decompress_all(content_coding::deflate, as_span(encoded));
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(text, decoded.value());
}

CRPC_TEST(content_coding_identity_round_trip) {
    content_encoder encoder(content_coding::identity);
    std::string encoded;
    CRPC_CHECK(encoder.start(encoded).ok());
    CRPC_CHECK(encoder.write(as_span("plain"), encoded).ok());
    CRPC_CHECK(encoder.finish(encoded).ok());
    CRPC_CHECK_EQ(std::string("plain"), encoded);

    content_decoder decoder(content_coding::identity);
    std::string decoded;
    CRPC_CHECK(decoder.feed(as_span(encoded), decoded).ok());
    CRPC_CHECK(decoder.finish(decoded).ok());
    CRPC_CHECK_EQ(std::string("plain"), decoded);
}

CRPC_TEST(content_coding_decodes_zlib_vectors) {
    auto gzip_hello =
        crpc::http::decompress_all(content_coding::gzip,
                                   as_span(k_gzip_hello, k_gzip_hello_len));
    CRPC_CHECK(gzip_hello.ok());
    CRPC_CHECK_EQ(k_hello, gzip_hello.value());

    auto deflate_hello = crpc::http::decompress_all(
        content_coding::deflate, as_span(k_deflate_hello, k_deflate_hello_len));
    CRPC_CHECK(deflate_hello.ok());
    CRPC_CHECK_EQ(k_hello, deflate_hello.value());

    auto gzip_text = crpc::http::decompress_all(
        content_coding::gzip, as_span(k_gzip_text, k_gzip_text_len));
    CRPC_CHECK(gzip_text.ok());
    CRPC_CHECK_EQ(make_text(), gzip_text.value());
}

CRPC_TEST(content_coding_streams_zlib_vector) {
    content_decoder decoder(content_coding::gzip);
    std::string decoded;
    for (std::size_t i = 0; i < k_gzip_text_len; ++i) {
        CRPC_CHECK(decoder.feed(as_span(k_gzip_text + i, 1), decoded).ok());
    }
    CRPC_CHECK(decoder.finish(decoded).ok());
    CRPC_CHECK_EQ(make_text(), decoded);
}

CRPC_TEST(content_coding_encoder_flushes_per_write) {
    content_encoder encoder(content_coding::gzip);
    std::string encoded;
    CRPC_CHECK(encoder.start(encoded).ok());
    CRPC_CHECK(encoder.write(as_span("part1"), encoded).ok());
    const std::size_t flushed = encoded.size();

    content_decoder decoder(content_coding::gzip);
    std::string decoded;
    CRPC_CHECK(decoder.feed(as_span(encoded), decoded).ok());
    CRPC_CHECK_EQ(std::string("part1"), decoded);

    std::string tail;
    CRPC_CHECK(encoder.write(as_span("part2"), tail).ok());
    CRPC_CHECK(encoder.finish(tail).ok());
    CRPC_CHECK(decoder.feed(as_span(tail), decoded).ok());
    CRPC_CHECK(decoder.finish(decoded).ok());
    CRPC_CHECK_EQ(std::string("part1part2"), decoded);
    CRPC_CHECK(flushed > 10);
}

CRPC_TEST(content_coding_ratio_bomb_rejected) {
    content_decoder decoder(content_coding::gzip);
    std::string out;
    status st =
        decoder.feed(as_span(k_gzip_zeros, k_gzip_zeros_len), out);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::resource_exhausted == st.code());
    CRPC_CHECK_FALSE(decoder.finish(out).ok());
}

CRPC_TEST(content_coding_size_bomb_rejected) {
    coding_limits limits;
    limits.max_compression_ratio = 0;
    limits.max_decompressed_bytes = 1000;
    content_decoder decoder(content_coding::gzip, limits);
    std::string out;
    status st = decoder.feed(as_span(k_gzip_zeros, k_gzip_zeros_len), out);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::resource_exhausted == st.code());
}

CRPC_TEST(content_coding_large_input_allowed_without_ratio) {
    coding_limits limits;
    limits.max_compression_ratio = 0;
    limits.max_decompressed_bytes = 1u * 1024u * 1024u;
    auto decoded = crpc::http::decompress_all(
        content_coding::gzip, as_span(k_gzip_zeros, k_gzip_zeros_len), limits);
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(std::size_t{400000}, decoded.value().size());
    bool all_zero = true;
    for (char c : decoded.value()) {
        if (c != '\0') {
            all_zero = false;
            break;
        }
    }
    CRPC_CHECK(all_zero);
}

CRPC_TEST(content_coding_input_bomb_rejected) {
    coding_limits limits;
    limits.max_input_bytes = 10;
    content_decoder decoder(content_coding::gzip, limits);
    std::string out;
    status st =
        decoder.feed(as_span(k_gzip_hello, k_gzip_hello_len), out);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::resource_exhausted == st.code());
}

CRPC_TEST(content_coding_truncated_rejected) {
    content_decoder decoder(content_coding::gzip);
    std::string out;
    CRPC_CHECK(decoder.feed(as_span(k_gzip_hello, 20), out).ok());
    status st = decoder.finish(out);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(rpc_code::decode_error == st.code());
}

CRPC_TEST(content_coding_bad_checksum_rejected) {
    std::string corrupted(reinterpret_cast<const char*>(k_gzip_hello),
                          k_gzip_hello_len);
    corrupted[corrupted.size() - 5] =
        static_cast<char>(corrupted[corrupted.size() - 5] + 1);
    auto decoded =
        crpc::http::decompress_all(content_coding::gzip, as_span(corrupted));
    CRPC_CHECK_FALSE(decoded.ok());
    CRPC_CHECK(rpc_code::decode_error == decoded.error().code);
}

CRPC_TEST(content_coding_trailing_data_rejected) {
    std::string with_tail(reinterpret_cast<const char*>(k_gzip_hello),
                          k_gzip_hello_len);
    with_tail.push_back('\x00');
    auto decoded =
        crpc::http::decompress_all(content_coding::gzip, as_span(with_tail));
    CRPC_CHECK_FALSE(decoded.ok());
    CRPC_CHECK(rpc_code::protocol_error == decoded.error().code);
}

CRPC_TEST(content_coding_invalid_header_rejected) {
    auto decoded = crpc::http::decompress_all(
        content_coding::gzip, as_span(std::string("GET / HTTP/1.1\r\n\r\n")));
    CRPC_CHECK_FALSE(decoded.ok());
    CRPC_CHECK(rpc_code::protocol_error == decoded.error().code);
}

CRPC_TEST(content_coding_token_helpers) {
    content_coding coding = content_coding::identity;
    CRPC_CHECK(crpc::http::content_coding_from_token("GZIP", coding));
    CRPC_CHECK(content_coding::gzip == coding);
    CRPC_CHECK(crpc::http::content_coding_from_token("deflate", coding));
    CRPC_CHECK(content_coding::deflate == coding);
    CRPC_CHECK(crpc::http::content_coding_from_token("identity", coding));
    CRPC_CHECK_FALSE(crpc::http::content_coding_from_token("br", coding));
    CRPC_CHECK_EQ(std::string("gzip"),
                  std::string(crpc::http::to_string(content_coding::gzip)));
}

// The encoder must run LZ77 matching, not fixed-Huffman literals only: a
// highly repetitive payload has to shrink well below its identity size while
// remaining a valid gzip stream.
CRPC_TEST(content_coding_lz77_repetitive_ratio) {
    std::string text;
    for (int i = 0; i < 5000; ++i) {
        text += "The quick brown fox jumps over the lazy dog. ";
    }
    std::string encoded;
    CRPC_CHECK(crpc::http::compress_all(content_coding::gzip, as_span(text),
                                        encoded)
                   .ok());
    CRPC_CHECK(encoded.size() < text.size() / 4);
    // LZ77 makes this far more compressible than the 100:1 bomb ceiling; the
    // ratio guard is disabled explicitly (it exists to bound hostile inputs,
    // not legitimate repetitive payloads).
    coding_limits limits;
    limits.max_compression_ratio = 0;
    auto decoded = crpc::http::decompress_all(content_coding::gzip,
                                              as_span(encoded), limits);
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(text, decoded.value());
}

// A match may reference bytes written by an earlier `write` call; the history
// ring and hash chain must persist across blocks.
CRPC_TEST(content_coding_lz77_match_crosses_write_boundary) {
    const std::string first =
        "abcdefghijklmnopqrstuvwxyz0123456789 repeated repeated repeated";
    const std::string second = first + " tail tail tail";
    content_encoder encoder(content_coding::gzip);
    std::string encoded;
    CRPC_CHECK(encoder.start(encoded).ok());
    CRPC_CHECK(encoder.write(as_span(first), encoded).ok());
    CRPC_CHECK(encoder.write(as_span(second), encoded).ok());
    CRPC_CHECK(encoder.finish(encoded).ok());

    auto decoded =
        crpc::http::decompress_all(content_coding::gzip, as_span(encoded));
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(first + second, decoded.value());
    // The second half repeats the first: the match surely crosses the boundary.
    CRPC_CHECK(encoded.size() < (first.size() + second.size()));
}

namespace {

// Encodes `data` split at `chunks` boundaries and requires an exact round trip.
void check_split_round_trip(content_coding coding, const std::string& data,
                            const std::vector<std::size_t>& cuts) {
    content_encoder encoder(coding);
    std::string encoded;
    CRPC_CHECK(encoder.start(encoded).ok());
    std::size_t begin = 0;
    for (const std::size_t cut : cuts) {
        if (cut < begin || cut > data.size()) {
            continue;
        }
        CRPC_CHECK(encoder
                       .write(span<const std::byte>(
                                  reinterpret_cast<const std::byte*>(data.data() +
                                                                     begin),
                                  cut - begin),
                              encoded)
                       .ok());
        begin = cut;
    }
    CRPC_CHECK(encoder
                   .write(span<const std::byte>(
                              reinterpret_cast<const std::byte*>(data.data() +
                                                                 begin),
                              data.size() - begin),
                          encoded)
                   .ok());
    CRPC_CHECK(encoder.finish(encoded).ok());

    coding_limits limits;
    limits.max_compression_ratio = 0;
    limits.max_decompressed_bytes = 4u * 1024u * 1024u;
    auto decoded = crpc::http::decompress_all(coding, as_span(encoded), limits);
    CRPC_CHECK(decoded.ok());
    if (decoded.ok()) {
        CRPC_CHECK_EQ(data.size(), decoded.value().size());
        CRPC_CHECK_EQ(data, decoded.value());
    }
}

std::uint32_t next_random(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// The exact shape reported by the independent audit: the second write starts
// with a run that matches the first write, so a match extension crosses the
// write boundary into bytes whose ring slots were never written.
CRPC_TEST(content_coding_two_write_pattern_round_trip) {
    std::string first(98, 'B');
    first.push_back('A');
    first.push_back('B');
    std::string second(98, 'B');
    second.push_back('\0');
    second.push_back('\0');
    for (const content_coding coding : {content_coding::gzip,
                                        content_coding::deflate}) {
        content_encoder encoder(coding);
        std::string encoded;
        CRPC_CHECK(encoder.start(encoded).ok());
        CRPC_CHECK(encoder.write(as_span(first), encoded).ok());
        CRPC_CHECK(encoder.write(as_span(second), encoded).ok());
        CRPC_CHECK(encoder.finish(encoded).ok());

        coding_limits limits;
        limits.max_compression_ratio = 0;
        auto decoded =
            crpc::http::decompress_all(coding, as_span(encoded), limits);
        CRPC_CHECK(decoded.ok());
        if (decoded.ok()) {
            CRPC_CHECK_EQ(first + second, decoded.value());
        }
    }
}

// Audit B's minimal reproducer: the stale boundary bytes happen to equal the
// continuation bytes, so a wrong match is only caught by the CRC.
CRPC_TEST(content_coding_minimal_two_write_boundary) {
    std::string first = "ABC";
    first.push_back('\x42');
    first.push_back('\x43');
    std::string second = "ABC";
    second.push_back('\0');
    second.push_back('\0');
    second += "XYZ";
    content_encoder encoder(content_coding::gzip);
    std::string encoded;
    CRPC_CHECK(encoder.start(encoded).ok());
    CRPC_CHECK(encoder.write(as_span(first), encoded).ok());
    CRPC_CHECK(encoder.write(as_span(second), encoded).ok());
    CRPC_CHECK(encoder.finish(encoded).ok());
    auto decoded =
        crpc::http::decompress_all(content_coding::gzip, as_span(encoded));
    CRPC_CHECK(decoded.ok());
    if (decoded.ok()) {
        CRPC_CHECK_EQ(first + second, decoded.value());
    }
}

// Deterministic random splits (1..~300 byte chunks) over mixed payloads. This
// exercises every tail position of a write, not just the two-byte boundary.
CRPC_TEST(content_coding_split_fuzz_round_trip) {
    std::uint32_t state = 0x12345678u;
    for (int round = 0; round < 200; ++round) {
        std::string data;
        const std::size_t size =
            1 + static_cast<std::size_t>(next_random(state) % 5000u);
        data.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            const std::uint32_t value = next_random(state);
            if ((value & 3u) == 0u && !data.empty()) {
                // Repeat a recent byte to create matches.
                data.push_back(data[data.size() - 1 -
                                    (value % (data.size() > 64 ? 64 : data.size()))]);
            } else {
                data.push_back(static_cast<char>(value & 0xffu));
            }
        }
        std::vector<std::size_t> cuts;
        std::size_t position = 0;
        while (position < data.size()) {
            position += 1 + (next_random(state) % 300u);
            if (position >= data.size()) {
                break;
            }
            cuts.push_back(position);
        }
        for (const content_coding coding : {content_coding::gzip,
                                            content_coding::deflate}) {
            check_split_round_trip(coding, data, cuts);
        }
    }
}

// More than one 32 KiB window of input split across writes: the ring wraps and
// old slots are reused by newer positions.
CRPC_TEST(content_coding_multi_write_window_wrap) {
    std::uint32_t state = 0x9e3779b9u;
    std::string data;
    data.reserve(100000);
    for (std::size_t i = 0; i < 100000; ++i) {
        const std::uint32_t value = next_random(state);
        if ((value & 7u) == 0u && !data.empty()) {
            const std::size_t back =
                static_cast<std::size_t>(value % 1000u) % data.size();
            data.push_back(data[data.size() - 1 - back]);
        } else {
            data.push_back(static_cast<char>('a' + (value % 26u)));
        }
    }
    std::vector<std::size_t> cuts;
    for (std::size_t position = 777; position < data.size(); position += 777) {
        cuts.push_back(position);
    }
    for (const content_coding coding : {content_coding::gzip,
                                        content_coding::deflate}) {
        check_split_round_trip(coding, data, cuts);
    }
}

} // namespace

CRPC_TEST_MAIN()
