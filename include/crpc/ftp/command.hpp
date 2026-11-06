// CRPC -- FTP command framing (RFC 959 section 4.1 and extensions).
//
// A command is `VERB [SP argument] CRLF`.  The verb is 1..16 ASCII letters and
// is case-insensitive; this layer canonicalises it to upper case.  Everything
// after the first space is the argument and is preserved byte-for-byte: FTP
// path names may contain spaces, and a client that trims them silently renames
// the file the user asked for.
//
// Injection defence
// -----------------
// The argument of a command is attacker-controlled in a server and the command
// text is attacker-controlled in a client.  In both directions a CR, LF or NUL
// byte would let one command smuggle another past a line-oriented parser, so
// parsing and serialisation reject them.  Parsing additionally rejects every
// other C0 control byte: FTP does not give them a meaning and they are the
// vehicle for terminal escapes and log injection.
#ifndef CRPC_FTP_COMMAND_HPP
#define CRPC_FTP_COMMAND_HPP

#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace ftp {

struct CRPC_API ftp_command {
    // Upper-case canonical verb, e.g. "RETR".
    std::string verb;
    // Raw argument; empty when the command carried no argument.
    std::string argument;
};

// True when `text` contains CR, LF or NUL.
[[nodiscard]] CRPC_API bool contains_injection(std::string_view text) noexcept;

// True when `verb` is 1..16 ASCII letters.
[[nodiscard]] CRPC_API bool is_valid_verb(std::string_view verb) noexcept;

// Parses one command line.  A trailing CRLF (or LF) is optional and is
// stripped; any remaining CR/LF/NUL or other C0 control byte is rejected with
// `protocol_error`.  A missing or non-alphabetic verb is also rejected.
[[nodiscard]] CRPC_API result<ftp_command> parse_command(std::string_view line);

// Serialises a command as `VERB[ argument]\r\n`.  The verb is upper-cased; the
// argument is written verbatim.  CR/LF/NUL in either field is rejected with
// `invalid_argument`.
[[nodiscard]] CRPC_API result<std::string> encode_command(
    std::string_view verb, std::string_view argument = {});

} // namespace ftp
} // namespace crpc

#endif // CRPC_FTP_COMMAND_HPP
