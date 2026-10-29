// CRPC -- tests for the Redis RESP protocol codec.
//
// Coverage targets: golden bytes for every RESP2/RESP3 marker, the value model
// accessors, streaming at every split point and one byte at a time, several
// values per feed, the end-of-stream check, fail-closed rejection of hostile
// input (unknown marker, malformed length, missing CRLF, bad boolean, bad
// double, negative count, size/depth limits), reset after error, size
// computation against the actual write, and deterministic mutation loops that
// assert the parser never crashes on arbitrary bytes.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <crpc/redis/resp.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::redis;

namespace {

std::vector<std::byte> bytes_of(const std::string& text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

span<const std::byte> as_span(const std::vector<std::byte>& bytes) {
    return span<const std::byte>(bytes.data(), bytes.size());
}

span<std::byte> as_writable_span(std::vector<std::byte>& bytes) {
    return span<std::byte>(bytes.data(), bytes.size());
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

resp_value parse_one(const std::string& text) {
    resp_parser parser;
    parser.feed(as_span(bytes_of(text)));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::value_ready));
    CRPC_CHECK(parser.finish().ok());
    return value;
}

std::string encode_value_text(const resp_value& value, resp_protocol protocol) {
    // The buffer is exactly the declared size: a size computation that
    // disagrees with the encoder now fails the encode instead of being masked
    // by a larger buffer.
    const std::size_t declared = encoded_value_size(value, protocol);
    std::vector<std::byte> out(declared);
    std::size_t written = 0;
    CRPC_CHECK(encode_value(as_writable_span(out), value, written, protocol).ok());
    CRPC_CHECK_EQ(written, declared);
    return text_of(std::vector<std::byte>(out.begin(), out.begin() + written));
}

std::string encode_command_text(const resp_command& command) {
    const std::size_t declared = encoded_command_size(command);
    std::vector<std::byte> out(declared);
    std::size_t written = 0;
    CRPC_CHECK(encode_command(as_writable_span(out), command, written).ok());
    CRPC_CHECK_EQ(written, declared);
    return text_of(std::vector<std::byte>(out.begin(), out.begin() + written));
}

resp_command command_of(std::initializer_list<std::string> args) {
    return resp_command(args);
}

void check_bulk(const resp_value& value, const std::string& expected) {
    CRPC_CHECK(value.is_bulk_string());
    CRPC_CHECK_EQ(expected, value.as_bulk_string());
}

void check_error(const resp_value& value, const std::string& expected) {
    CRPC_CHECK(value.is_error());
    CRPC_CHECK_EQ(expected, value.as_error());
}

} // namespace

// --- Command encoding -------------------------------------------------------

CRPC_TEST(resp_encode_command_resp2_golden_bytes) {
    const std::string encoded = encode_command_text(command_of({"SET", "key", "value"}));
    CRPC_CHECK_EQ(std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"), encoded);
}

CRPC_TEST(resp_encode_command_is_protocol_independent) {
    // A command is an array of bulk strings, valid in both protocols.
    const resp_command command = command_of({"GET", "hello"});
    CRPC_CHECK_EQ(encode_command_text(command),
                  encode_command_to_string(command));
    CRPC_CHECK_EQ(encoded_command_size(command), encode_command_text(command).size());
}

CRPC_TEST(resp_encode_command_empty_argument_and_no_arguments) {
    CRPC_CHECK_EQ(std::string("*1\r\n$3\r\nDEL\r\n"),
                  encode_command_text(command_of({"DEL"})));
    CRPC_CHECK_EQ(std::string("*2\r\n$3\r\nSET\r\n$0\r\n\r\n"),
                  encode_command_text(command_of({"SET", ""})));
}

CRPC_TEST(resp_encode_command_rejects_small_output_buffer) {
    const resp_command command = command_of({"PING"});
    std::byte small[8] = {};
    std::size_t written = 123;
    const status result = encode_command(span<std::byte>(small, sizeof(small)), command, written);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, static_cast<std::size_t>(0));
}

// --- RESP2 value encoding ---------------------------------------------------

