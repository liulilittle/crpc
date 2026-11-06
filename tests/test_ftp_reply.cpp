// CRPC -- FTP reply and command framing tests (RFC 959 sections 4.1 / 4.2).
//
// These cases are pure parser/serialiser checks: no sockets.  The reply tests
// pin the exact multi-line termination rule (only the opening code closes the
// group) and the incremental parse contract; the command tests pin the
// CRLF/NUL injection rejection in both directions.
#include <crpc/ftp/command.hpp>
#include <crpc/ftp/reply.hpp>

#include "test_framework.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

using crpc::ftp::contains_injection;
using crpc::ftp::encode_command;
using crpc::ftp::encode_reply;
using crpc::ftp::ftp_command;
using crpc::ftp::ftp_reply;
using crpc::ftp::is_valid_verb;
using crpc::ftp::parse_command;
using crpc::ftp::parse_reply;
using crpc::ftp::parse_reply_exact;
using crpc::ftp::reply_parse_result;
using crpc::ftp::reply_state;

[[nodiscard]] reply_parse_result parse(std::string_view text) {
    return parse_reply(text);
}

} // namespace

// --- reply ------------------------------------------------------------------

CRPC_TEST(ftp_reply_single_line) {
    const reply_parse_result parsed = parse("220 Service ready\r\n");
    CRPC_CHECK(parsed.state == reply_state::complete);
    CRPC_CHECK_EQ(std::size_t{19}, parsed.consumed);
    CRPC_CHECK_EQ(220, parsed.reply.code);
    CRPC_CHECK_EQ(std::string("Service ready"), parsed.reply.text);
    CRPC_CHECK_FALSE(parsed.reply.is_multiline);
    CRPC_REQUIRE(parsed.reply.lines.size() == 1);
    CRPC_CHECK_EQ(std::string("Service ready"), parsed.reply.lines[0]);
    CRPC_CHECK(parsed.reply.is_positive_completion());
}

CRPC_TEST(ftp_reply_single_line_without_text) {
    const reply_parse_result parsed = parse("200\r\n");
    CRPC_CHECK(parsed.state == reply_state::complete);
    CRPC_CHECK_EQ(200, parsed.reply.code);
    CRPC_CHECK(parsed.reply.text.empty());
    CRPC_REQUIRE(parsed.reply.lines.size() == 1);
    CRPC_CHECK(parsed.reply.lines[0].empty());
}

CRPC_TEST(ftp_reply_multiline_reads_every_line) {
    // The middle line `230 not a terminator` repeats the opening code but uses
    // a space, which per RFC 959 DOES terminate.  The real trap is the other
    // direction: a line with a different code must not terminate the group.
    const std::string wire =
        "211-Features:\r\n"
        " UTF8\r\n"
        "230 Different code inside the group\r\n"
        "211 End\r\n";
    const reply_parse_result parsed = parse(wire);
    CRPC_CHECK(parsed.state == reply_state::complete);
    CRPC_CHECK_EQ(std::size_t{wire.size()}, parsed.consumed);
    CRPC_CHECK_EQ(211, parsed.reply.code);
    CRPC_CHECK(parsed.reply.is_multiline);
    CRPC_REQUIRE(parsed.reply.lines.size() == 4);
    CRPC_CHECK_EQ(std::string("Features:"), parsed.reply.lines[0]);
    CRPC_CHECK_EQ(std::string(" UTF8"), parsed.reply.lines[1]);
    CRPC_CHECK_EQ(std::string("230 Different code inside the group"),
                  parsed.reply.lines[2]);
    CRPC_CHECK_EQ(std::string("End"), parsed.reply.lines[3]);
    CRPC_CHECK_EQ(std::string("Features:\n UTF8\n230 Different code inside the group\nEnd"),
                  parsed.reply.text);
}

CRPC_TEST(ftp_reply_multiline_with_code_prefixed_middle_line) {
    const std::string wire =
        "123-first\r\n"
        "123-middle without a space\r\n"
        "123 last\r\n";
    const reply_parse_result parsed = parse(wire);
    CRPC_CHECK(parsed.state == reply_state::complete);
    CRPC_CHECK_EQ(123, parsed.reply.code);
    CRPC_CHECK(parsed.reply.is_multiline);
    CRPC_REQUIRE(parsed.reply.lines.size() == 3);
    CRPC_CHECK_EQ(std::string("first"), parsed.reply.lines[0]);
    CRPC_CHECK_EQ(std::string("123-middle without a space"), parsed.reply.lines[1]);
    CRPC_CHECK_EQ(std::string("last"), parsed.reply.lines[2]);
}

