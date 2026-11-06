// CRPC -- FTP reply framing.  See include/crpc/ftp/reply.hpp.
//
// The parser works one line at a time so a caller can feed a socket in whatever
// chunks arrive.  A line is only accepted once its CRLF is present, which makes
// the incremental and the whole-buffer path agree exactly: there is no state
// that depends on how the bytes were split.
#include <crpc/ftp/reply.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace crpc {
namespace ftp {
namespace {

[[nodiscard]] bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Splits off one CRLF-terminated line starting at `offset`.  Returns false when
// the terminator is not present yet.  A bare LF or a CR that is not followed by
// LF is a framing error and is reported through `malformed`.
enum class line_state : unsigned char { ok, need_more, bad };

[[nodiscard]] line_state next_line(std::string_view buffer, std::size_t offset,
                                   std::string& line, std::size_t& next_offset,
                                   std::string& diagnostic) {
    if (offset > buffer.size()) {
        return line_state::need_more;
    }
    const std::size_t cr = buffer.find('\r', offset);
    const std::size_t lf = buffer.find('\n', offset);
    if (lf != std::string_view::npos && (cr == std::string_view::npos || lf < cr)) {
        diagnostic = "bare LF (reply lines must end with CRLF)";
        return line_state::bad;
    }
    if (cr == std::string_view::npos) {
        return line_state::need_more;
    }
    if (cr + 1 >= buffer.size()) {
        return line_state::need_more; // the CR of a split CRLF
    }
    if (buffer[cr + 1] != '\n') {
        diagnostic = "CR is not followed by LF";
        return line_state::bad;
    }
    line.assign(buffer.substr(offset, cr - offset));
    next_offset = cr + 2;
    return line_state::ok;
}

// The numeric code at the start of `line`, or -1 when the three leading bytes
// are not digits in the reply-code range.  FTP codes are 100..599: a three digit
// value below 100 would be written with leading zeros, which RFC 959 does not
// allow and real servers never send.
[[nodiscard]] int leading_code(std::string_view line) noexcept {
    if (line.size() < 3) {
        return -1;
    }
    if (!is_digit(line[0]) || !is_digit(line[1]) || !is_digit(line[2])) {
        return -1;
    }
    const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    if (code < 100 || code > 599) {
        return -1;
    }
    return code;
}

[[nodiscard]] reply_parse_result malformed(std::string message) {
    reply_parse_result result;
    result.state = reply_state::malformed;
    result.diagnostic = std::move(message);
    return result;
}

} // namespace

reply_parse_result parse_reply(std::string_view buffer) {
    reply_parse_result result;

    std::string first;
    std::size_t offset = 0;
    std::string diagnostic;
    const line_state first_state =
        next_line(buffer, offset, first, offset, diagnostic);
    if (first_state == line_state::need_more) {
        return result; // incomplete
    }
    if (first_state == line_state::bad) {
        return malformed(std::move(diagnostic));
    }

    const int code = leading_code(first);
    if (code < 0) {
        return malformed("reply does not start with a three-digit code");
    }
    if (first.size() == 3) {
        // `ddd` alone is a valid single-line reply with empty text.
        result.state = reply_state::complete;
        result.consumed = offset;
        result.reply.code = code;
        result.reply.lines.emplace_back();
        return result;
    }
    const char separator = first[3];
    if (separator == ' ') {
        result.state = reply_state::complete;
        result.consumed = offset;
        result.reply.code = code;
        result.reply.lines.push_back(first.substr(4));
        result.reply.text = result.reply.lines.front();
        return result;
    }
    if (separator != '-') {
        return malformed("the fourth byte of a reply must be '-' or SPACE");
    }

    // Multi-line group: ends at the first line that repeats the opening code
    // followed by a space. A line with a different code is text.
    result.reply.code = code;
    result.reply.is_multiline = true;
    result.reply.lines.push_back(first.substr(4));
    for (;;) {
        std::string line;
        const line_state state =
            next_line(buffer, offset, line, offset, diagnostic);
        if (state == line_state::need_more) {
            return reply_parse_result{}; // incomplete, discard partial work
        }
        if (state == line_state::bad) {
            return malformed(std::move(diagnostic));
        }
        const int line_code = leading_code(line);
        if (line_code == code && line.size() >= 4 && line[3] == ' ') {
            result.reply.lines.push_back(line.substr(4));
            result.state = reply_state::complete;
            result.consumed = offset;
            for (std::size_t i = 0; i < result.reply.lines.size(); ++i) {
                if (i != 0) {
                    result.reply.text.push_back('\n');
                }
                result.reply.text += result.reply.lines[i];
            }
            return result;
        }
        result.reply.lines.push_back(std::move(line));
    }
}

result<ftp_reply> parse_reply_exact(std::string_view text) {
    if (text.empty()) {
        return rpc_error{rpc_code::invalid_argument, "no reply bytes"};
    }
    reply_parse_result parsed = parse_reply(text);
    if (parsed.state == reply_state::incomplete) {
        return rpc_error{rpc_code::protocol_error, "truncated FTP reply"};
    }
    if (parsed.state == reply_state::malformed) {
        return rpc_error{rpc_code::protocol_error,
                         "malformed FTP reply: " + parsed.diagnostic};
    }
    if (parsed.consumed != text.size()) {
        return rpc_error{rpc_code::protocol_error,
                         "trailing bytes after the FTP reply"};
    }
    return std::move(parsed.reply);
}

result<std::string> encode_reply(const ftp_reply& reply) {
    if (reply.code < 100 || reply.code > 599) {
        return rpc_error{rpc_code::invalid_argument, "FTP reply code out of range"};
    }
    std::vector<std::string> lines = reply.lines;
    if (lines.empty()) {
        lines.push_back(reply.text);
    }
    for (const std::string& line : lines) {
        if (line.find('\r') != std::string::npos ||
            line.find('\n') != std::string::npos ||
            line.find('\0') != std::string::npos) {
            return rpc_error{rpc_code::invalid_argument,
                             "FTP reply text contains CR, LF or NUL"};
        }
    }
    if (reply.is_multiline && lines.size() < 2) {
        return rpc_error{rpc_code::invalid_argument,
                         "a multi-line reply needs at least two lines"};
    }

    const std::string code = std::to_string(reply.code);
    std::string out;
    if (!reply.is_multiline) {
        out.reserve(code.size() + 2 + lines[0].size());
        out += code;
        if (!lines[0].empty()) {
            out.push_back(' ');
            out += lines[0];
        }
        out += "\r\n";
        return out;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool first = i == 0;
        const bool last = i + 1 == lines.size();
        if (first || last) {
            out += code;
            out.push_back(last ? ' ' : '-');
        }
        out += lines[i];
        out += "\r\n";
    }
    return out;
}

} // namespace ftp
} // namespace crpc