CRPC_TEST(resp_encode_resp2_scalars) {
    CRPC_CHECK_EQ(std::string("$-1\r\n"),
                  encode_value_text(resp_value{resp_null{}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("+OK\r\n"),
                  encode_value_text(resp_value{resp_simple_string{"OK"}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("-ERR boom\r\n"),
                  encode_value_text(resp_value{resp_error{"ERR boom"}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string(":42\r\n"),
                  encode_value_text(resp_value{resp_integer{42}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string(":-7\r\n"),
                  encode_value_text(resp_value{resp_integer{-7}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("$5\r\nhello\r\n"),
                  encode_value_text(resp_value{resp_bulk_string{"hello"}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("$0\r\n\r\n"),
                  encode_value_text(resp_value{resp_bulk_string{""}}, resp_protocol::resp2));
}

CRPC_TEST(resp_encode_resp2_array) {
    const resp_value value{resp_array_value{{resp_integer{1}, resp_integer{2},
                                             resp_integer{3}}}};
    CRPC_CHECK_EQ(std::string("*3\r\n:1\r\n:2\r\n:3\r\n"),
                  encode_value_text(value, resp_protocol::resp2));
}

CRPC_TEST(resp_encode_resp2_nested_array_with_bulk) {
    const resp_value inner{resp_array_value{{resp_bulk_string{"a"}, resp_bulk_string{"b"}}}};
    const resp_value value{resp_array_value{{resp_simple_string{"X"}, inner}}};
    CRPC_CHECK_EQ(std::string("*2\r\n+X\r\n*2\r\n$1\r\na\r\n$1\r\nb\r\n"),
                  encode_value_text(value, resp_protocol::resp2));
}

CRPC_TEST(resp_encode_resp2_falls_back_for_resp3_only_types) {
    CRPC_CHECK_EQ(std::string(":1\r\n"),
                  encode_value_text(resp_value{resp_boolean{true}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string(":0\r\n"),
                  encode_value_text(resp_value{resp_boolean{false}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("$3\r\n1.5\r\n"),
                  encode_value_text(resp_value{resp_double{1.5}}, resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("$11\r\n12345678901\r\n"),
                  encode_value_text(resp_value{resp_big_number{"12345678901"}},
                                    resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("-ERR bad\r\n"),
                  encode_value_text(resp_value{resp_blob_error{"ERR bad"}},
                                    resp_protocol::resp2));
    CRPC_CHECK_EQ(std::string("$4\r\ntext\r\n"),
                  encode_value_text(resp_value{resp_verbatim_string{"text", "txt"}},
                                    resp_protocol::resp2));
}

// --- RESP3 value encoding ---------------------------------------------------

CRPC_TEST(resp_encode_resp3_types) {
    CRPC_CHECK_EQ(std::string("_\r\n"),
                  encode_value_text(resp_value{resp_null{}}, resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string("#t\r\n"),
                  encode_value_text(resp_value{resp_boolean{true}}, resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string("#f\r\n"),
                  encode_value_text(resp_value{resp_boolean{false}}, resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string(",1.5\r\n"),
                  encode_value_text(resp_value{resp_double{1.5}}, resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string(",inf\r\n"),
                  encode_value_text(resp_value{resp_double{
                                        std::numeric_limits<double>::infinity()}},
                                    resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string(",-inf\r\n"),
                  encode_value_text(resp_value{resp_double{
                                        -std::numeric_limits<double>::infinity()}},
                                    resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string("(12345678901\r\n"),
                  encode_value_text(resp_value{resp_big_number{"12345678901"}},
                                    resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string("!7\r\nERR bad\r\n"),
                  encode_value_text(resp_value{resp_blob_error{"ERR bad"}},
                                    resp_protocol::resp3));
    CRPC_CHECK_EQ(std::string("=8\r\ntxt:text\r\n"),
                  encode_value_text(resp_value{resp_verbatim_string{"text", "txt"}},
                                    resp_protocol::resp3));
}

CRPC_TEST(resp_encode_resp3_containers) {
    const resp_value map{resp_map_value{{{resp_bulk_string{"a"}, resp_integer{1}}}}};
    CRPC_CHECK_EQ(std::string("%1\r\n$1\r\na\r\n:1\r\n"),
                  encode_value_text(map, resp_protocol::resp3));

    const resp_value set{resp_set_value{{resp_integer{1}, resp_integer{2}}}};
    CRPC_CHECK_EQ(std::string("~2\r\n:1\r\n:2\r\n"),
                  encode_value_text(set, resp_protocol::resp3));

    const resp_value push{resp_push_value{{resp_bulk_string{"message"},
                                           resp_bulk_string{"channel"},
                                           resp_bulk_string{"payload"}}}};
    CRPC_CHECK_EQ(std::string(">3\r\n$7\r\nmessage\r\n$7\r\nchannel\r\n$7\r\npayload\r\n"),
                  encode_value_text(push, resp_protocol::resp3));

    // The RESP2 fallback flattens the map into a flat array.
    CRPC_CHECK_EQ(std::string("*2\r\n$1\r\na\r\n:1\r\n"),
                  encode_value_text(map, resp_protocol::resp2));
}

CRPC_TEST(resp_encode_resp3_nested_map) {
    const resp_value map{resp_map_value{
        {{resp_bulk_string{"outer"},
          resp_map_value{{{resp_bulk_string{"inner"}, resp_integer{7}}}}}}}};
    CRPC_CHECK_EQ(std::string("%1\r\n$5\r\nouter\r\n%1\r\n$5\r\ninner\r\n:7\r\n"),
                  encode_value_text(map, resp_protocol::resp3));
}

CRPC_TEST(resp_encode_size_matches_write_for_every_type) {
    const std::vector<resp_value> values = {
        resp_value{resp_null{}},
        resp_value{resp_simple_string{"hi"}},
        resp_value{resp_error{"ERR x"}},
        resp_value{resp_integer{-12}},
        resp_value{resp_bulk_string{""}},
        resp_value{resp_bulk_string{"payload"}},
        resp_value{resp_array_value{{resp_integer{1}, resp_null{}}}},
        resp_value{resp_boolean{true}},
        resp_value{resp_double{3.25}},
        resp_value{resp_big_number{"12345678901234567890"}},
        resp_value{resp_blob_error{"ERR x"}},
        resp_value{resp_verbatim_string{"body", "mkd"}},
        resp_value{resp_map_value{{{resp_integer{1}, resp_bulk_string{"v"}}}}},
        resp_value{resp_set_value{{resp_integer{1}}}},
        resp_value{resp_push_value{{resp_bulk_string{"p"}}}},
    };
    for (const resp_value& value : values) {
        for (const resp_protocol protocol : {resp_protocol::resp2, resp_protocol::resp3}) {
            const std::string encoded = encode_value_text(value, protocol);
            CRPC_CHECK_EQ(encoded_value_size(value, protocol), encoded.size());
        }
    }
}

CRPC_TEST(resp_encoder_class_matches_free_functions) {
    resp_encoder encoder{resp_protocol::resp3};
    CRPC_CHECK_EQ(static_cast<int>(encoder.protocol()),
                  static_cast<int>(resp_protocol::resp3));
    const resp_value value{resp_push_value{{resp_bulk_string{"p"}}}};
    CRPC_CHECK_EQ(encoder.encoded_size(value), encoded_value_size(value, resp_protocol::resp3));
    CRPC_CHECK_EQ(encoder.encode_to_string(value), encode_value_to_string(value, resp_protocol::resp3));
    encoder.protocol(resp_protocol::resp2);
    CRPC_CHECK_EQ(encoder.encode_to_string(value), "*1\r\n$1\r\np\r\n");
}

// --- RESP2 parsing ----------------------------------------------------------

CRPC_TEST(resp_parse_resp2_scalars) {
    const resp_value simple = parse_one("+OK\r\n");
    CRPC_CHECK(simple.is_simple_string());
    CRPC_CHECK_EQ(std::string("OK"), simple.as_simple_string());

    const resp_value error = parse_one("-ERR unknown command\r\n");
    check_error(error, "ERR unknown command");

    const resp_value integer = parse_one(":1000\r\n");
    CRPC_CHECK(integer.is_integer());
    CRPC_CHECK_EQ(integer.as_integer(), static_cast<std::int64_t>(1000));

    const resp_value negative = parse_one(":-123\r\n");
    CRPC_CHECK_EQ(negative.as_integer(), static_cast<std::int64_t>(-123));

    const resp_value bulk = parse_one("$6\r\nfoobar\r\n");
    check_bulk(bulk, "foobar");

    const resp_value empty = parse_one("$0\r\n\r\n");
    CRPC_CHECK(empty.is_bulk_string());
    CRPC_CHECK(empty.as_bulk_string().empty());
}

CRPC_TEST(resp_parse_resp2_nulls) {
    CRPC_CHECK(parse_one("$-1\r\n").is_null());
    CRPC_CHECK(parse_one("*-1\r\n").is_null());
}

CRPC_TEST(resp_parse_resp2_arrays) {
    const resp_value array = parse_one("*2\r\n:1\r\n$5\r\nhello\r\n");
    CRPC_CHECK(array.is_array());
    CRPC_CHECK_EQ(array.as_array().size(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(array.as_array()[0].as_integer(), static_cast<std::int64_t>(1));
    check_bulk(array.as_array()[1], "hello");
}

CRPC_TEST(resp_parse_resp2_nested_arrays) {
    const resp_value nested = parse_one("*2\r\n*2\r\n:1\r\n:2\r\n+OK\r\n");
    CRPC_CHECK(nested.is_array());
    CRPC_CHECK(nested.as_array()[0].is_array());
    CRPC_CHECK_EQ(nested.as_array()[0].as_array().size(), static_cast<std::size_t>(2));
    CRPC_CHECK(nested.as_array()[1].is_simple_string());
    CRPC_CHECK_EQ(std::string("OK"), nested.as_array()[1].as_simple_string());
}

CRPC_TEST(resp_parse_bulk_string_may_contain_crlf) {
    const resp_value bulk = parse_one("$4\r\na\r\nb\r\n");
    check_bulk(bulk, "a\r\nb");
}

// --- RESP3 parsing ----------------------------------------------------------

CRPC_TEST(resp_parse_resp3_scalars) {
    CRPC_CHECK(parse_one("_\r\n").is_null());

    const resp_value yes = parse_one("#t\r\n");
    CRPC_CHECK(yes.is_boolean());
    CRPC_CHECK(yes.as_boolean());

    const resp_value no = parse_one("#f\r\n");
    CRPC_CHECK(no.is_boolean());
    CRPC_CHECK_FALSE(no.as_boolean());

    const resp_value real = parse_one(",3.1415\r\n");
    CRPC_CHECK(real.is_double());
    CRPC_CHECK_EQ(real.as_double(), 3.1415);

    const resp_value infinity = parse_one(",inf\r\n");
    CRPC_CHECK(infinity.is_double());
    CRPC_CHECK(std::isinf(infinity.as_double()));

    const resp_value negative_infinity = parse_one(",-inf\r\n");
    CRPC_CHECK(std::isinf(negative_infinity.as_double()));
    CRPC_CHECK(negative_infinity.as_double() < 0.0);

    const resp_value nan = parse_one(",nan\r\n");
    CRPC_CHECK(std::isnan(nan.as_double()));

    const resp_value big = parse_one("(34928903284092350983423587459834235\r\n");
    CRPC_CHECK(big.is_big_number());
    CRPC_CHECK_EQ(std::string("34928903284092350983423587459834235"), big.as_big_number());

    const resp_value blob = parse_one("!21\r\nSYNTAX invalid syntax\r\n");
    CRPC_CHECK(blob.is_blob_error());
    CRPC_CHECK_EQ(std::string("SYNTAX invalid syntax"), blob.as_blob_error());

    const resp_value verbatim = parse_one("=15\r\ntxt:Some string\r\n");
    CRPC_CHECK(verbatim.is_verbatim_string());
    CRPC_CHECK_EQ(std::string("Some string"), verbatim.as_verbatim_string());
    CRPC_CHECK_EQ(std::string("txt"), verbatim.verbatim_format());
}

CRPC_TEST(resp_parse_resp3_map) {
    const resp_value map = parse_one("%2\r\n$3\r\nfoo\r\n:1\r\n$3\r\nbar\r\n:2\r\n");
    CRPC_CHECK(map.is_map());
    CRPC_CHECK_EQ(map.as_map().size(), static_cast<std::size_t>(2));
    check_bulk(map.as_map()[0].first, "foo");
    CRPC_CHECK_EQ(map.as_map()[0].second.as_integer(), static_cast<std::int64_t>(1));
    check_bulk(map.as_map()[1].first, "bar");
    CRPC_CHECK_EQ(map.as_map()[1].second.as_integer(), static_cast<std::int64_t>(2));
}

CRPC_TEST(resp_parse_resp3_map_with_non_string_keys) {
    const resp_value map = parse_one("%1\r\n:1\r\n#t\r\n");
    CRPC_CHECK(map.is_map());
    CRPC_CHECK_EQ(map.as_map()[0].first.as_integer(), static_cast<std::int64_t>(1));
    CRPC_CHECK(map.as_map()[0].second.as_boolean());
}

CRPC_TEST(resp_parse_resp3_set_and_push) {
    const resp_value set = parse_one("~3\r\n:1\r\n:2\r\n:3\r\n");
    CRPC_CHECK(set.is_set());
    CRPC_CHECK_EQ(set.as_set().size(), static_cast<std::size_t>(3));
    CRPC_CHECK_EQ(set.as_set()[2].as_integer(), static_cast<std::int64_t>(3));

    const resp_value push = parse_one(">2\r\n+message\r\n$7\r\npayload\r\n");
    CRPC_CHECK(push.is_push());
    CRPC_CHECK_EQ(push.as_push().size(), static_cast<std::size_t>(2));
    CRPC_CHECK(push.as_push()[0].is_simple_string());
    check_bulk(push.as_push()[1], "payload");
}

CRPC_TEST(resp_parse_resp3_empty_containers) {
    const resp_value array = parse_one("*0\r\n");
    CRPC_CHECK(array.is_array());
    CRPC_CHECK(array.as_array().empty());

    const resp_value map = parse_one("%0\r\n");
    CRPC_CHECK(map.is_map());
    CRPC_CHECK(map.as_map().empty());

    const resp_value set = parse_one("~0\r\n");
    CRPC_CHECK(set.is_set());
    CRPC_CHECK(set.as_set().empty());
}

// --- Streaming --------------------------------------------------------------

CRPC_TEST(resp_parser_byte_at_a_time) {
    const std::string text = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n";
    resp_parser parser;
    resp_value value;
    std::size_t ready = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::byte byte = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
        parser.feed(span<const std::byte>(&byte, 1));
        const resp_parser::result result = parser.next(value);
        if (i + 1 < text.size()) {
            CRPC_CHECK_EQ(static_cast<int>(result),
                          static_cast<int>(resp_parser::result::need_more));
        } else {
            CRPC_CHECK_EQ(static_cast<int>(result),
                          static_cast<int>(resp_parser::result::value_ready));
            ++ready;
        }
    }
    CRPC_CHECK_EQ(ready, static_cast<std::size_t>(1));
    CRPC_CHECK(value.is_array());
    CRPC_CHECK_EQ(value.as_array().size(), static_cast<std::size_t>(3));
    CRPC_CHECK(parser.finish().ok());
}

CRPC_TEST(resp_parser_split_at_every_boundary) {
    const std::string text = "*2\r\n%1\r\n+a\r\n:1\r\n$4\r\nbulk\r\n";
    for (std::size_t split = 1; split < text.size(); ++split) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text.substr(0, split))));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::need_more));
        parser.feed(as_span(bytes_of(text.substr(split))));
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::value_ready));
        CRPC_CHECK(value.is_array());
        CRPC_CHECK(value.as_array()[0].is_map());
        CRPC_CHECK(value.as_array()[1].is_bulk_string());
        CRPC_CHECK(parser.finish().ok());
    }
}

CRPC_TEST(resp_parser_several_values_in_one_feed) {
    const std::string text = ":1\r\n:2\r\n+three\r\n";
    resp_parser parser;
    parser.feed(as_span(bytes_of(text)));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::value_ready));
    CRPC_CHECK_EQ(value.as_integer(), static_cast<std::int64_t>(1));
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::value_ready));
    CRPC_CHECK_EQ(value.as_integer(), static_cast<std::int64_t>(2));
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::value_ready));
    CRPC_CHECK_EQ(std::string("three"), value.as_simple_string());
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK(parser.finish().ok());
    CRPC_CHECK_EQ(parser.buffered(), static_cast<std::size_t>(0));
}

CRPC_TEST(resp_parser_need_more_and_buffered) {
    resp_parser parser;
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK_EQ(parser.buffered(), static_cast<std::size_t>(0));
}

CRPC_TEST(resp_parser_truncated_value_finish_reports_error) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("*2\r\n:1\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK(parser.buffered() > 0);
    const status result = parser.finish();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parser_reset_clears_error_and_buffered) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("?bad\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK(parser.failed());
    parser.reset();
    CRPC_CHECK_FALSE(parser.failed());
    CRPC_CHECK_EQ(static_cast<int>(parser.error()), static_cast<int>(rpc_code::ok));
    CRPC_CHECK_EQ(parser.buffered(), static_cast<std::size_t>(0));
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
}

// --- Fail-closed parsing ----------------------------------------------------

CRPC_TEST(resp_parser_rejects_unknown_marker) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("Xnope\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
    CRPC_CHECK_EQ(static_cast<int>(parser.finish().code()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parser_rejects_malformed_numbers) {
    const std::string bad[] = {":\r\n", ":12a\r\n", ": 1\r\n", ":+1\r\n",
                               ":1.5\r\n", ":99999999999999999999999\r\n"};
    for (const std::string& text : bad) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text)));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::error));
        CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                      static_cast<int>(rpc_code::protocol_error));
    }
}

CRPC_TEST(resp_parser_rejects_malformed_lengths) {
    // A malformed length must never yield a value: it either errors
    // immediately or leaves a truncated value (need_more).
    const std::string malformed[] = {"$abc\r\n", "*x\r\n", "$ \r\n", "*x2\r\n",
                                     "%q\r\n", "~z\r\n", ">w\r\n", "!n\r\n",
                                     "=v\r\n"};
    for (const std::string& text : malformed) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text)));
        resp_value value;
        const resp_parser::result result = parser.next(value);
        CRPC_CHECK(result != resp_parser::result::value_ready);
        if (result == resp_parser::result::error) {
            CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                          static_cast<int>(rpc_code::protocol_error));
        } else {
            CRPC_CHECK_FALSE(parser.finish().ok());
        }
    }
}

CRPC_TEST(resp_parser_rejects_bad_boolean) {
    for (const std::string text : {"#x\r\n", "#true\r\n", "#\r\n"}) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text)));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::error));
        CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                      static_cast<int>(rpc_code::protocol_error));
    }
}

