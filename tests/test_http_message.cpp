// CRPC -- tests for the HTTP/1.1 message parser and serializer.
//
// The split-at-every-byte tests are the important ones: they exercise the
// incremental state machine at every position where a line ending, a header
// colon or a chunk boundary can fall across two `feed` calls.
#include <crpc/http/message.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::http;

namespace {

span<const std::byte> to_bytes(std::string_view text) {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

parse_status feed_text(http_parser& parser, std::string_view text) {
    return parser.feed(to_bytes(text));
}

void expect_status(parse_status actual, parse_status expected) {
    CRPC_CHECK_EQ(static_cast<int>(expected), static_cast<int>(actual));
}

void expect_code(const rpc_error& error, rpc_code expected) {
    CRPC_CHECK_EQ(static_cast<int>(expected), static_cast<int>(error.code));
}

std::string simple_get() {
    return "GET /hello?x=1 HTTP/1.1\r\n"
           "Host: example.com\r\n"
           "User-Agent: crpc\r\n"
           "\r\n";
}

std::string simple_post() {
    return "POST /submit HTTP/1.1\r\n"
           "Host: example.com\r\n"
           "Content-Length: 5\r\n"
           "\r\n"
           "hello";
}

std::string chunked_request() {
    return "POST /split HTTP/1.1\r\n"
           "Host: example.com\r\n"
           "Transfer-Encoding: chunked\r\n"
           "\r\n"
           "4\r\n"
           "Wiki\r\n"
           "5;ext=1\r\n"
           "pedia\r\n"
           "0\r\n"
           "X-Trailer: yes\r\n"
           "\r\n";
}

} // namespace

CRPC_TEST(http_simple_get_round_trip) {
    http_parser parser;
    expect_status(feed_text(parser, simple_get()), parse_status::message_complete);

    const request& req = parser.get_request();
    CRPC_CHECK(req.method == method::get);
    CRPC_CHECK_EQ(req.method_token, "GET");
    CRPC_CHECK(req.form == target_form::origin);
    CRPC_CHECK_EQ(req.target, "/hello?x=1");
    CRPC_CHECK_EQ(req.path, "/hello");
    CRPC_CHECK_EQ(req.query, "x=1");
    CRPC_CHECK(req.version == http_version::http_1_1);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), req.headers.size());
    CRPC_CHECK(req.headers.contains("host"));
    CRPC_CHECK_FALSE(req.expect_continue);
    CRPC_CHECK(req.body.empty());

    std::string wire;
    CRPC_CHECK(serialize(req, wire).ok());
    CRPC_CHECK(wire.find("Content-Length: 0\r\n") != std::string::npos);

    http_parser second;
    expect_status(feed_text(second, wire), parse_status::message_complete);
    CRPC_CHECK_EQ(second.get_request().path, "/hello");
    CRPC_CHECK_EQ(second.get_request().query, "x=1");
}

CRPC_TEST(http_simple_post_round_trip) {
    http_parser parser;
    expect_status(feed_text(parser, simple_post()), parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().body, "hello");
    CRPC_CHECK_EQ(parser.get_request().headers.find("content-length") != nullptr, true);
    CRPC_CHECK_EQ(*parser.get_request().headers.find("CONTENT-LENGTH"), "5");

    std::string wire;
    CRPC_CHECK(serialize(parser.get_request(), wire).ok());
    http_parser second;
    expect_status(feed_text(second, wire), parse_status::message_complete);
    CRPC_CHECK_EQ(second.get_request().body, "hello");
    CRPC_CHECK_EQ(second.get_request().headers.count("content-length"),
                  static_cast<std::size_t>(1));
}

CRPC_TEST(http_header_case_insensitive_and_duplicates) {
    header_list headers;
    headers.add("Set-Cookie", "a=1");
    headers.add("set-cookie", "b=2");
    headers.add("Content-Type", "text/plain");

    CRPC_CHECK_EQ(static_cast<std::size_t>(3), headers.size());
    CRPC_CHECK_EQ(headers.count("SET-COOKIE"), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(*headers.find("SET-COOKIE"), "a=1");
    CRPC_CHECK_EQ(*headers.find("content-TYPE"), "text/plain");

    const std::vector<std::string_view> all = headers.get_all("Set-Cookie");
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), all.size());
    CRPC_CHECK_EQ(all[0], "a=1");
    CRPC_CHECK_EQ(all[1], "b=2");

    http_parser parser;
    const std::string raw = "GET / HTTP/1.1\r\nX-Dup: one\r\nx-dup: two\r\n\r\n";
    expect_status(feed_text(parser, raw), parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().headers.count("x-dup"),
                  static_cast<std::size_t>(2));
}

