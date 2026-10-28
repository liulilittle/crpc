// CRPC -- memcached protocol codec tests.
//
// The codec is a pure function of its input bytes, so every case here drives it
// directly: no socket, no server, no timing. That is deliberate: a framing bug
// must fail here, deterministically, and not as a flaky integration failure.
//
// The cases are grouped by what could break:
//
//   * encoding: byte-exact output, because the server parses what we write;
//   * parsing: a reply is a state machine over a byte stream, so the same reply
//     is fed whole, one byte at a time, and in random splits;
//   * limits: every length is bounded, so an oversized or lying length must
//     produce an error instead of an allocation;
//   * malformed input: a bad token, a missing CRLF and a truncated stream must
//     all fail closed with `protocol_error`.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/memcached/protocol.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::memcached::binary_header;
using crpc::memcached::binary_opcode;
using crpc::memcached::binary_status;
using crpc::memcached::error_kind;
using crpc::memcached::store_status;
using crpc::memcached::text_encoder;
using crpc::memcached::text_parser;
using crpc::memcached::text_reply;

[[nodiscard]] crpc::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

[[nodiscard]] std::string as_text(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Encodes with a generously sized buffer and returns the exact bytes.
template <typename Encoder>
[[nodiscard]] std::string encode(Encoder&& call) {
    std::vector<std::byte> buffer(4096);
    std::size_t written = 0;
    const crpc::status result =
        call(crpc::span<std::byte>(buffer.data(), buffer.size()), written);
    CRPC_CHECK(result.ok());
    CRPC_CHECK(written <= buffer.size());
    buffer.resize(written);
    return as_text(buffer);
}

// Feeds the whole stream in one piece and collects every reply.
[[nodiscard]] std::vector<text_reply> parse_all(std::string_view stream, text_parser& parser) {
    std::vector<text_reply> replies;
    parser.feed(as_bytes(stream));
    for (;;) {
        text_reply reply;
        const text_parser::result step = parser.next(reply);
        if (step == text_parser::result::need_more) {
            break;
        }
        if (step == text_parser::result::error) {
            break;
        }
        replies.push_back(std::move(reply));
    }
    return replies;
}

// Feeds `stream` in chunks of `chunk` bytes, so the parser must survive a reply
// boundary landing anywhere, including inside a CRLF or a value block.
[[nodiscard]] std::vector<text_reply> parse_in_chunks(std::string_view stream, std::size_t chunk,
                                                      text_parser& parser) {
    std::vector<text_reply> replies;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const std::size_t count = std::min(chunk, stream.size() - offset);
        parser.feed(as_bytes(stream.substr(offset, count)));
        offset += count;
        for (;;) {
            text_reply reply;
            const text_parser::result step = parser.next(reply);
            if (step != text_parser::result::reply_ready) {
                break;
            }
            replies.push_back(std::move(reply));
        }
    }
    // Drain anything the final chunk completed.
    for (;;) {
        text_reply reply;
        if (parser.next(reply) != text_parser::result::reply_ready) {
            break;
        }
        replies.push_back(std::move(reply));
    }
    return replies;
}

} // namespace

// --- Encoding ----------------------------------------------------------------

CRPC_TEST(text_encode_storage) {
    const std::string set = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.storage("set", "user:1", 0, 3600, "hello", 0, out, written);
    });
    CRPC_CHECK_EQ(std::string("set user:1 0 3600 5\r\nhello\r\n"), set);

    // A non-zero CAS token adds the sixth field, which only `cas` accepts.
    const std::string cas = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.storage("cas", "k", 7, 0, "v", 4242, out, written);
    });
    CRPC_CHECK_EQ(std::string("cas k 7 0 1 4242\r\nv\r\n"), cas);

    // An empty value is legal and must still emit the zero length.
    const std::string empty = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.storage("add", "k", 0, 0, "", 0, out, written);
    });
    CRPC_CHECK_EQ(std::string("add k 0 0 0\r\n\r\n"), empty);

    // The size estimate must never be smaller than what the encoder writes.
    CRPC_CHECK(text_encoder::storage_size("set", "user:1", "hello", 0) >= set.size());
    CRPC_CHECK(text_encoder::storage_size("cas", "k", "v", 4242) >= cas.size());
}