CRPC_TEST(resp_parser_rejects_bad_double) {
    for (const std::string text : {",\r\n", ",abc\r\n", ",1.2.3\r\n", ",1 2\r\n"}) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text)));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::error));
    }
}

CRPC_TEST(resp_parser_rejects_negative_count_other_than_null) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("*-2\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parser_rejects_missing_crlf_after_bulk) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("$3\r\nabcXX")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parser_rejects_bad_verbatim_format) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("=4\r\nabcd\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parser_enforces_max_value_size) {
    resp_parser parser{8};
    parser.feed(as_span(bytes_of("$9\r\n123456789\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::resource_exhausted));

    resp_parser array_parser{8};
    array_parser.feed(as_span(bytes_of("*9\r\n")));
    CRPC_CHECK_EQ(static_cast<int>(array_parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(array_parser.error()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

CRPC_TEST(resp_parser_enforces_max_depth) {
    resp_parser parser{resp_default_max_value_size, 4};
    std::string text;
    for (int i = 0; i < 6; ++i) {
        text += "*1\r\n";
    }
    text += "+ok\r\n";
    parser.feed(as_span(bytes_of(text)));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

// --- Round trips ------------------------------------------------------------

CRPC_TEST(resp_round_trip_every_type) {
    const std::vector<resp_value> values = {
        resp_value{resp_null{}},
        resp_value{resp_simple_string{"simple"}},
        resp_value{resp_error{"ERR bad"}},
        resp_value{resp_integer{-9223372036854775807LL - 1}},
        resp_value{resp_bulk_string{"bulk\r\nbytes"}},
        resp_value{resp_array_value{{resp_integer{1}, resp_bulk_string{"x"}}}},
        resp_value{resp_boolean{true}},
        resp_value{resp_double{2.5}},
        resp_value{resp_big_number{"123456789012345678901234567890"}},
        resp_value{resp_blob_error{"ERR bad"}},
        resp_value{resp_verbatim_string{"payload", "txt"}},
        resp_value{resp_map_value{{{resp_bulk_string{"k"}, resp_integer{1}}}}},
        resp_value{resp_set_value{{resp_integer{1}, resp_integer{2}}}},
        resp_value{resp_push_value{{resp_bulk_string{"channel"}, resp_bulk_string{"data"}}}},
    };
    for (const resp_value& value : values) {
        // RESP3 has a marker for every type, so the round trip is exact.
        const std::string encoded3 = encode_value_text(value, resp_protocol::resp3);
        const resp_value decoded3 = parse_one(encoded3);
        CRPC_CHECK(decoded3 == value);

        // RESP2 has no marker for the RESP3-only types; the encoder downgrades
        // them (boolean -> integer, double -> bulk string, map -> array, ...),
        // so the type is not preserved. Require only that the wire form parses
        // back to a value.
        const std::string encoded2 = encode_value_text(value, resp_protocol::resp2);
        const resp_value decoded2 = parse_one(encoded2);
        (void)decoded2;
    }
}

CRPC_TEST(resp_command_round_trip) {
    const resp_command command = command_of({"HSET", "hash", "field", "value"});
    const std::string encoded = encode_command_text(command);
    const resp_value decoded = parse_one(encoded);
    CRPC_CHECK(decoded.is_array());
    CRPC_CHECK_EQ(decoded.as_array().size(), command.size());
    for (std::size_t i = 0; i < command.size(); ++i) {
        check_bulk(decoded.as_array()[i], command[i]);
    }
}

CRPC_TEST(resp_equality_operator) {
    CRPC_CHECK(parse_one(":1\r\n") == parse_one(":1\r\n"));
    CRPC_CHECK(parse_one(":1\r\n") != parse_one(":2\r\n"));
    CRPC_CHECK(parse_one("+a\r\n") != parse_one("$1\r\na\r\n"));
}

// --- Audit regression coverage ----------------------------------------------

CRPC_TEST(resp_equality_all_types) {
    // The rewritten operator== uses only public type()/as_*() accessors; this
    // exercises every branch and the nested container comparisons.
    const resp_value null_value{resp_null{}};
    const resp_value simple{resp_simple_string{"s"}};
    const resp_value error{resp_error{"e"}};
    const resp_value integer{resp_integer{7}};
    const resp_value bulk{resp_bulk_string{"b"}};
    const resp_value array{resp_array_value{{resp_integer{1}}}};
    const resp_value boolean{resp_boolean{true}};
    const resp_value real{resp_double{1.5}};
    const resp_value big{resp_big_number{"123"}};
    const resp_value blob{resp_blob_error{"be"}};
    const resp_value verbatim{resp_verbatim_string{"v", "txt"}};
    const resp_value map{
        resp_map_value{{{resp_bulk_string{"k"}, resp_integer{2}}}}};
    const resp_value set{resp_set_value{{resp_integer{1}}}};
    const resp_value push{resp_push_value{{resp_integer{1}}}};

    const resp_value all[] = {null_value, simple, error, integer, bulk, array,
                              boolean,     real,    big,   blob,  verbatim,
                              map,         set,     push};
    constexpr std::size_t all_count = sizeof(all) / sizeof(all[0]);
    for (const resp_value& value : all) {
        CRPC_CHECK(value == value);
    }
    for (std::size_t i = 0; i < all_count; ++i) {
        for (std::size_t j = i + 1; j < all_count; ++j) {
            CRPC_CHECK(all[i] != all[j]);
        }
    }
    const resp_value nested_a{resp_array_value{{resp_integer{1}, map}}};
    const resp_value nested_b{resp_array_value{{resp_integer{1}, map}}};
    CRPC_CHECK(nested_a == nested_b);
    const resp_value nested_c{resp_array_value{{resp_integer{2}, map}}};
    CRPC_CHECK(nested_a != nested_c);
}

CRPC_TEST(resp_encode_verbatim_format_length_consistency) {
    // The verbatim length covers the format, the colon and the payload, so the
    // declared size, the encoded bytes and the written count must agree for any
    // format length, not only the canonical three.
    const std::string formats[] = {"", "a", "ab", "txt", "abcd", "abcde"};
    for (const std::string& format : formats) {
        const resp_value value{resp_verbatim_string{"payload", format}};
        for (const resp_protocol protocol :
             {resp_protocol::resp2, resp_protocol::resp3}) {
            const std::size_t declared = encoded_value_size(value, protocol);
            std::vector<std::byte> out(declared);
            std::size_t written = 0;
            CRPC_CHECK(
                encode_value(as_writable_span(out), value, written, protocol).ok());
            CRPC_CHECK_EQ(written, declared);
        }
    }
}

CRPC_TEST(resp_encode_large_map_size_matches_write) {
    resp_map entries;
    for (std::int64_t i = 0; i < 1000; ++i) {
        entries.emplace_back(resp_value{resp_integer{i}},
                             resp_value{resp_integer{i * 2}});
    }
    const resp_value map{resp_map_value{std::move(entries)}};
    for (const resp_protocol protocol :
         {resp_protocol::resp2, resp_protocol::resp3}) {
        const std::string encoded = encode_value_text(map, protocol);
        CRPC_CHECK_EQ(encoded_value_size(map, protocol), encoded.size());
    }
}

CRPC_TEST(resp_parser_rejects_length_above_size_t_range) {
    // 2^32 + 10 is far above the default 512 MiB limit; on a 32-bit host a
    // naive size_t cast would truncate it to 10 and accept the value.
    resp_parser bulk;
    bulk.feed(as_span(bytes_of("$4294967306\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(bulk.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(bulk.error()),
                  static_cast<int>(rpc_code::resource_exhausted));

    resp_parser array;
    array.feed(as_span(bytes_of("*4294967301\r\n")));
    CRPC_CHECK_EQ(static_cast<int>(array.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(array.error()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

CRPC_TEST(resp_parser_enforces_line_length_limit) {
    resp_parser parser{8};
    std::string long_line = "+";
    long_line.append(100, 'a');
    long_line += "\r\n";
    parser.feed(as_span(bytes_of(long_line)));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::resource_exhausted));

    // An unterminated line must not grow the buffer without bound.
    resp_parser unterminated{8};
    std::string partial = "+";
    partial.append(100, 'a');
    unterminated.feed(as_span(bytes_of(partial)));
    CRPC_CHECK_EQ(static_cast<int>(unterminated.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(unterminated.error()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

CRPC_TEST(resp_parser_large_pipeline_consumes_all) {
    constexpr std::size_t count = 20000;
    std::string text;
    text.reserve(count * 4u);
    for (std::size_t i = 0; i < count; ++i) {
        text += ":1\r\n";
    }
    resp_parser parser;
    parser.feed(as_span(bytes_of(text)));
    resp_value value;
    for (std::size_t i = 0; i < count; ++i) {
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::value_ready));
        CRPC_CHECK_EQ(value.as_integer(), static_cast<std::int64_t>(1));
    }
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK_EQ(parser.buffered(), static_cast<std::size_t>(0));
    CRPC_CHECK(parser.finish().ok());
}

CRPC_TEST(resp_parser_rejects_bare_cr_in_simple_string) {
    for (const std::string text : {"+ab\rcd\r\n", "-ab\rcd\r\n", "+ab\ncd\r\n"}) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text)));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::error));
        CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                      static_cast<int>(rpc_code::protocol_error));
    }
}

// --- Encoding fail-closed ---------------------------------------------------

CRPC_TEST(resp_encode_rejects_crlf_in_simple_string) {
    const resp_value value{resp_simple_string{"a\r\nb"}};
    const std::size_t declared = encoded_value_size(value, resp_protocol::resp2);
    std::vector<std::byte> out(declared);
    std::size_t written = 123;
    const status result =
        encode_value(as_writable_span(out), value, written, resp_protocol::resp2);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()),
                  static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, static_cast<std::size_t>(0));
}

CRPC_TEST(resp_encode_rejects_crlf_in_error) {
    const resp_value value{resp_error{"ERR a\nb"}};
    const std::size_t declared = encoded_value_size(value, resp_protocol::resp2);
    std::vector<std::byte> out(declared);
    std::size_t written = 123;
    const status result =
        encode_value(as_writable_span(out), value, written, resp_protocol::resp2);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()),
                  static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, static_cast<std::size_t>(0));
}

CRPC_TEST(resp_encode_rejects_crlf_nested_in_containers) {
    const resp_value nested{resp_array_value{{resp_simple_string{"x\ry"}}}};
    const std::size_t declared = encoded_value_size(nested, resp_protocol::resp2);
    std::vector<std::byte> out(declared);
    std::size_t written = 123;
    CRPC_CHECK_FALSE(encode_value(as_writable_span(out), nested, written,
                                  resp_protocol::resp2)
                         .ok());
    CRPC_CHECK_EQ(written, static_cast<std::size_t>(0));

    const resp_value in_map{resp_map_value{
        {{resp_bulk_string{"k"}, resp_error{"ERR a\r\nb"}}}}};
    const std::size_t map_declared =
        encoded_value_size(in_map, resp_protocol::resp3);
    std::vector<std::byte> map_out(map_declared);
    std::size_t map_written = 123;
    CRPC_CHECK_FALSE(encode_value(as_writable_span(map_out), in_map, map_written,
                                  resp_protocol::resp3)
                         .ok());
    CRPC_CHECK_EQ(map_written, static_cast<std::size_t>(0));
}

CRPC_TEST(resp_encode_to_string_rejects_crlf_in_simple_string) {
    const resp_value value{resp_simple_string{"a\r\nb"}};
    bool threw = false;
    try {
        (void)encode_value_to_string(value, resp_protocol::resp2);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CRPC_CHECK(threw);

    const resp_value nested{resp_array_value{{resp_error{"ERR a\nb"}}}};
    bool nested_threw = false;
    try {
        (void)encode_value_to_string(nested, resp_protocol::resp2);
    } catch (const std::invalid_argument&) {
        nested_threw = true;
    }
    CRPC_CHECK(nested_threw);
}

CRPC_TEST(resp_parser_rejects_double_overflow) {
    for (const std::string text : {",1e999\r\n", ",-1e999\r\n"}) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text)));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::error));
        CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                      static_cast<int>(rpc_code::protocol_error));
    }
}