CRPC_TEST(http_split_at_every_byte) {
    const std::string inputs[] = {simple_get(), simple_post(), chunked_request()};
    for (const std::string& raw : inputs) {
        for (std::size_t split = 0; split <= raw.size(); ++split) {
            http_parser parser;
            const std::string_view text(raw);
            (void)feed_text(parser, text.substr(0, split));
            const parse_status final_status =
                feed_text(parser, text.substr(split));
            expect_status(final_status, parse_status::message_complete);
            CRPC_CHECK(parser.complete());
        }
    }
}

CRPC_TEST(http_split_one_byte_at_a_time) {
    const std::string raw = chunked_request();
    http_parser parser;
    parse_status status = parse_status::need_more;
    for (char c : raw) {
        status = parser.feed(span<const std::byte>(
            reinterpret_cast<const std::byte*>(&c), 1));
        CRPC_CHECK(status != parse_status::error);
    }
    expect_status(status, parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().body, "Wikipedia");
}

CRPC_TEST(http_chunked_single_chunk) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "4\r\nWiki\r\n0\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().body, "Wiki");
    CRPC_CHECK(parser.get_request().trailers.empty());
}

CRPC_TEST(http_chunked_multiple_chunks_and_extension) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Transfer-Encoding: gzip, chunked\r\n"
                            "\r\n"
                            "3;name=\"a b\"\r\nabc\r\n"
                            "2\r\nde\r\n"
                            "0\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().body, "abcde");
}

CRPC_TEST(http_chunked_trailers) {
    http_parser parser;
    expect_status(feed_text(parser, chunked_request()),
                  parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().body, "Wikipedia");
    CRPC_CHECK_EQ(parser.get_request().trailers.size(), static_cast<std::size_t>(1));
    CRPC_CHECK_EQ(*parser.get_request().trailers.find("x-trailer"), "yes");

    std::string wire;
    CRPC_CHECK(serialize(parser.get_request(), wire).ok());
    CRPC_CHECK(wire.find("0\r\nX-Trailer: yes\r\n\r\n") != std::string::npos);
    http_parser second;
    expect_status(feed_text(second, wire), parse_status::message_complete);
    CRPC_CHECK_EQ(second.get_request().body, "Wikipedia");
    CRPC_CHECK_EQ(*second.get_request().trailers.find("X-Trailer"), "yes");
}

CRPC_TEST(http_chunked_truncated_final_chunk_is_incomplete) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "4\r\nWiki\r\n0\r\n"),
                  parse_status::headers_complete);
    CRPC_CHECK_FALSE(parser.complete());

    http_parser missing_crlf;
    expect_status(feed_text(missing_crlf,
                            "POST / HTTP/1.1\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "4\r\nWiki\r\n0\r\nX: y\r\n"),
                  parse_status::headers_complete);
    CRPC_CHECK_FALSE(missing_crlf.complete());
}

CRPC_TEST(http_chunked_bare_lf_rejected) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "4\nWiki\r\n0\r\n\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_chunked_bad_sizes_rejected) {
    const char* bodies[] = {"zz\r\nx\r\n0\r\n\r\n",
                            "4x\r\nx\r\n0\r\n\r\n",
                            "FFFFFFFFFFFFFFFFFFFFFFFF\r\nx\r\n0\r\n\r\n",
                            "4;=x\r\nWiki\r\n0\r\n\r\n",
                            "4;foo=\r\nWiki\r\n0\r\n\r\n"};

    for (const char* body : bodies) {
        const std::string raw = std::string("POST / HTTP/1.1\r\n"
                                            "Transfer-Encoding: chunked\r\n"
                                            "\r\n") +
                                body;
        http_parser parser;
        expect_status(feed_text(parser, raw), parse_status::error);
        expect_code(parser.error(), rpc_code::protocol_error);
    }
}

