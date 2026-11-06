// CRPC -- FTP reply framing (RFC 959 section 4.2).
//
// The control connection carries CRLF-terminated lines.  A reply is either one
// line `ddd<SP>text` or a multi-line group:
//
//   ddd-line one
//   line two
//   ddd line three
//
// The first three bytes are always a decimal code.  The fourth byte selects the
// shape: '-' opens a multi-line reply, a space (or the end of the line) closes a
// single-line reply.  A multi-line reply ends at the first line whose first
// three bytes repeat the opening code and whose fourth byte is a space.  Inside
// the group, a line that looks like a different reply code is ordinary text;
// only the opening code terminates it.  That rule is what makes a banner such
// as `220-...\r\n220 ready\r\n` unambiguous, and it is enforced strictly here:
// a group that ends with the wrong code is a protocol error, never a "close
// enough" success.
//
// Parsing is incremental.  `parse_reply` inspects a buffer and reports one of
// three outcomes: complete, incomplete, or malformed.  A caller reading from a
// socket appends lines and retries until the state is complete; a caller with a
// whole reply in hand uses `parse_reply_exact`, which additionally rejects
// trailing bytes so a desynchronised stream cannot be mistaken for a valid
// reply followed by silence.
//
// Fields
// ------
//   * `code`        the numeric reply code (100..599)
//   * `is_multiline` true when the first line used the '-' separator
//   * `lines`       one entry per wire line, with the `ddd-` / `ddd ` prefix
//                   removed from the first and last line; middle lines are
//                   kept verbatim
//   * `text`        `lines` joined with '\n'
#ifndef CRPC_FTP_REPLY_HPP
#define CRPC_FTP_REPLY_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace ftp {

struct CRPC_API ftp_reply {
    int code = 0;
    std::string text;
    bool is_multiline = false;
    std::vector<std::string> lines;

    // Reply classes (RFC 959 section 4.2.1).
    [[nodiscard]] bool is_positive_preliminary() const noexcept {
        return code >= 100 && code < 200;
    }
    [[nodiscard]] bool is_positive_completion() const noexcept {
        return code >= 200 && code < 300;
    }
    [[nodiscard]] bool is_positive_intermediate() const noexcept {
        return code >= 300 && code < 400;
    }
    [[nodiscard]] bool is_transient_negative() const noexcept {
        return code >= 400 && code < 500;
    }
    [[nodiscard]] bool is_permanent_negative() const noexcept {
        return code >= 500 && code < 600;
    }
};

// The outcome of feeding a buffer to the incremental parser.
enum class reply_state : unsigned char {
    incomplete, // a valid prefix; more bytes are needed
    complete,   // one whole reply is present
    malformed   // the bytes cannot become a valid reply
};

struct CRPC_API reply_parse_result {
    reply_state state = reply_state::incomplete;
    // Bytes consumed on `complete`; the parser never stops mid-line.
    std::size_t consumed = 0;
    ftp_reply reply;
    // Human-readable reason when `state == malformed`.
    std::string diagnostic;
};

// Parses one reply from the front of `buffer`.  The buffer may hold a prefix of
// a reply (incomplete), a whole reply plus further bytes (consumed tells the
// caller how much belongs to this reply), or junk.  Every line must end with
// CRLF; a bare LF or CR is malformed because a peer that does not frame lines
// cannot be resynchronised safely.
[[nodiscard]] CRPC_API reply_parse_result parse_reply(std::string_view buffer);

// Parses exactly one reply and rejects trailing bytes.  Error codes:
//   * `protocol_error`  malformed framing, bad code, or trailing bytes
//   * `invalid_argument` an empty buffer
[[nodiscard]] CRPC_API result<ftp_reply> parse_reply_exact(std::string_view text);

// Encodes a reply as wire bytes, always ending in CRLF.  Multi-line replies
// require at least two `lines`; `text` is used when `lines` is empty.
// A CR, LF or NUL inside any line is rejected with `invalid_argument` so that a
// server can never emit a reply that injects a second reply.
[[nodiscard]] CRPC_API result<std::string> encode_reply(const ftp_reply& reply);

} // namespace ftp
} // namespace crpc

#endif // CRPC_FTP_REPLY_HPP