CRPC_TEST(resp_parser_finish_complete_value_ok) {
    resp_parser parser;
    parser.feed(as_span(bytes_of(":1\r\n:2\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::value_ready));
    CRPC_CHECK_EQ(value.as_integer(), static_cast<std::int64_t>(1));
    // The second value is complete but unconsumed; the stream is not truncated.
    CRPC_CHECK(parser.finish().ok());
    CRPC_CHECK_FALSE(parser.failed());
}

CRPC_TEST(resp_parser_finish_truncated_sets_error) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("*2\r\n:1\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    const status result = parser.finish();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()),
                  static_cast<int>(rpc_code::protocol_error));
    CRPC_CHECK(parser.failed());
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

// --- Mutation loops ---------------------------------------------------------

CRPC_TEST(resp_parser_fuzz_random_input_never_crashes) {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const std::size_t length = static_cast<std::size_t>(state % 257u);
        std::vector<std::byte> data(length);
        for (std::size_t i = 0; i < length; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            data[i] = static_cast<std::byte>(state & 0xFFu);
        }

        resp_parser parser;
        resp_value value;
        std::size_t offset = 0;
        int guard = 0;
        while (offset < data.size() && guard < 512 && !parser.failed()) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::size_t chunk = 1 + static_cast<std::size_t>(state % 23u);
            const std::size_t take = (chunk < data.size() - offset) ? chunk : (data.size() - offset);
            parser.feed(span<const std::byte>(data.data() + offset, take));
            offset += take;
            int value_guard = 0;
            while (parser.next(value) == resp_parser::result::value_ready &&
                   value_guard < 512) {
                ++value_guard;
            }
            ++guard;
        }
        (void)parser.finish();
    }
    CRPC_CHECK(true);
}

CRPC_TEST(resp_parser_fuzz_mutated_values_never_crashes) {
    std::uint64_t state = 0x1234567890ABCDEFull;
    const std::vector<resp_value> seeds = {
        resp_value{resp_array_value{{resp_integer{1}, resp_bulk_string{"hello"},
                                    resp_simple_string{"ok"}}}},
        resp_value{resp_map_value{{{resp_bulk_string{"k"}, resp_double{1.5}}}}},
        resp_value{resp_push_value{{resp_bulk_string{"p"}, resp_integer{2}}}},
        resp_value{resp_verbatim_string{"body", "txt"}},
    };

    for (int iteration = 0; iteration < 1000; ++iteration) {
        const resp_value& seed = seeds[static_cast<std::size_t>(iteration) % seeds.size()];
        std::string encoded = encode_value_to_string(seed, resp_protocol::resp3);
        std::vector<std::byte> bytes = bytes_of(encoded);

        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const std::size_t mutations = 1 + static_cast<std::size_t>(state % 3u);
        for (std::size_t m = 0; m < mutations && !bytes.empty(); ++m) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::size_t index = static_cast<std::size_t>(state % bytes.size());
            bytes[index] = static_cast<std::byte>(
                static_cast<unsigned>(static_cast<unsigned char>(bytes[index])) ^
                static_cast<unsigned>(state & 0xFFu));
        }

        resp_parser parser;
        resp_value decoded;
        parser.feed(as_span(bytes));
        int value_guard = 0;
        while (parser.next(decoded) == resp_parser::result::value_ready && value_guard < 512) {
            ++value_guard;
        }
        (void)parser.finish();
    }
    CRPC_CHECK(true);
}