CRPC_TEST(http_smuggling_content_length_and_transfer_encoding) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Content-Length: 4\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "0\r\n\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_smuggling_conflicting_content_length) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Content-Length: 4\r\n"
                            "content-length: 5\r\n"
                            "\r\n"
                            "hello"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);

    http_parser duplicate;
    expect_status(feed_text(duplicate,
                            "POST / HTTP/1.1\r\n"
                            "Content-Length: 4\r\n"
                            "Content-Length: 4\r\n"
                            "\r\n"
                            "hello"),
                  parse_status::error);
    expect_code(duplicate.error(), rpc_code::protocol_error);

    http_parser invalid;
    expect_status(feed_text(invalid,
                            "POST / HTTP/1.1\r\n"
                            "Content-Length: +4\r\n"
                            "\r\n"
                            "hello"),
                  parse_status::error);
    expect_code(invalid.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_smuggling_transfer_encoding_must_end_in_chunked) {
    const char* values[] = {"gzip", "chunked, gzip", "identity"};
    for (const char* value : values) {
        const std::string raw = std::string("POST / HTTP/1.1\r\n"
                                            "Transfer-Encoding: ") +
                                value + "\r\n\r\n";
        http_parser parser;
        expect_status(feed_text(parser, raw), parse_status::error);
        expect_code(parser.error(), rpc_code::protocol_error);
    }
}

CRPC_TEST(http_obs_fold_rejected) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "GET / HTTP/1.1\r\n"
                            "X-Test: one\r\n"
                            " two\r\n"
                            "\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_whitespace_before_colon_rejected) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "GET / HTTP/1.1\r\n"
                            "X-Test : one\r\n"
                            "\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_bare_lf_accepted_in_header_block) {
    http_parser parser;
    expect_status(feed_text(parser, "GET /lf HTTP/1.1\nHost: h\nX-A: 1\n\n"),
                  parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().path, "/lf");
    CRPC_CHECK_EQ(*parser.get_request().headers.find("X-A"), "1");
}