CRPC_TEST(ftp_reply_incomplete_prefix_is_reported) {
    CRPC_CHECK(parse("").state == reply_state::incomplete);
    CRPC_CHECK(parse("22").state == reply_state::incomplete);
    CRPC_CHECK(parse("220 Serv").state == reply_state::incomplete);
    // No CRLF yet: the line is not finished.
    CRPC_CHECK(parse("220 Service ready").state == reply_state::incomplete);
    // A multi-line group whose terminator has not arrived is incomplete.
    CRPC_CHECK(parse("220-hello\r\nworld\r\n").state == reply_state::incomplete);
    // CRLF split across reads.
    CRPC_CHECK(parse("220 ok\r").state == reply_state::incomplete);
}

CRPC_TEST(ftp_reply_two_replies_consume_only_the_first) {
    const std::string wire = "220 one\r\n331 two\r\n";
    const reply_parse_result parsed = parse(wire);
    CRPC_CHECK(parsed.state == reply_state::complete);
    CRPC_CHECK_EQ(220, parsed.reply.code);
    CRPC_CHECK_EQ(std::size_t{9}, parsed.consumed);
}

CRPC_TEST(ftp_reply_malformed_framing_is_rejected) {
    // Non-digit code.
    CRPC_CHECK(parse("2x0 hi\r\n").state == reply_state::malformed);
    // Only two digits.
    CRPC_CHECK(parse("22 hi\r\n").state == reply_state::malformed);
    // Bad separator.
    CRPC_CHECK(parse("220xhi\r\n").state == reply_state::malformed);
    // Code out of range.
    CRPC_CHECK(parse("999 hi\r\n").state == reply_state::malformed);
    CRPC_CHECK(parse("099 hi\r\n").state == reply_state::malformed);
    // Bare LF instead of CRLF.
    CRPC_CHECK(parse("220 hi\n").state == reply_state::malformed);
    // A different code with a space is ordinary text inside the group: only the
    // opening code followed by a space terminates it, so this reply is still
    // waiting for its terminator.
    CRPC_CHECK(parse("211-hello\r\n212 done\r\n").state == reply_state::incomplete);
    // Empty line inside a group is legal text; an empty buffer is incomplete.
    CRPC_CHECK(parse("211-a\r\n\r\n211 b\r\n").state == reply_state::complete);
}

CRPC_TEST(ftp_reply_exact_rejects_trailing_bytes) {
    auto exact = parse_reply_exact("220 ok\r\n");
    CRPC_CHECK(exact.ok());
    if (exact.ok()) {
        CRPC_CHECK_EQ(220, exact.value().code);
    }
    auto trailing = parse_reply_exact("220 ok\r\n331 more\r\n");
    CRPC_CHECK_FALSE(trailing.ok());
    if (!trailing.ok()) {
        CRPC_CHECK(trailing.code() == crpc::rpc_code::protocol_error);
    }
    auto empty = parse_reply_exact("");
    CRPC_CHECK_FALSE(empty.ok());
    if (!empty.ok()) {
        CRPC_CHECK(empty.code() == crpc::rpc_code::invalid_argument);
    }
    auto junk = parse_reply_exact("hello\r\n");
    CRPC_CHECK_FALSE(junk.ok());
    if (!junk.ok()) {
        CRPC_CHECK(junk.code() == crpc::rpc_code::protocol_error);
    }
}

CRPC_TEST(ftp_reply_encode_single_and_multiline) {
    ftp_reply single;
    single.code = 220;
    single.text = "Ready";
    auto encoded = encode_reply(single);
    CRPC_REQUIRE(encoded.ok());
    CRPC_CHECK_EQ(std::string("220 Ready\r\n"), encoded.value());

    ftp_reply multi;
    multi.code = 211;
    multi.is_multiline = true;
    multi.lines = {"Features:", " UTF8", "End"};
    auto multi_encoded = encode_reply(multi);
    CRPC_REQUIRE(multi_encoded.ok());
    CRPC_CHECK_EQ(std::string("211-Features:\r\n UTF8\r\n211 End\r\n"),
                  multi_encoded.value());

    // Round trip through the parser.
    auto round = parse_reply_exact(multi_encoded.value());
    CRPC_REQUIRE(round.ok());
    CRPC_CHECK_EQ(211, round.value().code);
    CRPC_CHECK(round.value().is_multiline);
    CRPC_REQUIRE(round.value().lines.size() == 3);
    CRPC_CHECK_EQ(std::string("End"), round.value().lines[2]);
}