// --- RESP3 attributes and streamed frames -----------------------------------
//
// Byte forms confirmed against the official RESP3 specification
// (redis/redis-specifications repository, protocol/RESP3.md), not from memory:
//   attribute      `|<count>\r\n` then count key/value pairs, then the value it
//                  augments (spec section "Attribute type");
//   streamed str   `$?\r\n` then `;<len>\r\n<data>\r\n` chunks, terminated by
//                  `;0\r\n` (spec section "Streamed strings"); the example in
//                  the spec spells `$?` / `;4` / `;5` / `;1` / `;0`;
//   streamed agg   `*?` (also `~?`, `%?`) then elements, terminated by a
//                  `<CR><LF>` line whose lone byte is `.` (spec section
//                  "Streamed aggregated data types").
// Redis 7.0 core commands emit none of these frames (the spec states the
// server has no command producing streamed strings or aggregates), so every
// case in this section is a constructed-byte case, not a real-server capture.

CRPC_TEST(resp_parse_resp3_attribute_augments_reply) {
    const resp_value reply = parse_one("|1\r\n+ttl\r\n:3600\r\n+OK\r\n");
    CRPC_CHECK(reply.is_simple_string());
    CRPC_CHECK_EQ(std::string("OK"), reply.as_simple_string());
    CRPC_CHECK(reply.has_attributes());
    CRPC_CHECK_EQ(reply.attributes().size(), static_cast<std::size_t>(1));
    CRPC_CHECK_EQ(std::string("ttl"), reply.attributes()[0].first.as_simple_string());
    CRPC_CHECK_EQ(reply.attributes()[0].second.as_integer(),
                  static_cast<std::int64_t>(3600));
}