CRPC_TEST(text_encode_retrieval) {
    const std::vector<std::string_view> keys{"a", "b", "c"};
    const std::string get = encode([&](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.retrieval("get", keys, 0, out, written);
    });
    CRPC_CHECK_EQ(std::string("get a b c\r\n"), get);

    const std::vector<std::string_view> one{"k"};
    const std::string gat = encode([&](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.retrieval("gat", one, 60, out, written);
    });
    CRPC_CHECK_EQ(std::string("gat 60 k\r\n"), gat);

    CRPC_CHECK(text_encoder::retrieval_size("get", keys, 0) >= get.size());
    CRPC_CHECK(text_encoder::retrieval_size("gat", one, 60) >= gat.size());
}

CRPC_TEST(text_encode_mutations) {
    const std::string del = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.remove("k", 0, out, written);
    });
    CRPC_CHECK_EQ(std::string("delete k\r\n"), del);

    const std::string del_cas = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.remove("k", 9, out, written);
    });
    CRPC_CHECK_EQ(std::string("delete k 9\r\n"), del_cas);

    const std::string incr = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.arithmetic("incr", "counter", 3, out, written);
    });
    CRPC_CHECK_EQ(std::string("incr counter 3\r\n"), incr);

    const std::string touch = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.touch("k", 120, out, written);
    });
    CRPC_CHECK_EQ(std::string("touch k 120\r\n"), touch);

    const std::string version = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.simple("version", "", out, written);
    });
    CRPC_CHECK_EQ(std::string("version\r\n"), version);

    const std::string flush = encode([](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.simple("flush_all", "30", out, written);
    });
    CRPC_CHECK_EQ(std::string("flush_all 30\r\n"), flush);
}

CRPC_TEST(text_encode_meta) {
    const std::vector<std::string_view> flags{"v", "f", "t"};
    const std::string mg = encode([&](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.meta("mg", "k", 0, flags, "", out, written);
    });
    CRPC_CHECK_EQ(std::string("mg k v f t\r\n"), mg);

    // `ms` carries the byte count before the flags, then the value block.
    const std::vector<std::string_view> set_flags{"T30", "F0"};
    const std::string ms = encode([&](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.meta("ms", "k", 5, set_flags, "hello", out, written);
    });
    CRPC_CHECK_EQ(std::string("ms k 5 T30 F0\r\nhello\r\n"), ms);

    // `mn` has neither key nor flags.
    const std::vector<std::string_view> none;
    const std::string mn = encode([&](crpc::span<std::byte> out, std::size_t& written) {
        return text_encoder{}.meta("mn", "", 0, none, "", out, written);
    });
    CRPC_CHECK_EQ(std::string("mn\r\n"), mn);
}

CRPC_TEST(text_encode_rejects_bad_input) {
    std::vector<std::byte> buffer(512);
    std::size_t written = 99;
    const std::string too_long(251, 'k');

    // memcached itself rejects a key above 250 bytes, so the encoder does too
    // rather than emitting a request the server would answer with CLIENT_ERROR.
    const crpc::status long_key = text_encoder{}.storage(
        "set", too_long, 0, 0, "v", 0, crpc::span<std::byte>(buffer.data(), buffer.size()),
        written);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(long_key.code()));
    CRPC_CHECK_EQ(std::size_t{0}, written);

    const std::vector<std::string_view> empty_keys;
    const crpc::status no_keys = text_encoder{}.retrieval(
        "get", empty_keys, 0, crpc::span<std::byte>(buffer.data(), buffer.size()), written);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(no_keys.code()));

    // A buffer that cannot hold the frame is an encode error, and `written`
    // stays zero so the caller cannot send a partial command.
    std::vector<std::byte> tiny(4);
    const crpc::status too_small = text_encoder{}.storage(
        "set", "key", 0, 0, "value", 0, crpc::span<std::byte>(tiny.data(), tiny.size()),
        written);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::encode_error),
                  static_cast<int>(too_small.code()));
    CRPC_CHECK_EQ(std::size_t{0}, written);
}

