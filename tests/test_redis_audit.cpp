// CRPC Redis -- boundary and error-path audit tests.
//
// These tests pin the fail-closed guarantees of the wire codecs on inputs a
// hostile peer could send or a caller could build:
//
//   * the data-type codec (`decode_value`) must reject a hostile container count
//     as `false` instead of throwing `std::bad_alloc` out of `reserve()`;
//   * the RESP encoder must reject a RESP2 fallback that has no length-prefixed
//     form when its payload carries CR/LF, because the emitted frame would be
//     two frames and silently desynchronise the stream.
//
// They live in a dedicated file so the audit additions never collide with the
// per-module test suites maintained alongside the implementation.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <crpc/redis/resp.hpp>
#include <crpc/redis/types.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::redis;

namespace {

// Calls `decode_value` and reports whether it returned false with `consumed`
// reset (the documented outcome for malformed input). A throw is a fail-closed
// violation: `decode_value` has no error channel other than the return value.
bool decode_rejected_without_throwing(const std::string& bytes) {
    value out;
    std::size_t consumed = 123;
    try {
        return !decode_value(bytes, out, consumed) && consumed == 0;
    } catch (...) {
        return false;
    }
}

} // namespace

CRPC_TEST(audit_decode_value_rejects_huge_array_count) {
    CRPC_CHECK(decode_rejected_without_throwing("*1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(audit_decode_value_rejects_huge_map_count) {
    CRPC_CHECK(decode_rejected_without_throwing("%1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(audit_decode_value_rejects_huge_set_count) {
    CRPC_CHECK(decode_rejected_without_throwing("~1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(audit_decode_value_rejects_huge_push_count) {
    CRPC_CHECK(decode_rejected_without_throwing(">1000000000000000000\r\n:1\r\n"));
}

// The reserve cap must not turn a legitimate large array into a failure.
CRPC_TEST(audit_decode_value_accepts_large_array_after_reserve_cap) {
    std::string bytes = "*5000\r\n";
    for (int i = 0; i < 5000; ++i) {
        bytes += ":1\r\n";
    }
    value out;
    std::size_t consumed = 0;
    CRPC_CHECK(decode_value(bytes, out, consumed));
    CRPC_CHECK_EQ(out.as_array().size(), std::size_t{5000});
    CRPC_CHECK_EQ(consumed, bytes.size());
}

CRPC_TEST(audit_resp2_blob_error_with_crlf_is_rejected) {
    const resp_value value{resp_blob_error{"a\r\nb"}};
    resp_encoder encoder(resp_protocol::resp2);

    bool threw = false;
    try {
        (void)encoder.encode_to_string(value);
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (...) {
        threw = false;
    }
    CRPC_CHECK(threw);

    std::size_t written = 99;
    std::vector<std::byte> buffer(64);
    const status encoded = encoder.encode(
        span<std::byte>(buffer.data(), buffer.size()), value, written);
    CRPC_CHECK_EQ(static_cast<int>(encoded.code()),
                  static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, std::size_t{0});
}

CRPC_TEST(audit_resp3_blob_error_with_crlf_is_length_prefixed) {
    const resp_value value{resp_blob_error{"a\r\nb"}};
    resp_encoder encoder(resp_protocol::resp3);
    const std::string encoded = encoder.encode_to_string(value);
    CRPC_CHECK_EQ(encoded, std::string("!4\r\na\r\nb\r\n"));
}

CRPC_TEST(audit_resp2_simple_string_and_error_crlf_are_rejected) {
    const resp_value simple{resp_simple_string{"a\r\nb"}};
    const resp_value error{resp_error{"a\r\nb"}};
    resp_encoder encoder(resp_protocol::resp2);

    bool threw = false;
    try {
        (void)encoder.encode_to_string(simple);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CRPC_CHECK(threw);

    threw = false;
    try {
        (void)encoder.encode_to_string(error);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CRPC_CHECK(threw);
}

// A blob error is binary safe: CR/LF survive a RESP3 round trip.
CRPC_TEST(audit_blob_error_round_trips_through_parser) {
    static const char wire[] = "!4\r\na\r\nb\r\n";
    resp_parser parser;
    parser.feed(span<const std::byte>(reinterpret_cast<const std::byte*>(wire),
                                       sizeof(wire) - 1));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::value_ready));
    CRPC_CHECK(value.is_blob_error());
    CRPC_CHECK_EQ(value.as_blob_error(), std::string("a\r\nb"));
}

// A RESP3 big number is a line form, so CR/LF in its payload is the same
// frame-desynchronising hazard as a RESP2 blob error.
CRPC_TEST(audit_resp3_big_number_with_crlf_is_rejected) {
    const resp_value value{resp_big_number{"123\r\n456"}};
    resp_encoder encoder(resp_protocol::resp3);

    bool threw = false;
    try {
        (void)encoder.encode_to_string(value);
    } catch (const std::invalid_argument&) {
        threw = true;
    } catch (...) {
        threw = false;
    }
    CRPC_CHECK(threw);

    std::size_t written = 99;
    std::vector<std::byte> buffer(64);
    const status encoded = encoder.encode(
        span<std::byte>(buffer.data(), buffer.size()), value, written);
    CRPC_CHECK_EQ(static_cast<int>(encoded.code()),
                  static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, std::size_t{0});
}

// RESP2 has no big-number marker: the fallback is a length-prefixed bulk, so
// CR/LF is ordinary data.
CRPC_TEST(audit_resp2_big_number_with_crlf_is_bulk_safe) {
    const resp_value value{resp_big_number{"123\r\n456"}};
    resp_encoder encoder(resp_protocol::resp2);
    const std::string encoded = encoder.encode_to_string(value);
    CRPC_CHECK_EQ(encoded, std::string("$8\r\n123\r\n456\r\n"));
}

CRPC_TEST(audit_resp3_big_number_without_crlf_round_trips) {
    const resp_value value{resp_big_number{"12345678901234567890"}};
    resp_encoder encoder(resp_protocol::resp3);
    const std::string encoded = encoder.encode_to_string(value);
    CRPC_CHECK_EQ(encoded, std::string("(12345678901234567890\r\n"));
}

CRPC_TEST_MAIN()