CRPC_TEST(resp_parse_resp3_attribute_does_not_change_the_reply) {
    // A caller that ignores attributes still reads the exact same reply value.
    const resp_value reply = parse_one("|1\r\n+ttl\r\n:3600\r\n*2\r\n:1\r\n:2\r\n");
    CRPC_CHECK(reply.is_array());
    CRPC_CHECK_EQ(reply.as_array().size(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(reply.as_array()[1].as_integer(), static_cast<std::int64_t>(2));
}

CRPC_TEST(resp_parse_resp3_attribute_spec_key_popularity_example) {
    // The exact example from the RESP3 spec: an attribute whose value is a map.
    const resp_value reply = parse_one(
        "|1\r\n+key-popularity\r\n%2\r\n$1\r\na\r\n,0.1923\r\n$1\r\nb\r\n,0.0012\r\n"
        "*2\r\n:2039123\r\n:9543892\r\n");
    CRPC_CHECK(reply.is_array());
    CRPC_CHECK(reply.has_attributes());
    CRPC_CHECK_EQ(reply.attributes().size(), static_cast<std::size_t>(1));
    CRPC_CHECK_EQ(std::string("key-popularity"),
                  reply.attributes()[0].first.as_simple_string());
    CRPC_CHECK(reply.attributes()[0].second.is_map());
    CRPC_CHECK_EQ(reply.attributes()[0].second.as_map().size(),
                  static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(reply.attributes()[0].second.as_map()[1].second.as_double(), 0.0012);
}

CRPC_TEST(resp_parse_resp3_attribute_nested_inside_array) {
    // The spec example: the third element of an array carries its own attribute.
    const resp_value reply = parse_one("*3\r\n:1\r\n:2\r\n|1\r\n+ttl\r\n:3600\r\n:3\r\n");
    CRPC_CHECK(reply.is_array());
    CRPC_CHECK_FALSE(reply.has_attributes());
    CRPC_CHECK_FALSE(reply.as_array()[0].has_attributes());
    CRPC_CHECK(reply.as_array()[2].has_attributes());
    CRPC_CHECK_EQ(reply.as_array()[2].as_integer(), static_cast<std::int64_t>(3));
    CRPC_CHECK_EQ(std::string("ttl"),
                  reply.as_array()[2].attributes()[0].first.as_simple_string());
    CRPC_CHECK_EQ(reply.as_array()[2].attributes()[0].second.as_integer(),
                  static_cast<std::int64_t>(3600));
}

CRPC_TEST(resp_parse_resp3_empty_attribute_map) {
    const resp_value reply = parse_one("|0\r\n:7\r\n");
    CRPC_CHECK_EQ(reply.as_integer(), static_cast<std::int64_t>(7));
    CRPC_CHECK(reply.has_attributes());
    CRPC_CHECK(reply.attributes().empty());
}

CRPC_TEST(resp_parse_resp3_attribute_attribute_attached_after_split) {
    // The attribute frame and the augmented reply may arrive in separate feeds.
    const std::string text = "|1\r\n+ttl\r\n:3600\r\n+OK\r\n";
    for (std::size_t split = 1; split < text.size(); ++split) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text.substr(0, split))));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::need_more));
        parser.feed(as_span(bytes_of(text.substr(split))));
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::value_ready));
        CRPC_CHECK(value.is_simple_string());
        CRPC_CHECK(value.has_attributes());
        CRPC_CHECK_EQ(value.attributes().size(), static_cast<std::size_t>(1));
        CRPC_CHECK(parser.finish().ok());
    }
}