// --- Parsing -----------------------------------------------------------------

CRPC_TEST(text_parse_multi_value) {
    text_parser parser;
    const std::string stream =
        "VALUE user:1 0 5\r\nhello\r\n"
        "VALUE user:2 1 3 77\r\nabc\r\n"
        "END\r\n";
    const std::vector<text_reply> replies = parse_all(stream, parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::values),
                  static_cast<int>(replies[0].type));
    CRPC_CHECK_EQ(std::size_t{2}, replies[0].entries.size());
    CRPC_CHECK_EQ(std::string("user:1"), replies[0].entries[0].key);
    CRPC_CHECK_EQ(std::uint32_t{0}, replies[0].entries[0].flags);
    CRPC_CHECK_EQ(std::uint64_t{0}, replies[0].entries[0].cas);
    CRPC_CHECK_EQ(std::string("hello"), replies[0].entries[0].data);
    CRPC_CHECK_EQ(std::string("user:2"), replies[0].entries[1].key);
    CRPC_CHECK_EQ(std::uint32_t{1}, replies[0].entries[1].flags);
    CRPC_CHECK_EQ(std::uint64_t{77}, replies[0].entries[1].cas);
    CRPC_CHECK_EQ(std::string("abc"), replies[0].entries[1].data);
    CRPC_CHECK(parser.finish().ok());
}

