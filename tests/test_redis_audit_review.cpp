// CRPC Redis -- independent second-auditor review probes.
//
// These probes re-derive the fail-closed guarantees from scratch rather than
// trusting the first auditor's regression file:
//
//   * `decode_value` must reject a hostile container count / deep nesting as
//     `false` without throwing;
//   * the RESP encoder must refuse to emit a line-form payload that carries
//     CR/LF, because the emitted bytes would be read back as two frames.
//
// The last case is the one the first audit did not cover: a RESP3 big number is
// a line form (`(` + text + CRLF) exactly like a RESP2 blob error, so a payload
// carrying CR/LF must be refused for the same reason.
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

bool decode_rejected_without_throwing(const std::string& bytes) {
    value out;
    std::size_t consumed = 123;
    try {
        return !decode_value(bytes, out, consumed) && consumed == 0;
    } catch (...) {
        return false;
    }
}

bool encode_to_string_throws_invalid_argument(const resp_value& value,
                                              resp_protocol protocol) {
    resp_encoder encoder(protocol);
    try {
        (void)encoder.encode_to_string(value);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

// --- D1: hostile container counts must fail closed --------------------------

CRPC_TEST(review_decode_rejects_huge_array_count) {
    CRPC_CHECK(decode_rejected_without_throwing("*1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(review_decode_rejects_huge_map_count) {
    CRPC_CHECK(decode_rejected_without_throwing("%1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(review_decode_rejects_huge_set_count) {
    CRPC_CHECK(decode_rejected_without_throwing("~1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(review_decode_rejects_huge_push_count) {
    CRPC_CHECK(decode_rejected_without_throwing(">1000000000000000000\r\n:1\r\n"));
}

CRPC_TEST(review_decode_rejects_deep_nesting) {
    std::string bytes;
    for (int i = 0; i < 200; ++i) {
        bytes += "*1\r\n";
    }
    bytes += ":1\r\n";
    CRPC_CHECK(decode_rejected_without_throwing(bytes));
}

CRPC_TEST(review_decode_rejects_negative_count_other_than_minus_one) {
    CRPC_CHECK(decode_rejected_without_throwing("*-2\r\n:1\r\n"));
    CRPC_CHECK(decode_rejected_without_throwing("%-2\r\n:1\r\n"));
}

// A count just past the reserve cap but with the real elements present must
// still decode: the cap is an allocation optimisation, not a validity limit.
CRPC_TEST(review_decode_accepts_large_real_array) {
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

// --- D2: line-form payloads carrying CR/LF must be refused ------------------

CRPC_TEST(review_resp2_blob_error_crlf_rejected) {
    CRPC_CHECK(encode_to_string_throws_invalid_argument(
        resp_value{resp_blob_error{"a\r\nb"}}, resp_protocol::resp2));
}

CRPC_TEST(review_resp3_blob_error_crlf_is_length_prefixed) {
    resp_encoder encoder(resp_protocol::resp3);
    const std::string encoded =
        encoder.encode_to_string(resp_value{resp_blob_error{"a\r\nb"}});
    CRPC_CHECK_EQ(encoded, std::string("!4\r\na\r\nb\r\n"));
}

CRPC_TEST(review_resp2_blob_error_crlf_rejected_when_nested) {
    resp_array items;
    items.emplace_back(resp_blob_error{"a\r\nb"});
    CRPC_CHECK(encode_to_string_throws_invalid_argument(
        resp_value{resp_array_value{std::move(items)}}, resp_protocol::resp2));
}

CRPC_TEST(review_resp2_simple_string_and_error_crlf_rejected) {
    CRPC_CHECK(encode_to_string_throws_invalid_argument(
        resp_value{resp_simple_string{"a\r\nb"}}, resp_protocol::resp2));
    CRPC_CHECK(encode_to_string_throws_invalid_argument(
        resp_value{resp_error{"a\r\nb"}}, resp_protocol::resp2));
}

// A RESP3 big number is a line form, so CR/LF in its payload is the same
// frame-desynchronising hazard as a RESP2 blob error.
CRPC_TEST(review_resp3_big_number_crlf_rejected) {
    CRPC_CHECK(encode_to_string_throws_invalid_argument(
        resp_value{resp_big_number{"123\r\n456"}}, resp_protocol::resp3));
}

// A RESP3 big number is a line form (`(` + text + CRLF). Before the fix the
// encoder emitted "(" + "123" + CRLF + "456" + CRLF, which the parser read as a
// big number "123" followed by an unknown marker. The encoder must now refuse
// the payload so that no malformed frame is produced.
// (Updated from the pre-fix bug demonstration to the fixed-behaviour
// regression by the first auditor, as requested by the reviewer so the review
// binary can go green.)
CRPC_TEST(review_resp3_big_number_crlf_refused_no_desync) {
    const resp_value value{resp_big_number{"123\r\n456"}};
    CRPC_CHECK(
        encode_to_string_throws_invalid_argument(value, resp_protocol::resp3));

    resp_encoder encoder(resp_protocol::resp3);
    std::size_t written = 99;
    std::vector<std::byte> buffer(64);
    const status encoded = encoder.encode(
        span<std::byte>(buffer.data(), buffer.size()), value, written);
    CRPC_CHECK_EQ(static_cast<int>(encoded.code()),
                  static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, std::size_t{0});
}

// A RESP3 verbatim string is length-prefixed, so CR/LF is ordinary data.
CRPC_TEST(review_resp3_verbatim_crlf_is_length_prefixed) {
    resp_encoder encoder(resp_protocol::resp3);
    const std::string encoded = encoder.encode_to_string(
        resp_value{resp_verbatim_string{"a\r\nb", "txt"}});
    CRPC_CHECK_EQ(encoded, std::string("=8\r\ntxt:a\r\nb\r\n"));
}

CRPC_TEST_MAIN()