CRPC_TEST(resp_parse_resp3_attribute_without_reply_is_truncated) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("|1\r\n+ttl\r\n:3600\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK_FALSE(parser.finish().ok());
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parse_resp3_attribute_rejects_null_count) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("|-1\r\n:3\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parse_resp3_streamed_string_spec_example) {
    // The exact bytes from the RESP3 spec section "Streamed strings". Note the
    // spec's own example concatenates to "Hello word", not "Hello world" (the
    // prose says the latter but the chunks Hell / o wor / d spell the former);
    // the decoder must reproduce exactly what the chunks carry.
    const resp_value reply =
        parse_one("$?\r\n;4\r\nHell\r\n;5\r\no wor\r\n;1\r\nd\r\n;0\r\n");
    CRPC_CHECK(reply.is_bulk_string());
    CRPC_CHECK_EQ(std::string("Hello word"), reply.as_bulk_string());
}

CRPC_TEST(resp_parse_resp3_streamed_string_correct_concatenation) {
    // A canonical "Hello world" split into chunks that do add up, pinning the
    // intended semantics (each chunk appends its bytes, byte-for-byte).
    const resp_value reply =
        parse_one("$?\r\n;5\r\nHello\r\n;1\r\n \r\n;5\r\nworld\r\n;0\r\n");
    CRPC_CHECK(reply.is_bulk_string());
    CRPC_CHECK_EQ(std::string("Hello world"), reply.as_bulk_string());
}

CRPC_TEST(resp_parse_resp3_streamed_string_empty) {
    const resp_value reply = parse_one("$?\r\n;0\r\n");
    CRPC_CHECK(reply.is_bulk_string());
    CRPC_CHECK(reply.as_bulk_string().empty());
}

CRPC_TEST(resp_parse_resp3_streamed_string_split_at_every_boundary) {
    const std::string text =
        "$?\r\n;5\r\nHello\r\n;1\r\n \r\n;5\r\nworld\r\n;0\r\n";
    for (std::size_t split = 1; split < text.size(); ++split) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text.substr(0, split))));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::need_more));
        parser.feed(as_span(bytes_of(text.substr(split))));
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::value_ready));
        CRPC_CHECK(value.is_bulk_string());
        CRPC_CHECK_EQ(std::string("Hello world"), value.as_bulk_string());
        CRPC_CHECK(parser.finish().ok());
    }
}

CRPC_TEST(resp_parse_resp3_streamed_string_rejects_bad_length) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("$?\r\n;-1\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parse_resp3_streamed_string_missing_terminator_is_truncated) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("$?\r\n;4\r\nHell\r\n;5\r\no wor\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK_FALSE(parser.finish().ok());
}