CRPC_TEST(http_bare_cr_rejected) {
    http_parser parser;
    expect_status(feed_text(parser, "GET / HTTP/1.1\r\nX: a\rb\r\n\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_unsupported_version) {
    http_parser parser;
    expect_status(feed_text(parser, "GET / HTTP/2.0\r\n\r\n"), parse_status::error);
    expect_code(parser.error(), rpc_code::protocol_error);
    CRPC_CHECK(parser.error().message.find("HTTP/2.0") != std::string::npos);

    http_parser response_parser(http_parser::kind::response);
    expect_status(feed_text(response_parser, "HTTP/9.9 200 OK\r\n\r\n"),
                  parse_status::error);
    expect_code(response_parser.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_limit_request_line) {
    parser_limits limits;
    limits.request_line_bytes = 16;
    http_parser parser(http_parser::kind::request, limits);
    expect_status(feed_text(parser, "GET /aaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(parser.error().message.find("16") != std::string::npos);
}

CRPC_TEST(http_limit_default_request_line) {
    http_parser parser;
    std::string raw = "GET /";
    raw.append(9000, 'a');
    raw.append(" HTTP/1.1");
    expect_status(feed_text(parser, raw), parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(parser.error().message.find("8192") != std::string::npos);
}

CRPC_TEST(http_limit_header_block) {
    parser_limits limits;
    limits.header_block_bytes = 32;
    http_parser parser(http_parser::kind::request, limits);
    expect_status(feed_text(parser,
                            "GET / HTTP/1.1\r\n"
                            "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                            "\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(parser.error().message.find("32") != std::string::npos);
}

CRPC_TEST(http_limit_header_block_boundary) {
    parser_limits limits;
    limits.header_block_bytes = 6; // exactly "X: y\r\n"
    http_parser accepted(http_parser::kind::request, limits);
    expect_status(feed_text(accepted,
                            "GET / HTTP/1.1\r\n"
                            "X: y\r\n"
                            "\r\n"),
                  parse_status::message_complete);

    limits.header_block_bytes = 5;
    http_parser rejected(http_parser::kind::request, limits);
    expect_status(feed_text(rejected,
                            "GET / HTTP/1.1\r\n"
                            "X: y\r\n"
                            "\r\n"),
                  parse_status::error);
    expect_code(rejected.error(), rpc_code::resource_exhausted);
}

CRPC_TEST(http_limit_request_line_boundary) {
    parser_limits limits;
    limits.request_line_bytes = 14; // exactly "GET / HTTP/1.1"
    http_parser accepted(http_parser::kind::request, limits);
    expect_status(feed_text(accepted, "GET / HTTP/1.1\r\n\r\n"),
                  parse_status::message_complete);

    limits.request_line_bytes = 13;
    http_parser rejected(http_parser::kind::request, limits);
    expect_status(feed_text(rejected, "GET / HTTP/1.1\r\n\r\n"),
                  parse_status::error);
    expect_code(rejected.error(), rpc_code::resource_exhausted);
}

CRPC_TEST(http_limit_header_count) {
    parser_limits limits;
    limits.header_count = 2;
    http_parser parser(http_parser::kind::request, limits);
    expect_status(feed_text(parser,
                            "GET / HTTP/1.1\r\n"
                            "A: 1\r\n"
                            "B: 2\r\n"
                            "C: 3\r\n"
                            "\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(parser.error().message.find("header count") != std::string::npos);
}

CRPC_TEST(http_limit_body_content_length) {
    parser_limits limits;
    limits.body_bytes = 4;
    http_parser parser(http_parser::kind::request, limits);
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Content-Length: 5\r\n"
                            "\r\n"
                            "hello"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(parser.error().message.find("4") != std::string::npos);
}

CRPC_TEST(http_limit_body_chunked) {
    parser_limits limits;
    limits.body_bytes = 4;
    http_parser parser(http_parser::kind::request, limits);
    expect_status(feed_text(parser,
                            "POST / HTTP/1.1\r\n"
                            "Transfer-Encoding: chunked\r\n"
                            "\r\n"
                            "5\r\nhello\r\n0\r\n\r\n"),
                  parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
}

// The three chunked framing limits are enforced in src/http/message.cpp but no
// test set them: `chunk_line_bytes`, `trailer_block_bytes` and `trailer_count` had
// code and no coverage. Each is checked at its boundary as well as past it, so the
// limit is shown to be inclusive at the stated size rather than merely "somewhere
// near" it -- the same shape the request-line and header-block limits already use.
CRPC_TEST(http_limit_chunk_line_bytes) {
    parser_limits limits;
    // The limit counts the line content, not the CRLF: one byte for "4".
    limits.chunk_line_bytes = 1;
    http_parser accepted(http_parser::kind::request, limits);
    expect_status(feed_text(accepted,
                             "POST / HTTP/1.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "4\r\nbody\r\n0\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK_EQ(accepted.get_request().body, "body");

    limits.chunk_line_bytes = 0; // one short of the single-byte size line
    http_parser rejected(http_parser::kind::request, limits);
    expect_status(feed_text(rejected,
                             "POST / HTTP/1.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "4\r\nbody\r\n0\r\n\r\n"),
                  parse_status::error);
    expect_code(rejected.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(rejected.error().message.find("chunk line") != std::string::npos);
}

// An oversized chunk extension is the realistic way to exceed the line limit: the
// size itself is one byte, the extension is unbounded in the grammar.
CRPC_TEST(http_limit_chunk_line_bytes_rejects_long_extension) {
    parser_limits limits;
    limits.chunk_line_bytes = 16;
    http_parser parser(http_parser::kind::request, limits);
    std::string wire =
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4;";
    wire.append(64, 'x');
    wire += "\r\nbody\r\n0\r\n\r\n";
    expect_status(feed_text(parser, wire), parse_status::error);
    expect_code(parser.error(), rpc_code::resource_exhausted);
}

CRPC_TEST(http_limit_trailer_count) {
    parser_limits limits;
    limits.trailer_count = 1;
    http_parser accepted(http_parser::kind::request, limits);
    expect_status(feed_text(accepted,
                             "POST / HTTP/1.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "4\r\nbody\r\n0\r\n"
                             "X-One: 1\r\n"
                             "\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK_EQ(accepted.get_request().trailers.size(), static_cast<std::size_t>(1));

    http_parser rejected(http_parser::kind::request, limits);
    expect_status(feed_text(rejected,
                             "POST / HTTP/1.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "4\r\nbody\r\n0\r\n"
                             "X-One: 1\r\n"
                             "X-Two: 2\r\n"
                             "\r\n"),
                  parse_status::error);
    expect_code(rejected.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(rejected.error().message.find("trailer") != std::string::npos);
}

CRPC_TEST(http_limit_trailer_block_bytes) {
    parser_limits limits;
    // The limit counts each trailer line including its CRLF: 8 + 2.
    limits.trailer_block_bytes = 10;
    http_parser accepted(http_parser::kind::request, limits);
    expect_status(feed_text(accepted,
                             "POST / HTTP/1.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "4\r\nbody\r\n0\r\n"
                             "X-One: 1\r\n"
                             "\r\n"),
                  parse_status::message_complete);

    limits.trailer_block_bytes = 9;
    http_parser rejected(http_parser::kind::request, limits);
    expect_status(feed_text(rejected,
                             "POST / HTTP/1.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "4\r\nbody\r\n0\r\n"
                             "X-One: 1\r\n"
                             "\r\n"),
                  parse_status::error);
    expect_code(rejected.error(), rpc_code::resource_exhausted);
    CRPC_CHECK(rejected.error().message.find("trailer block") != std::string::npos);
}

CRPC_TEST(http_expect_100_continue_flag) {
    http_parser parser;
    expect_status(feed_text(parser,
                            "POST /upload HTTP/1.1\r\n"
                            "Expect: 100-continue\r\n"
                            "Content-Length: 0\r\n"
                            "\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(parser.expect_100_continue());
    CRPC_CHECK(parser.get_request().expect_continue);

    http_parser without;
    expect_status(feed_text(without, "POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK_FALSE(without.expect_100_continue());
}

CRPC_TEST(http_method_tokens) {
    http_parser parser;
    expect_status(feed_text(parser, "PATCH / HTTP/1.1\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(parser.get_request().method == method::patch);

    http_parser extension;
    expect_status(feed_text(extension, "PROPFIND / HTTP/1.1\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(extension.get_request().method == method::extension);
    CRPC_CHECK_EQ(extension.get_request().method_token, "PROPFIND");

    http_parser lower;
    expect_status(feed_text(lower, "get / HTTP/1.1\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(lower.get_request().method == method::extension);
    CRPC_CHECK_EQ(lower.get_request().method_token, "get");

    CRPC_CHECK(method_from_token("DELETE") == method::del);
    CRPC_CHECK(method_from_token("delete") == method::extension);
    CRPC_CHECK_EQ(std::string(to_token(method::connect)), "CONNECT");
}

CRPC_TEST(http_target_forms) {
    target_form form = target_form::origin;
    std::string path;
    std::string query;

    CRPC_CHECK(split_target("/a/b?c=d", form, path, query).ok());
    CRPC_CHECK(form == target_form::origin);
    CRPC_CHECK_EQ(path, "/a/b");
    CRPC_CHECK_EQ(query, "c=d");

    CRPC_CHECK(split_target("http://example.com/x?y=1", form, path, query).ok());
    CRPC_CHECK(form == target_form::absolute);
    CRPC_CHECK_EQ(path, "/x");
    CRPC_CHECK_EQ(query, "y=1");

    CRPC_CHECK(split_target("https://example.com", form, path, query).ok());
    CRPC_CHECK(form == target_form::absolute);
    CRPC_CHECK_EQ(path, "/");
    CRPC_CHECK(query.empty());

    CRPC_CHECK(split_target("*", form, path, query).ok());
    CRPC_CHECK(form == target_form::asterisk);
    CRPC_CHECK_EQ(path, "*");

    CRPC_CHECK(split_target("example.com:443", form, path, query).ok());
    CRPC_CHECK(form == target_form::authority);
    CRPC_CHECK(path.empty());

    CRPC_CHECK_FALSE(split_target("/a#frag", form, path, query).ok());
    CRPC_CHECK_FALSE(split_target("", form, path, query).ok());
}

CRPC_TEST(http_method_specific_target_forms) {
    http_parser connect;
    expect_status(feed_text(connect,
                            "CONNECT example.com:443 HTTP/1.1\r\n"
                            "Host: example.com\r\n"
                            "\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(connect.get_request().form == target_form::authority);

    http_parser options;
    expect_status(feed_text(options, "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(options.get_request().form == target_form::asterisk);

    http_parser bad_connect;
    expect_status(feed_text(bad_connect, "CONNECT /x HTTP/1.1\r\n\r\n"),
                  parse_status::error);
    expect_code(bad_connect.error(), rpc_code::protocol_error);

    http_parser bad_authority;
    expect_status(feed_text(bad_authority, "GET example.com:443 HTTP/1.1\r\n\r\n"),
                  parse_status::error);
    expect_code(bad_authority.error(), rpc_code::protocol_error);
}

CRPC_TEST(http_percent_decode) {
    std::string out;
    CRPC_CHECK(percent_decode("%20%2F", out).ok());
    CRPC_CHECK_EQ(out, " /");
    CRPC_CHECK(percent_decode("%2f", out).ok());
    CRPC_CHECK_EQ(out, "/");
    CRPC_CHECK(percent_decode("a+b", out).ok());
    CRPC_CHECK_EQ(out, "a+b");

    const char* invalid[] = {"%zz", "%", "%2", "%0", "%00"};
    for (const char* text : invalid) {
        const status result = percent_decode(text, out);
        CRPC_CHECK_FALSE(result.ok());
        expect_code(result.error(), rpc_code::invalid_argument);
    }

    const std::string with_nul("a\0b", 3);
    CRPC_CHECK_FALSE(percent_decode(std::string_view(with_nul.data(), with_nul.size()),
                                    out)
                         .ok());
}

CRPC_TEST(http_serializer_rejects_header_injection) {
    request req;
    req.method = method::get;
    req.method_token = "GET";
    req.target = "/";
    req.headers.add("X-Test", "a\r\nInjected: 1");

    std::string out = "sentinel";
    const status result = serialize(req, out);
    CRPC_CHECK_FALSE(result.ok());
    expect_code(result.error(), rpc_code::encode_error);
    CRPC_CHECK_EQ(out, "sentinel");

    request name_injection;
    name_injection.method = method::get;
    name_injection.method_token = "GET";
    name_injection.target = "/";
    name_injection.headers.add("Bad Name", "x");
    CRPC_CHECK_FALSE(serialize(name_injection, out).ok());
    CRPC_CHECK_EQ(out, "sentinel");

    request crlf_name;
    crlf_name.method = method::get;
    crlf_name.method_token = "GET";
    crlf_name.target = "/";
    crlf_name.headers.add("X\r\nInjected", "1");
    CRPC_CHECK_FALSE(serialize(crlf_name, out).ok());
    CRPC_CHECK_EQ(out, "sentinel");

    response resp;
    resp.status_code = 200;
    resp.reason = "OK\r\nInjected: 1";
    CRPC_CHECK_FALSE(serialize(resp, out).ok());
    CRPC_CHECK_EQ(out, "sentinel");
}

CRPC_TEST(http_serializer_content_length_mismatch) {
    request req;
    req.method = method::post;
    req.method_token = "POST";
    req.target = "/";
    req.body = "abc";
    req.headers.add("Content-Length", "5");

    std::string out;
    const status result = serialize(req, out);
    CRPC_CHECK_FALSE(result.ok());
    expect_code(result.error(), rpc_code::encode_error);
}

CRPC_TEST(http_serializer_preserves_header_order) {
    request req;
    req.method = method::get;
    req.method_token = "GET";
    req.target = "/";
    req.headers.add("X-First", "1");
    req.headers.add("X-Second", "2");

    std::string wire;
    CRPC_CHECK(serialize(req, wire).ok());
    const std::size_t first = wire.find("X-First: 1");
    const std::size_t second = wire.find("X-Second: 2");
    CRPC_CHECK(first != std::string::npos);
    CRPC_CHECK(second != std::string::npos);
    CRPC_CHECK(first < second);
}

CRPC_TEST(http_response_parse_and_round_trip) {
    http_parser parser(http_parser::kind::response);
    expect_status(feed_text(parser,
                            "HTTP/1.1 404 Not Found\r\n"
                            "Content-Length: 3\r\n"
                            "\r\n"
                            "abc"),
                  parse_status::message_complete);
    const response& resp = parser.get_response();
    CRPC_CHECK(resp.status_code == 404);
    CRPC_CHECK_EQ(resp.reason, "Not Found");
    CRPC_CHECK_EQ(resp.body, "abc");
    CRPC_CHECK(resp.version == http_version::http_1_1);

    std::string wire;
    CRPC_CHECK(serialize(resp, wire).ok());
    http_parser second(http_parser::kind::response);
    expect_status(feed_text(second, wire), parse_status::message_complete);
    CRPC_CHECK(second.get_response().status_code == 404);
    CRPC_CHECK_EQ(second.get_response().body, "abc");
}

CRPC_TEST(http_response_no_body_status) {
    http_parser parser(http_parser::kind::response);
    expect_status(feed_text(parser, "HTTP/1.1 204 No Content\r\n\r\n"),
                  parse_status::message_complete);
    CRPC_CHECK(parser.get_response().body.empty());

    response resp;
    resp.status_code = 204;
    resp.reason = "No Content";
    std::string wire;
    CRPC_CHECK(serialize(resp, wire).ok());
    CRPC_CHECK(wire.find("Content-Length") == std::string::npos);
}

CRPC_TEST(http_serializer_chunked) {
    response resp;
    resp.status_code = 200;
    resp.body = "hello";
    serialize_options options;
    options.force_chunked = true;

    std::string wire;
    CRPC_CHECK(serialize(resp, wire, options).ok());
    CRPC_CHECK(wire.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    CRPC_CHECK(wire.find("5\r\nhello\r\n0\r\n\r\n") != std::string::npos);

    http_parser parser(http_parser::kind::response);
    expect_status(feed_text(parser, wire), parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_response().body, "hello");
}

CRPC_TEST(http_chunked_encoder_decoder_standalone) {
    const std::string body = "abcdefghij";
    chunked_encoder encoder(4);
    std::string wire;
    CRPC_CHECK(encoder.append(to_bytes(body), wire).ok());

    header_list trailers;
    trailers.add("X-Sum", "10");
    CRPC_CHECK(encoder.finish(trailers, wire).ok());
    CRPC_CHECK(wire.find("4\r\nabcd\r\n") != std::string::npos);

    chunked_decoder decoder;
    std::string decoded;
    std::size_t consumed = 0;
    const chunk_status result = decoder.feed(to_bytes(wire), decoded, consumed);
    CRPC_CHECK(result == chunk_status::complete);
    CRPC_CHECK_EQ(decoded, body);
    CRPC_CHECK_EQ(consumed, wire.size());
    CRPC_CHECK_EQ(*decoder.trailers().find("x-sum"), "10");

    std::string truncated = wire.substr(0, wire.size() - 1);
    chunked_decoder partial;
    std::string partial_body;
    std::size_t partial_consumed = 0;
    CRPC_CHECK(partial.feed(to_bytes(truncated), partial_body, partial_consumed) ==
               chunk_status::need_more);
    CRPC_CHECK_FALSE(partial.complete());
}

CRPC_TEST(http_pipelined_pending_bytes) {
    http_parser parser;
    const std::string two = simple_get() + simple_get();
    expect_status(feed_text(parser, two), parse_status::message_complete);
    const span<const std::byte> remaining = parser.pending();
    CRPC_CHECK_EQ(remaining.size(), simple_get().size());
    expect_status(feed_text(parser, std::string_view(
                                        reinterpret_cast<const char*>(remaining.data()),
                                        remaining.size())),
                  parse_status::message_complete);
}

CRPC_TEST(http_pipelined_pending_after_chunked) {
    http_parser parser;
    const std::string two = chunked_request() + simple_get();
    expect_status(feed_text(parser, two), parse_status::message_complete);
    CRPC_CHECK_EQ(parser.get_request().body, "Wikipedia");
    const span<const std::byte> remaining = parser.pending();
    CRPC_CHECK_EQ(remaining.size(), simple_get().size());
    expect_status(feed_text(parser, std::string_view(
                                        reinterpret_cast<const char*>(remaining.data()),
                                        remaining.size())),
                  parse_status::message_complete);
}

CRPC_TEST(http_fuzz_mutations_never_crash) {
    const std::string base = chunked_request();
    std::uint32_t state = 0x12345678u;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        state = state * 1664525u + 1013904223u;
        std::string mutated = base;
        const int mutations = 1 + static_cast<int>(state % 4u);
        for (int m = 0; m < mutations; ++m) {
            state = state * 1664525u + 1013904223u;
            const std::size_t position = state % mutated.size();
            state = state * 1664525u + 1013904223u;
            mutated[position] = static_cast<char>(state & 0xFFu);
        }
        state = state * 1664525u + 1013904223u;
        const std::size_t split = state % (mutated.size() + 1);
        http_parser parser;
        const std::string_view text(mutated);
        (void)feed_text(parser, text.substr(0, split));
        (void)feed_text(parser, text.substr(split));
    }
    CRPC_CHECK(true);
}

CRPC_TEST_MAIN()
