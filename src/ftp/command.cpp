// CRPC -- FTP command framing.  See include/crpc/ftp/command.hpp.
#include <crpc/ftp/command.hpp>

#include <cstddef>

namespace crpc {
namespace ftp {
namespace {

[[nodiscard]] bool is_alpha(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] char as_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Every C0 control byte, DEL, and NUL is rejected.  CR and LF are the injection
// vector; the rest are rejected because FTP gives them no meaning and they are
// how an escape sequence or a forged log line gets in.
[[nodiscard]] bool has_control(std::string_view text) noexcept {
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

} // namespace

bool contains_injection(std::string_view text) noexcept {
    return text.find('\r') != std::string_view::npos ||
           text.find('\n') != std::string_view::npos ||
           text.find('\0') != std::string_view::npos;
}

bool is_valid_verb(std::string_view verb) noexcept {
    if (verb.empty() || verb.size() > 16) {
        return false;
    }
    for (const char c : verb) {
        if (!is_alpha(c)) {
            return false;
        }
    }
    return true;
}

result<ftp_command> parse_command(std::string_view line) {
    // Only a complete CRLF is stripped.  A bare LF or a lone CR is a framing
    // error: a line-oriented parser that guesses at the terminator is exactly
    // how one command becomes two.
    if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
        line.remove_suffix(2);
    } else if (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r')) {
        return rpc_error{rpc_code::protocol_error,
                         "FTP command is not terminated by CRLF"};
    }
    if (line.empty()) {
        return rpc_error{rpc_code::protocol_error, "empty FTP command"};
    }
    if (has_control(line)) {
        return rpc_error{rpc_code::protocol_error,
                         "FTP command contains a control byte"};
    }

    const std::size_t space = line.find(' ');
    const std::string_view raw_verb =
        space == std::string_view::npos ? line : line.substr(0, space);
    if (!is_valid_verb(raw_verb)) {
        return rpc_error{rpc_code::protocol_error, "invalid FTP command verb"};
    }

    ftp_command parsed;
    parsed.verb.reserve(raw_verb.size());
    for (const char c : raw_verb) {
        parsed.verb.push_back(as_upper(c));
    }
    if (space != std::string_view::npos) {
        parsed.argument.assign(line.substr(space + 1));
    }
    return parsed;
}

result<std::string> encode_command(std::string_view verb, std::string_view argument) {
    if (!is_valid_verb(verb)) {
        return rpc_error{rpc_code::invalid_argument, "invalid FTP command verb"};
    }
    if (has_control(argument)) {
        return rpc_error{rpc_code::invalid_argument,
                         "FTP command argument contains a control byte"};
    }
    std::string out;
    out.reserve(verb.size() + 1 + argument.size() + 2);
    for (const char c : verb) {
        out.push_back(as_upper(c));
    }
    if (!argument.empty()) {
        out.push_back(' ');
        out.append(argument.data(), argument.size());
    }
    out += "\r\n";
    return out;
}

} // namespace ftp
} // namespace crpc