CRPC_TEST(resp_parse_resp3_streamed_string_respects_size_limit) {
    resp_parser parser(8u);
    parser.feed(as_span(bytes_of("$?\r\n;4\r\nHell\r\n;5\r\no wor\r\n;0\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

CRPC_TEST(resp_parse_resp3_streamed_array_and_set) {
    const resp_value array = parse_one("*?\r\n:1\r\n:2\r\n.\r\n");
    CRPC_CHECK(array.is_array());
    CRPC_CHECK_EQ(array.as_array().size(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(array.as_array()[1].as_integer(), static_cast<std::int64_t>(2));

    const resp_value set = parse_one("~?\r\n+a\r\n+b\r\n.\r\n");
    CRPC_CHECK(set.is_set());
    CRPC_CHECK_EQ(set.as_set().size(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(std::string("b"), set.as_set()[1].as_simple_string());
}

CRPC_TEST(resp_parse_resp3_streamed_map) {
    const resp_value map = parse_one("%?\r\n+a\r\n:1\r\n+b\r\n:2\r\n.\r\n");
    CRPC_CHECK(map.is_map());
    CRPC_CHECK_EQ(map.as_map().size(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(std::string("a"), map.as_map()[0].first.as_simple_string());
    CRPC_CHECK_EQ(map.as_map()[1].second.as_integer(), static_cast<std::int64_t>(2));
}

CRPC_TEST(resp_parse_resp3_streamed_aggregates_nest) {
    const resp_value reply =
        parse_one("*?\r\n$?\r\n;2\r\nab\r\n;0\r\n*?\r\n:7\r\n.\r\n.\r\n");
    CRPC_CHECK(reply.is_array());
    CRPC_CHECK_EQ(reply.as_array().size(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(std::string("ab"), reply.as_array()[0].as_bulk_string());
    CRPC_CHECK(reply.as_array()[1].is_array());
    CRPC_CHECK_EQ(reply.as_array()[1].as_array()[0].as_integer(),
                  static_cast<std::int64_t>(7));
}

CRPC_TEST(resp_parse_resp3_streamed_aggregate_split_at_every_boundary) {
    const std::string text = "*?\r\n:1\r\n%?\r\n+a\r\n:2\r\n.\r\n:3\r\n.\r\n";
    for (std::size_t split = 1; split < text.size(); ++split) {
        resp_parser parser;
        parser.feed(as_span(bytes_of(text.substr(0, split))));
        resp_value value;
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::need_more));
        parser.feed(as_span(bytes_of(text.substr(split))));
        CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                      static_cast<int>(resp_parser::result::value_ready));
        CRPC_CHECK(value.is_array());
        CRPC_CHECK_EQ(value.as_array().size(), static_cast<std::size_t>(3));
        CRPC_CHECK_EQ(value.as_array()[1].as_map()[0].second.as_integer(),
                      static_cast<std::int64_t>(2));
        CRPC_CHECK(parser.finish().ok());
    }
}

CRPC_TEST(resp_parse_resp3_streamed_map_odd_element_count_rejected) {
    // `%?` must emit an even number of elements; a terminator where a value is
    // expected leaves the key without its pair.
    resp_parser parser;
    parser.feed(as_span(bytes_of("%?\r\n+a\r\n.\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parse_resp3_streamed_push_rejected) {
    // `>` has no streamed form in the RESP3 spec.
    resp_parser parser;
    parser.feed(as_span(bytes_of(">?\r\n.\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::error));
    CRPC_CHECK_EQ(static_cast<int>(parser.error()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(resp_parse_resp3_streamed_aggregate_missing_terminator_is_truncated) {
    resp_parser parser;
    parser.feed(as_span(bytes_of("*?\r\n:1\r\n:2\r\n")));
    resp_value value;
    CRPC_CHECK_EQ(static_cast<int>(parser.next(value)),
                  static_cast<int>(resp_parser::result::need_more));
    CRPC_CHECK_FALSE(parser.finish().ok());
}

CRPC_TEST(resp_parse_resp3_streamed_string_carries_no_attributes) {
    // Attributes only attach through `|`; a streamed string never sets them.
    const resp_value reply = parse_one("$?\r\n;3\r\nabc\r\n;0\r\n");
    CRPC_CHECK(reply.is_bulk_string());
    CRPC_CHECK_FALSE(reply.has_attributes());
}

CRPC_TEST(resp_parse_resp3_attribute_augments_streamed_string) {
    const resp_value reply = parse_one("|1\r\n+ttl\r\n:3600\r\n$?\r\n;3\r\nabc\r\n;0\r\n");
    CRPC_CHECK(reply.is_bulk_string());
    CRPC_CHECK_EQ(std::string("abc"), reply.as_bulk_string());
    CRPC_CHECK(reply.has_attributes());
    CRPC_CHECK_EQ(reply.attributes().size(), static_cast<std::size_t>(1));
}

CRPC_TEST(resp_encode_resp3_attribute_round_trips) {
    resp_value reply{resp_simple_string{"OK"}};
    resp_map attributes;
    attributes.emplace_back(resp_value{resp_simple_string{"ttl"}},
                            resp_value{resp_integer{3600}});
    reply.set_attributes(std::move(attributes));

    const std::string encoded = encode_value_text(reply, resp_protocol::resp3);
    CRPC_CHECK_EQ(std::string("|1\r\n+ttl\r\n:3600\r\n+OK\r\n"), encoded);
    CRPC_CHECK(parse_one(encoded) == reply);
}

CRPC_TEST(resp_encode_resp3_attribute_size_matches_bytes) {
    resp_value reply{resp_integer{7}};
    resp_map attributes;
    attributes.emplace_back(resp_value{resp_bulk_string{"k"}},
                            resp_value{resp_double{0.5}});
    reply.set_attributes(std::move(attributes));
    const std::size_t declared = encoded_value_size(reply, resp_protocol::resp3);
    CRPC_CHECK_EQ(declared, encode_value_text(reply, resp_protocol::resp3).size());
}

CRPC_TEST(resp_encode_resp3_attribute_nested_round_trips) {
    resp_value item{resp_integer{3}};
    resp_map attributes;
    attributes.emplace_back(resp_value{resp_simple_string{"ttl"}},
                            resp_value{resp_integer{3600}});
    item.set_attributes(std::move(attributes));
    const resp_value reply{resp_array_value{{resp_integer{1}, item}}};

    const std::string encoded = encode_value_text(reply, resp_protocol::resp3);
    CRPC_CHECK_EQ(std::string("*2\r\n:1\r\n|1\r\n+ttl\r\n:3600\r\n:3\r\n"), encoded);
    CRPC_CHECK(parse_one(encoded) == reply);
}

CRPC_TEST(resp_encode_resp2_drops_attributes) {
    // RESP2 has no attribute frame; the fallback encodes only the value.
    resp_value reply{resp_simple_string{"OK"}};
    resp_map attributes;
    attributes.emplace_back(resp_value{resp_simple_string{"ttl"}},
                            resp_value{resp_integer{3600}});
    reply.set_attributes(std::move(attributes));
    CRPC_CHECK_EQ(std::string("+OK\r\n"),
                  encode_value_text(reply, resp_protocol::resp2));
    CRPC_CHECK_EQ(static_cast<std::size_t>(5),
                  encoded_value_size(reply, resp_protocol::resp2));
}

CRPC_TEST(resp_attribute_equality_distinguishes_attributes) {
    resp_map one;
    one.emplace_back(resp_value{resp_simple_string{"ttl"}}, resp_value{resp_integer{1}});
    resp_map two;
    two.emplace_back(resp_value{resp_simple_string{"ttl"}}, resp_value{resp_integer{2}});

    resp_value a{resp_integer{5}};
    a.set_attributes(std::move(one));
    resp_value b{resp_integer{5}};
    b.set_attributes(std::move(two));
    resp_value bare{resp_integer{5}};

    CRPC_CHECK_FALSE(a == b);
    CRPC_CHECK_FALSE(a == bare);
    CRPC_CHECK(a == a);
    resp_value copy = a;
    CRPC_CHECK(a == copy);
}

CRPC_TEST(resp_encode_resp3_attribute_forbidden_crlf_is_rejected) {
    resp_value reply{resp_integer{1}};
    resp_map attributes;
    attributes.emplace_back(resp_value{resp_simple_string{"bad\r\nkey"}},
                            resp_value{resp_integer{1}});
    reply.set_attributes(std::move(attributes));

    resp_encoder encoder(resp_protocol::resp3);
    bool threw = false;
    try {
        (void)encoder.encode_to_string(reply);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CRPC_CHECK(threw);
}

CRPC_TEST_MAIN()