CRPC_TEST(ftp_reply_encode_rejects_injection_and_bad_codes) {
    ftp_reply injected;
    injected.code = 220;
    injected.text = "hi\r\n331 fake";
    CRPC_CHECK_FALSE(encode_reply(injected).ok());

    ftp_reply nul;
    nul.code = 220;
    nul.text = std::string("a\0b", 3);
    CRPC_CHECK_FALSE(encode_reply(nul).ok());

    ftp_reply bad_code;
    bad_code.code = 42;
    bad_code.text = "x";
    CRPC_CHECK_FALSE(encode_reply(bad_code).ok());

    ftp_reply empty_multi;
    empty_multi.code = 211;
    empty_multi.is_multiline = true;
    empty_multi.lines = {"only one"};
    CRPC_CHECK_FALSE(encode_reply(empty_multi).ok());
}

// --- command ----------------------------------------------------------------

CRPC_TEST(ftp_command_parse_basic) {
    auto parsed = parse_command("USER alice\r\n");
    CRPC_REQUIRE(parsed.ok());
    CRPC_CHECK_EQ(std::string("USER"), parsed.value().verb);
    CRPC_CHECK_EQ(std::string("alice"), parsed.value().argument);

    auto bare = parse_command("PWD\r\n");
    CRPC_REQUIRE(bare.ok());
    CRPC_CHECK_EQ(std::string("PWD"), bare.value().verb);
    CRPC_CHECK(bare.value().argument.empty());

    // Lower-case verbs are canonicalised.
    auto lower = parse_command("retr file name with spaces\r\n");
    CRPC_REQUIRE(lower.ok());
    CRPC_CHECK_EQ(std::string("RETR"), lower.value().verb);
    CRPC_CHECK_EQ(std::string("file name with spaces"), lower.value().argument);
}

CRPC_TEST(ftp_command_parse_rejects_injection_and_junk) {
    CRPC_CHECK_FALSE(parse_command("USER a\r\nPASS b\r\n").ok());
    CRPC_CHECK_FALSE(parse_command("USER a\n").ok());
    CRPC_CHECK_FALSE(parse_command(std::string("USER a\0b\r\n", 11)).ok());
    CRPC_CHECK_FALSE(parse_command("\r\n").ok());
    CRPC_CHECK_FALSE(parse_command("123 x\r\n").ok());
    CRPC_CHECK_FALSE(parse_command("USER\tx\r\n").ok());
    // 4 bytes of verb is legal, 17 is not.
    CRPC_CHECK(parse_command("ABCD\r\n").ok());
    CRPC_CHECK_FALSE(parse_command("ABCDEFGHIJKLMNOPQ\r\n").ok());
}

CRPC_TEST(ftp_command_encode_round_trip) {
    auto encoded = encode_command("retr", "/path/with space.txt");
    CRPC_REQUIRE(encoded.ok());
    CRPC_CHECK_EQ(std::string("RETR /path/with space.txt\r\n"), encoded.value());

    auto bare = encode_command("noop");
    CRPC_REQUIRE(bare.ok());
    CRPC_CHECK_EQ(std::string("NOOP\r\n"), bare.value());

    auto parsed = parse_command(encoded.value());
    CRPC_REQUIRE(parsed.ok());
    CRPC_CHECK_EQ(std::string("RETR"), parsed.value().verb);
    CRPC_CHECK_EQ(std::string("/path/with space.txt"), parsed.value().argument);
}

CRPC_TEST(ftp_command_encode_rejects_crlf) {
    CRPC_CHECK_FALSE(encode_command("RETR", "a\r\nDELE b").ok());
    CRPC_CHECK_FALSE(encode_command("RETR", "a\nDELE b").ok());
    CRPC_CHECK_FALSE(encode_command("RETR", std::string("a\0b", 3)).ok());
    CRPC_CHECK_FALSE(encode_command("RE\rTR", "x").ok());
    CRPC_CHECK_FALSE(encode_command("", "x").ok());
    CRPC_CHECK_FALSE(encode_command("123", "x").ok());
    CRPC_CHECK(contains_injection("a\rb"));
    CRPC_CHECK(contains_injection(std::string("a\0b", 3)));
    CRPC_CHECK_FALSE(contains_injection("plain"));
    CRPC_CHECK(is_valid_verb("RETR"));
    CRPC_CHECK_FALSE(is_valid_verb("RE TR"));
    CRPC_CHECK_FALSE(is_valid_verb(""));
}

CRPC_TEST_MAIN()