CRPC_TEST(text_parse_value_with_binary_payload) {
    // A value is length-delimited, so CRLF bytes inside it must not end it.
    std::string stream = "VALUE k 0 4\r\n\r\n\r\n\r\nEND\r\n";
    text_parser parser;
    const std::vector<text_reply> replies = parse_all(stream, parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    CRPC_CHECK_EQ(std::size_t{1}, replies[0].entries.size());
    CRPC_CHECK_EQ(std::string("\r\n\r\n", 4), replies[0].entries[0].data);
}

CRPC_TEST(text_parse_empty_value) {
    text_parser parser;
    const std::vector<text_reply> replies = parse_all("VALUE k 0 0\r\n\r\nEND\r\n", parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    CRPC_CHECK_EQ(std::size_t{1}, replies[0].entries.size());
    CRPC_CHECK(replies[0].entries[0].data.empty());
}

CRPC_TEST(text_parse_missing_key) {
    text_parser parser;
    const std::vector<text_reply> replies = parse_all("END\r\n", parser);
    CRPC_CHECK_EQ(std::size_t{0}, replies.size());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(parser.error()));
}

CRPC_TEST(text_parse_bare_end_when_enabled) {
    // The transport enables this for get/gets/gat/gats, where a bare END is the
    // all-miss reply rather than a stray line.
    text_parser parser;
    parser.set_allow_bare_end(true);
    const std::vector<text_reply> replies = parse_all("END\r\n", parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    if (replies.size() == 1) {
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::values),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK(replies[0].entries.empty());
    }
    CRPC_CHECK(parser.finish().ok());
}

CRPC_TEST(text_parse_status_words) {
    struct expected_word {
        const char* text;
        store_status status;
    };
    const expected_word words[] = {
        {"STORED\r\n", store_status::stored},
        {"NOT_STORED\r\n", store_status::not_stored},
        {"EXISTS\r\n", store_status::exists},
        {"NOT_FOUND\r\n", store_status::not_found},
        {"DELETED\r\n", store_status::deleted},
        {"TOUCHED\r\n", store_status::touched},
        {"OK\r\n", store_status::ok},
    };
    for (const expected_word& word : words) {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all(word.text, parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        if (replies.size() != 1) {
            continue;
        }
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::status),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK_EQ(static_cast<int>(word.status), static_cast<int>(replies[0].store));
    }
}

CRPC_TEST(text_parse_errors) {
    {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("ERROR\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK(replies[0].is_error());
        CRPC_CHECK_EQ(static_cast<int>(error_kind::error),
                      static_cast<int>(replies[0].error));
    }
    {
        // The message keeps its spaces: it is the rest of the line, not a token.
        text_parser parser;
        const std::vector<text_reply> replies =
            parse_all("CLIENT_ERROR bad command line format\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK(replies[0].is_error());
        CRPC_CHECK_EQ(static_cast<int>(error_kind::client_error),
                      static_cast<int>(replies[0].error));
        CRPC_CHECK_EQ(std::string("bad command line format"), replies[0].text);
    }
    {
        text_parser parser;
        const std::vector<text_reply> replies =
            parse_all("SERVER_ERROR out of memory storing object\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(error_kind::server_error),
                      static_cast<int>(replies[0].error));
        CRPC_CHECK_EQ(std::string("out of memory storing object"), replies[0].text);
    }
}

CRPC_TEST(text_parse_stats) {
    text_parser parser;
    const std::string stream =
        "STAT pid 1234\r\n"
        "STAT version 1.6.21\r\n"
        "STAT libevent 2.1.12-stable\r\n"
        "END\r\n";
    const std::vector<text_reply> replies = parse_all(stream, parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::stats),
                  static_cast<int>(replies[0].type));
    CRPC_CHECK_EQ(std::size_t{3}, replies[0].stats_entries.size());
    CRPC_CHECK_EQ(std::string("pid"), replies[0].stats_entries[0].first);
    CRPC_CHECK_EQ(std::string("1234"), replies[0].stats_entries[0].second);
    CRPC_CHECK_EQ(std::string("libevent"), replies[0].stats_entries[2].first);
    CRPC_CHECK_EQ(std::string("2.1.12-stable"), replies[0].stats_entries[2].second);
}

CRPC_TEST(text_parse_stats_subcommands) {
    {
        // `stats reset` answers with the single word RESET.
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("RESET\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(store_status::ok),
                      static_cast<int>(replies[0].store));
    }
    {
        // `stats cachedump` answers with ITEM lines terminated by END; the parser
        // folds them into one reply so the caller does not reassemble them.
        text_parser parser;
        const std::string stream =
            "ITEM user:1 [5 b; 0 s]\r\n"
            "ITEM user:2 [3 b; 12 s]\r\n"
            "END\r\n";
        const std::vector<text_reply> replies = parse_all(stream, parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::stats),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK_EQ(std::size_t{2}, replies[0].stats_entries.size());
        CRPC_CHECK_EQ(std::string("user:1"), replies[0].stats_entries[0].first);
        CRPC_CHECK_EQ(std::string("[5 b; 0 s]"), replies[0].stats_entries[0].second);
        CRPC_CHECK_EQ(std::string("[3 b; 12 s]"), replies[0].stats_entries[1].second);
    }
}

CRPC_TEST(text_parse_version_and_counter) {    {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("VERSION 1.6.21\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::version),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK_EQ(std::string("1.6.21"), replies[0].text);
    }
    {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("42\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::number),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK_EQ(std::uint64_t{42}, replies[0].number);
    }
    {
        // The largest counter memcached can report is an unsigned 64-bit value.
        text_parser parser;
        const std::vector<text_reply> replies =
            parse_all("18446744073709551615\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(std::uint64_t{18446744073709551615ull}, replies[0].number);
    }
}

CRPC_TEST(text_parse_meta) {
    {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("HD\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::meta_status),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK_EQ(std::string("HD"), replies[0].meta_tokens.at(0));
    }
    {
        // A meta hit carries flags, then the value block.
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("VA 5 f0 c12\r\nhello\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::meta_hit),
                      static_cast<int>(replies[0].type));
        CRPC_CHECK_EQ(std::string("hello"), replies[0].value);
        CRPC_CHECK_EQ(std::uint64_t{12}, replies[0].meta_cas);
        CRPC_CHECK(replies[0].has_value());
    }
    {
        // A zero-length meta value is still a hit.
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("VA 0\r\n\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK(replies[0].has_value());
        CRPC_CHECK(replies[0].value.empty());
    }
    {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("EN\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(static_cast<int>(text_reply::kind::meta_miss),
                      static_cast<int>(replies[0].type));
    }
    {
        text_parser parser;
        const std::vector<text_reply> replies = parse_all("MN\r\n", parser);
        CRPC_CHECK_EQ(std::size_t{1}, replies.size());
        CRPC_CHECK_EQ(std::string("MN"), replies[0].meta_tokens.at(0));
    }
}

CRPC_TEST(text_parse_sequence) {
    // One stream, many replies: a store, a miss, a get and a counter.
    text_parser parser;
    const std::string stream =
        "STORED\r\n"
        "NOT_FOUND\r\n"
        "VALUE k 0 2\r\nhi\r\nEND\r\n"
        "7\r\n";
    const std::vector<text_reply> replies = parse_all(stream, parser);
    CRPC_CHECK_EQ(std::size_t{4}, replies.size());
    if (replies.size() != 4) {
        return;
    }
    CRPC_CHECK_EQ(static_cast<int>(store_status::stored),
                  static_cast<int>(replies[0].store));
    CRPC_CHECK_EQ(static_cast<int>(store_status::not_found),
                  static_cast<int>(replies[1].store));
    CRPC_CHECK_EQ(std::size_t{1}, replies[2].entries.size());
    CRPC_CHECK_EQ(std::string("hi"), replies[2].entries[0].data);
    CRPC_CHECK_EQ(std::uint64_t{7}, replies[3].number);
    CRPC_CHECK(parser.finish().ok());
}

// --- Fragmentation -----------------------------------------------------------

CRPC_TEST(text_parse_single_byte_feed) {
    // Every reply boundary, every CRLF and the value block must survive a feed
    // that delivers one byte at a time.
    const std::string stream =
        "VALUE a 0 5\r\nhello\r\n"
        "VALUE b 0 0\r\n\r\n"
        "END\r\n"
        "STORED\r\n"
        "VA 2 c9\r\nhi\r\n"
        "STAT pid 1\r\nEND\r\n"
        "CLIENT_ERROR bad\r\n";
    text_parser whole;
    const std::vector<text_reply> expected = parse_all(stream, whole);
    CRPC_CHECK_EQ(std::size_t{5}, expected.size());

    text_parser sliced;
    const std::vector<text_reply> actual = parse_in_chunks(stream, 1, sliced);
    CRPC_CHECK_EQ(expected.size(), actual.size());
    if (expected.size() != actual.size()) {
        return;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(expected[i].type), static_cast<int>(actual[i].type));
        CRPC_CHECK_EQ(expected[i].entries.size(), actual[i].entries.size());
        CRPC_CHECK_EQ(expected[i].stats_entries.size(), actual[i].stats_entries.size());
        CRPC_CHECK_EQ(expected[i].text, actual[i].text);
        CRPC_CHECK_EQ(expected[i].value, actual[i].value);
        CRPC_CHECK_EQ(expected[i].number, actual[i].number);
    }
    CRPC_CHECK(sliced.finish().ok());
}

CRPC_TEST(text_parse_random_chunks) {
    // A deterministic pseudo-random split, so the case is reproducible but does
    // not always cut at the same place.
    const std::string stream =
        "VALUE k1 3 10\r\n0123456789\r\n"
        "END\r\n"
        "STORED\r\n"
        "VERSION 1.6.21\r\n"
        "STAT uptime 99\r\nEND\r\n"
        "18446744073709551615\r\n"
        "VA 4 c1\r\nabcd\r\n";
    text_parser reference;
    const std::vector<text_reply> expected = parse_all(stream, reference);

    std::uint32_t seed = 0x9e3779b9u;
    for (int round = 0; round < 64; ++round) {
        text_parser parser;
        std::vector<text_reply> actual;
        std::size_t offset = 0;
        while (offset < stream.size()) {
            seed = seed * 1664525u + 1013904223u;
            const std::size_t chunk = 1 + (seed % 7u);
            const std::size_t count = std::min(chunk, stream.size() - offset);
            parser.feed(as_bytes(stream.substr(offset, count)));
            offset += count;
            for (;;) {
                text_reply reply;
                if (parser.next(reply) != text_parser::result::reply_ready) {
                    break;
                }
                actual.push_back(std::move(reply));
            }
        }
        CRPC_CHECK_EQ(expected.size(), actual.size());
        if (expected.size() != actual.size()) {
            continue;
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CRPC_CHECK_EQ(static_cast<int>(expected[i].type), static_cast<int>(actual[i].type));
            CRPC_CHECK_EQ(expected[i].number, actual[i].number);
            CRPC_CHECK_EQ(expected[i].value, actual[i].value);
        }
        CRPC_CHECK(parser.finish().ok());
    }
}

// --- Limits and malformed input ----------------------------------------------

CRPC_TEST(text_parse_value_limit) {
    // The declared length exceeds the configured ceiling: the parser must reject
    // it before buffering the payload.
    text_parser parser(8);
    parser.feed(as_bytes("VALUE k 0 9\r\n"));
    text_reply reply;
    CRPC_CHECK_EQ(static_cast<int>(text_parser::result::error),
                  static_cast<int>(parser.next(reply)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(parser.error()));
}

CRPC_TEST(text_parse_value_at_limit) {
    text_parser parser(8);
    const std::vector<text_reply> replies = parse_all("VALUE k 0 8\r\nabcdefgh\r\nEND\r\n", parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    CRPC_CHECK_EQ(std::string("abcdefgh"), replies[0].entries[0].data);
}

CRPC_TEST(text_parse_line_limit) {
    // A reply line with no CRLF that grows past the ceiling must fail instead of
    // buffering without bound.
    text_parser parser(1024, 16);
    const std::string hostile(64, 'A');
    parser.feed(as_bytes(hostile));
    text_reply reply;
    CRPC_CHECK_EQ(static_cast<int>(text_parser::result::error),
                  static_cast<int>(parser.next(reply)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(parser.error()));
}

CRPC_TEST(text_parse_malformed_value_header) {
    struct bad_header {
        const char* text;
    };
    const bad_header cases[] = {
        {"VALUE\r\n"},
        {"VALUE k\r\n"},
        {"VALUE k 0\r\n"},
        {"VALUE k 0 1 2 3\r\n"},
        {"VALUE k x 1\r\n"},
        {"VALUE k 0 y\r\n"},
        {"VALUE k 0 1 z\r\n"},
    };
    for (const bad_header& item : cases) {
        text_parser parser;
        text_reply reply;
        parser.feed(as_bytes(item.text));
        CRPC_CHECK_EQ(static_cast<int>(text_parser::result::error),
                      static_cast<int>(parser.next(reply)));
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(parser.error()));
    }
}

CRPC_TEST(text_parse_value_without_crlf) {
    // The declared length is right but the terminator is not CRLF.
    text_parser parser;
    parser.feed(as_bytes("VALUE k 0 2\r\nabXX"));
    text_reply reply;
    CRPC_CHECK_EQ(static_cast<int>(text_parser::result::error),
                  static_cast<int>(parser.next(reply)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(parser.error()));
}

CRPC_TEST(text_parse_unknown_line) {
    text_parser parser;
    parser.feed(as_bytes("BOGUS reply\r\n"));
    text_reply reply;
    CRPC_CHECK_EQ(static_cast<int>(text_parser::result::error),
                  static_cast<int>(parser.next(reply)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(parser.error()));
}

CRPC_TEST(text_parse_truncated_stream) {
    {
        // Half a line: the parser waits for more bytes.
        text_parser parser;
        parser.feed(as_bytes("STO"));
        text_reply reply;
        CRPC_CHECK_EQ(static_cast<int>(text_parser::result::need_more),
                      static_cast<int>(parser.next(reply)));
        CRPC_CHECK(!parser.finish().ok());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(parser.finish().code()));
    }
    {
        // A complete header with only part of its value.
        text_parser parser;
        parser.feed(as_bytes("VALUE k 0 5\r\nhel"));
        text_reply reply;
        CRPC_CHECK_EQ(static_cast<int>(text_parser::result::need_more),
                      static_cast<int>(parser.next(reply)));
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(parser.finish().code()));
    }
}

CRPC_TEST(text_parser_reset) {
    text_parser parser;
    parser.feed(as_bytes("BOGUS\r\n"));
    text_reply reply;
    CRPC_CHECK_EQ(static_cast<int>(text_parser::result::error),
                  static_cast<int>(parser.next(reply)));
    CRPC_CHECK(parser.failed());

    parser.reset();
    CRPC_CHECK(!parser.failed());
    CRPC_CHECK_EQ(std::size_t{0}, parser.buffered());

    const std::vector<text_reply> replies = parse_all("STORED\r\n", parser);
    CRPC_CHECK_EQ(std::size_t{1}, replies.size());
    CRPC_CHECK_EQ(static_cast<int>(store_status::stored),
                  static_cast<int>(replies[0].store));
}

// --- Binary protocol ---------------------------------------------------------

CRPC_TEST(binary_header_roundtrip) {
    binary_header header;
    header.opcode = static_cast<std::uint8_t>(binary_opcode::get);
    header.key_length = 3;
    header.extras_length = 0;
    header.body_length = 3;
    header.opaque = 0x11223344;
    header.cas = 0x0102030405060708ull;

    std::vector<std::byte> bytes(crpc::memcached::binary_header_size);
    std::size_t written = 0;
    const crpc::status encoded = crpc::memcached::encode_binary_header(
        crpc::span<std::byte>(bytes.data(), bytes.size()), header, written);
    CRPC_CHECK(encoded.ok());
    CRPC_CHECK_EQ(std::size_t{24}, written);
    CRPC_CHECK_EQ(int{0x80}, static_cast<int>(bytes[0]));
    // The binary protocol is big-endian: the first key byte count is high-order.
    CRPC_CHECK_EQ(int{0x00}, static_cast<int>(bytes[2]));
    CRPC_CHECK_EQ(int{0x03}, static_cast<int>(bytes[3]));

    binary_header decoded;
    const crpc::status result = crpc::memcached::decode_binary_header(
        crpc::span<const std::byte>(bytes.data(), bytes.size()), false, decoded);
    CRPC_CHECK(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(binary_opcode::get), static_cast<int>(decoded.opcode));
    CRPC_CHECK_EQ(std::uint16_t{3}, decoded.key_length);
    CRPC_CHECK_EQ(std::uint32_t{3}, decoded.body_length);
    CRPC_CHECK_EQ(std::uint32_t{0x11223344}, decoded.opaque);
    CRPC_CHECK_EQ(std::uint64_t{0x0102030405060708ull}, decoded.cas);
}

CRPC_TEST(binary_header_rejects_bad_magic) {
    std::vector<std::byte> bytes(crpc::memcached::binary_header_size, std::byte{0});
    bytes[0] = std::byte{0x80}; // request magic
    binary_header decoded;
    // Asking for a response against a request header must fail.
    const crpc::status result = crpc::memcached::decode_binary_header(
        crpc::span<const std::byte>(bytes.data(), bytes.size()), true, decoded);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(result.code()));

    std::vector<std::byte> short_header(8);
    const crpc::status truncated = crpc::memcached::decode_binary_header(
        crpc::span<const std::byte>(short_header.data(), short_header.size()), true, decoded);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(truncated.code()));
}

CRPC_TEST(binary_header_rejects_lying_lengths) {
    // A header whose key and extras claim more bytes than the body holds would
    // make the reader index past the frame.
    std::vector<std::byte> bytes(crpc::memcached::binary_header_size, std::byte{0});
    bytes[0] = std::byte{0x81};
    bytes[2] = std::byte{0x00};
    bytes[3] = std::byte{0x20}; // key length 32
    bytes[4] = std::byte{0x08}; // extras length 8
    bytes[8] = std::byte{0x00};
    bytes[9] = std::byte{0x00};
    bytes[10] = std::byte{0x00};
    bytes[11] = std::byte{0x10}; // body length 16

    binary_header decoded;
    const crpc::status result = crpc::memcached::decode_binary_header(
        crpc::span<const std::byte>(bytes.data(), bytes.size()), true, decoded);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(result.code()));
}

CRPC_TEST(binary_status_mapping) {
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(crpc::memcached::map_binary_status(0x0000)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(crpc::memcached::map_binary_status(0x0001)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(crpc::memcached::map_binary_status(0x0002)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(crpc::memcached::map_binary_status(0x0003)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(crpc::memcached::map_binary_status(0x0004)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::no_such_method),
                  static_cast<int>(crpc::memcached::map_binary_status(0x0081)));
    // An unmapped word is an internal failure, and the raw word is still
    // available to the caller for diagnostics.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                  static_cast<int>(crpc::memcached::map_binary_status(0xdead)));
    CRPC_CHECK_EQ(std::string("key_not_found"),
                  std::string(crpc::memcached::binary_status_name(0x0001)));
    CRPC_CHECK(crpc::memcached::binary_status_name(0xdead)[0] == '\0');
}

// --- Helpers -----------------------------------------------------------------

CRPC_TEST(parse_u64_rules) {
    std::uint64_t value = 0;
    CRPC_CHECK(crpc::memcached::parse_u64("0", value));
    CRPC_CHECK_EQ(std::uint64_t{0}, value);
    CRPC_CHECK(crpc::memcached::parse_u64("1234567890", value));
    CRPC_CHECK_EQ(std::uint64_t{1234567890}, value);
    CRPC_CHECK(crpc::memcached::parse_u64("18446744073709551615", value));
    CRPC_CHECK_EQ(std::uint64_t{18446744073709551615ull}, value);

    // Rejected: empty, signed, non-digit, and one past the maximum.
    CRPC_CHECK_FALSE(crpc::memcached::parse_u64("", value));
    CRPC_CHECK_FALSE(crpc::memcached::parse_u64("-1", value));
    CRPC_CHECK_FALSE(crpc::memcached::parse_u64("+1", value));
    CRPC_CHECK_FALSE(crpc::memcached::parse_u64("1a", value));
    CRPC_CHECK_FALSE(crpc::memcached::parse_u64(" 1", value));
    CRPC_CHECK_FALSE(crpc::memcached::parse_u64("18446744073709551616", value));
}

CRPC_TEST(split_tokens_rules) {
    const std::vector<std::string> tokens = crpc::memcached::split_tokens("VA 5 f0 c12");
    CRPC_CHECK_EQ(std::size_t{4}, tokens.size());
    CRPC_CHECK_EQ(std::string("VA"), tokens.at(0));
    CRPC_CHECK_EQ(std::string("c12"), tokens.at(3));

    // Runs of spaces collapse and a trailing space does not create a token.
    const std::vector<std::string> spaced = crpc::memcached::split_tokens("  a   b  ");
    CRPC_CHECK_EQ(std::size_t{2}, spaced.size());
    CRPC_CHECK_EQ(std::string("a"), spaced.at(0));
    CRPC_CHECK_EQ(std::string("b"), spaced.at(1));

    CRPC_CHECK(crpc::memcached::split_tokens("").empty());
    CRPC_CHECK(crpc::memcached::split_tokens("   ").empty());
}

CRPC_TEST_MAIN()