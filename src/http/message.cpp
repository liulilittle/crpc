// CRPC -- HTTP/1.1 message parser and serializer implementation.
#include <crpc/http/message.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace crpc::http {
namespace {

// --- Small ASCII helpers -----------------------------------------------------

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ci_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ows(std::string_view text) noexcept {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool is_alpha(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_tchar(char c) noexcept {
    if (is_alpha(c) || is_digit(c)) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

// field-value = *( field-content / obs-fold ); we accept visible ASCII,
// obs-text and the horizontal tab, and reject every other control byte. In
// particular CR, LF and NUL can never reach the serializer through this check.
bool valid_field_value(std::string_view value) noexcept {
    for (char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == 0x09u || u == 0x20u || (u >= 0x21u && u <= 0x7Eu) || u >= 0x80u) {
            continue;
        }
        return false;
    }
    return true;
}

bool valid_reason_phrase(std::string_view value) noexcept {
    return valid_field_value(value);
}

bool is_hex_digit(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

unsigned hex_value(char c) noexcept {
    if (is_digit(c)) {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    return static_cast<unsigned>(c - 'A' + 10);
}

std::string limit_message(const char* what, std::size_t limit) {
    return std::string(what) + " exceeds limit of " + std::to_string(limit) + " bytes";
}

bool valid_scheme(std::string_view scheme) noexcept {
    if (scheme.empty() || !is_alpha(scheme.front())) {
        return false;
    }
    for (char c : scheme) {
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

bool parse_version(std::string_view text, http_version& out) noexcept {
    if (text == "HTTP/1.1") {
        out = http_version::http_1_1;
        return true;
    }
    if (text == "HTTP/1.0") {
        out = http_version::http_1_0;
        return true;
    }
    return false;
}

// Classifies a request target and splits the raw path from the raw query. It
// does not apply method-specific rules; the parser does that after calling it.
bool classify_target(std::string_view target, target_form& form, std::string& path,
                     std::string& query, rpc_error& err) {
    if (target.empty()) {
        err = rpc_error(rpc_code::protocol_error, "empty request target");
        return false;
    }
    for (char c : target) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20u || u == 0x7Fu) {
            err = rpc_error(rpc_code::protocol_error,
                            "request target contains whitespace or a control character");
            return false;
        }
        if (c == '#') {
            err = rpc_error(rpc_code::protocol_error,
                            "request target must not contain a fragment");
            return false;
        }
    }

    if (target == "*") {
        form = target_form::asterisk;
        path.assign("*");
        query.clear();
        return true;
    }

    if (target.front() == '/') {
        form = target_form::origin;
        const std::size_t question = target.find('?');
        path.assign(target.substr(0, question));
        if (question == std::string_view::npos) {
            query.clear();
        } else {
            query.assign(target.substr(question + 1));
        }
        return true;
    }

    const std::size_t scheme_end = target.find("://");
    if (scheme_end != std::string_view::npos) {
        if (!valid_scheme(target.substr(0, scheme_end))) {
            err = rpc_error(rpc_code::protocol_error, "invalid scheme in absolute-form target");
            return false;
        }
        form = target_form::absolute;
        const std::size_t authority_start = scheme_end + 3;
        const std::size_t stop = target.find_first_of("/?", authority_start);
        const std::string_view authority =
            target.substr(authority_start, stop == std::string_view::npos
                                               ? std::string_view::npos
                                               : stop - authority_start);
        if (authority.empty()) {
            err = rpc_error(rpc_code::protocol_error,
                            "absolute-form target has an empty authority");
            return false;
        }
        if (stop == std::string_view::npos) {
            path.assign("/");
            query.clear();
            return true;
        }
        if (target[stop] == '?') {
            path.assign("/");
            query.assign(target.substr(stop + 1));
            return true;
        }
        const std::size_t question = target.find('?', stop);
        path.assign(target.substr(stop, question == std::string_view::npos
                                            ? std::string_view::npos
                                            : question - stop));
        if (question == std::string_view::npos) {
            query.clear();
        } else {
            query.assign(target.substr(question + 1));
        }
        return true;
    }

    // authority-form: uri-host ":" port. Anything else that is neither
    // origin-form nor absolute-form is rejected here; the parser additionally
    // requires that the method is CONNECT.
    if (target.find('/') != std::string_view::npos ||
        target.find('?') != std::string_view::npos) {
        err = rpc_error(rpc_code::protocol_error, "invalid request target form");
        return false;
    }
    const std::size_t colon = target.rfind(':');
    if (colon == std::string_view::npos || colon == 0 ||
        colon + 1 == target.size()) {
        err = rpc_error(rpc_code::protocol_error,
                        "authority-form target must contain a host and a port");
        return false;
    }
    for (std::size_t i = colon + 1; i < target.size(); ++i) {
        if (!is_digit(target[i])) {
            err = rpc_error(rpc_code::protocol_error,
                            "authority-form target has a non-numeric port");
            return false;
        }
    }
    form = target_form::authority;
    path.clear();
    query.clear();
    return true;
}

bool parse_decimal(std::string_view text, std::size_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::size_t value = 0;
    for (char c : text) {
        if (!is_digit(c)) {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        if (value > (maximum - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

// --- Framing analysis --------------------------------------------------------

struct framing_info {
    bool chunked = false;
    bool has_cl = false;
    std::size_t content_length = 0;
};

// Implements the request-smuggling defenses shared by the parser and the
// serializer:
//   * Content-Length and Transfer-Encoding together are rejected,
//   * repeated Content-Length fields are rejected,
//   * a Transfer-Encoding chain must end in "chunked",
//   * "chunked" may not appear anywhere but last and takes no parameters.
bool analyze_framing(const header_list& headers, std::size_t body_limit,
                     framing_info& info, rpc_error& err) {
    std::vector<std::string_view> content_lengths;
    std::vector<std::string_view> transfer_encodings;
    for (const header& field : headers) {
        if (ci_equal(field.name, "content-length")) {
            content_lengths.push_back(field.value);
        } else if (ci_equal(field.name, "transfer-encoding")) {
            transfer_encodings.push_back(field.value);
        }
    }

    if (!content_lengths.empty() && !transfer_encodings.empty()) {
        err = rpc_error(rpc_code::protocol_error,
                        "Content-Length and Transfer-Encoding must not be combined");
        return false;
    }

    if (content_lengths.size() > 1) {
        err = rpc_error(rpc_code::protocol_error,
                        "multiple Content-Length values are not allowed");
        return false;
    }

    if (content_lengths.size() == 1) {
        const std::string_view text = trim_ows(content_lengths.front());
        if (!parse_decimal(text, info.content_length)) {
            err = rpc_error(rpc_code::protocol_error, "invalid Content-Length value");
            return false;
        }
        info.has_cl = true;
        if (info.content_length > body_limit) {
            err = rpc_error(rpc_code::resource_exhausted,
                            limit_message("body", body_limit));
            return false;
        }
    }

    if (!transfer_encodings.empty()) {
        struct coding {
            std::string_view token;
            bool has_parameters;
        };
        std::vector<coding> codings;
        for (const std::string_view value : transfer_encodings) {
            std::size_t start = 0;
            for (;;) {
                const std::size_t comma = value.find(',', start);
                const std::string_view item =
                    trim_ows(value.substr(start, comma == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : comma - start));
                if (item.empty()) {
                    err = rpc_error(rpc_code::protocol_error,
                                    "empty transfer coding");
                    return false;
                }
                const std::size_t semicolon = item.find(';');
                const std::string_view token =
                    trim_ows(item.substr(0, semicolon));
                if (!is_token(token)) {
                    err = rpc_error(rpc_code::protocol_error,
                                    "invalid transfer coding token");
                    return false;
                }
                codings.push_back({token, semicolon != std::string_view::npos});
                if (comma == std::string_view::npos) {
                    break;
                }
                start = comma + 1;
            }
        }

        for (std::size_t i = 0; i < codings.size(); ++i) {
            if (!ci_equal(codings[i].token, "chunked")) {
                continue;
            }
            if (i + 1 != codings.size()) {
                err = rpc_error(rpc_code::protocol_error,
                                "chunked must be the final transfer coding");
                return false;
            }
            if (codings[i].has_parameters) {
                err = rpc_error(rpc_code::protocol_error,
                                "chunked does not take parameters");
                return false;
            }
        }
        if (!ci_equal(codings.back().token, "chunked")) {
            err = rpc_error(rpc_code::protocol_error,
                            "Transfer-Encoding must end in chunked");
            return false;
        }
        info.chunked = true;
    }

    return true;
}

bool status_forbids_body(int status_code) noexcept {
    return (status_code >= 100 && status_code < 200) || status_code == 204 ||
           status_code == 304;
}

bool forbidden_trailer(std::string_view name) noexcept {
    return ci_equal(name, "content-length") || ci_equal(name, "transfer-encoding") ||
           ci_equal(name, "host");
}

// Parses one header field line. The line has already had its CRLF removed.
bool parse_field_line(std::string_view line, header& out, rpc_error& err) {
    if (line.empty()) {
        err = rpc_error(rpc_code::protocol_error, "empty header field line");
        return false;
    }
    if (line.front() == ' ' || line.front() == '\t') {
        err = rpc_error(rpc_code::protocol_error,
                        "obsolete line folding is not permitted");
        return false;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        err = rpc_error(rpc_code::protocol_error, "header field without colon");
        return false;
    }
    const std::string_view name = line.substr(0, colon);
    if (!is_token(name)) {
        err = rpc_error(rpc_code::protocol_error, "invalid header field name");
        return false;
    }
    const std::string_view value = trim_ows(line.substr(colon + 1));
    if (!valid_field_value(value)) {
        err = rpc_error(rpc_code::protocol_error, "invalid header field value");
        return false;
    }
    out.name.assign(name);
    out.value.assign(value);
    return true;
}

const char* default_reason(int status_code) noexcept {
    switch (status_code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 418: return "I'm a teapot";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

// --- Chunk extension validation ----------------------------------------------

bool valid_quoted_string(std::string_view text, std::size_t& pos) noexcept {
    // `text[pos]` is the opening DQUOTE.
    ++pos;
    while (pos < text.size()) {
        const char c = text[pos];
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"') {
            ++pos;
            return true;
        }
        if (c == '\\') {
            ++pos;
            if (pos >= text.size()) {
                return false;
            }
            const unsigned char escaped = static_cast<unsigned char>(text[pos]);
            if (escaped != 0x09u && escaped != 0x20u &&
                !(escaped >= 0x21u && escaped <= 0x7Eu) && escaped < 0x80u) {
                return false;
            }
            ++pos;
            continue;
        }
        if (u == 0x09u || u == 0x20u || u == 0x21u ||
            (u >= 0x23u && u <= 0x5Bu) || (u >= 0x5Du && u <= 0x7Eu) || u >= 0x80u) {
            ++pos;
            continue;
        }
        return false;
    }
    return false;
}

bool valid_chunk_extensions(std::string_view text) noexcept {
    // `text` is everything after the first ';'.
    if (text.empty()) {
        return false;
    }
    std::size_t pos = 0;
    for (;;) {
        const std::size_t name_begin = pos;
        while (pos < text.size() && is_tchar(text[pos])) {
            ++pos;
        }
        if (pos == name_begin) {
            return false;
        }
        if (pos < text.size() && text[pos] == '=') {
            ++pos;
            if (pos >= text.size()) {
                return false;
            }
            if (text[pos] == '"') {
                if (!valid_quoted_string(text, pos)) {
                    return false;
                }
            } else {
                const std::size_t value_begin = pos;
                while (pos < text.size() && is_tchar(text[pos])) {
                    ++pos;
                }
                if (pos == value_begin) {
                    return false;
                }
            }
        }
        if (pos == text.size()) {
            return true;
        }
        if (text[pos] != ';') {
            return false;
        }
        ++pos;
        if (pos == text.size()) {
            return false;
        }
    }
}

void append_hex(std::string& out, std::size_t value) {
    char digits[2 * sizeof(std::size_t)];
    std::size_t count = 0;
    do {
        const unsigned nibble = static_cast<unsigned>(value & 0xFu);
        digits[count++] = static_cast<char>(nibble < 10u ? ('0' + nibble)
                                                         : ('a' + nibble - 10u));
        value >>= 4u;
    } while (value != 0);
    while (count > 0) {
        out.push_back(digits[--count]);
    }
}

status append_header_line(std::string& out, std::string_view name,
                          std::string_view value) {
    if (!is_token(name)) {
        return status(rpc_error(rpc_code::encode_error, "invalid header field name"));
    }
    if (!valid_field_value(value)) {
        return status(rpc_error(rpc_code::encode_error, "invalid header field value"));
    }
    out.append(name.data(), name.size());
    out.append(": ", 2);
    out.append(value.data(), value.size());
    out.append("\r\n", 2);
    return status();
}

} // namespace

// --- Method and version helpers ----------------------------------------------

method method_from_token(std::string_view token) noexcept {
    if (token == "GET") return method::get;
    if (token == "HEAD") return method::head;
    if (token == "POST") return method::post;
    if (token == "PUT") return method::put;
    if (token == "DELETE") return method::del;
    if (token == "PATCH") return method::patch;
    if (token == "OPTIONS") return method::options;
    if (token == "TRACE") return method::trace;
    if (token == "CONNECT") return method::connect;
    return method::extension;
}

const char* to_token(method value) noexcept {
    switch (value) {
    case method::get: return "GET";
    case method::head: return "HEAD";
    case method::post: return "POST";
    case method::put: return "PUT";
    case method::del: return "DELETE";
    case method::patch: return "PATCH";
    case method::options: return "OPTIONS";
    case method::trace: return "TRACE";
    case method::connect: return "CONNECT";
    case method::extension: return "";
    }
    return "";
}

bool is_token(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (!is_tchar(c)) {
            return false;
        }
    }
    return true;
}

const char* to_string(http_version version) noexcept {
    switch (version) {
    case http_version::http_1_0:
        return "HTTP/1.0";
    case http_version::http_2:
        return "HTTP/2";
    case http_version::http_1_1:
        break;
    }
    return "HTTP/1.1";
}

status split_target(std::string_view target, target_form& form, std::string& path,
                    std::string& query) {
    rpc_error err;
    if (!classify_target(target, form, path, query, err)) {
        return status(std::move(err));
    }
    return status();
}

status percent_decode(std::string_view input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '\0') {
            return status(rpc_error(rpc_code::invalid_argument,
                                    "percent-decoded input contains a NUL byte"));
        }
        if (c != '%') {
            output.push_back(c);
            continue;
        }
        if (i + 2 >= input.size() || !is_hex_digit(input[i + 1]) ||
            !is_hex_digit(input[i + 2])) {
            return status(rpc_error(rpc_code::invalid_argument,
                                    "invalid percent escape"));
        }
        const unsigned value =
            (hex_value(input[i + 1]) << 4u) | hex_value(input[i + 2]);
        if (value == 0u) {
            return status(rpc_error(rpc_code::invalid_argument,
                                    "percent-decoded input contains a NUL byte"));
        }
        output.push_back(static_cast<char>(value));
        i += 2;
    }
    return status();
}

// --- Header list -------------------------------------------------------------

void header_list::add(std::string_view name, std::string_view value) {
    entries_.push_back(header{std::string(name), std::string(value)});
}

const std::string* header_list::find(std::string_view name) const noexcept {
    for (const header& field : entries_) {
        if (ci_equal(field.name, name)) {
            return &field.value;
        }
    }
    return nullptr;
}

bool header_list::contains(std::string_view name) const noexcept {
    return find(name) != nullptr;
}

std::size_t header_list::count(std::string_view name) const noexcept {
    std::size_t total = 0;
    for (const header& field : entries_) {
        if (ci_equal(field.name, name)) {
            ++total;
        }
    }
    return total;
}

std::vector<std::string_view> header_list::get_all(std::string_view name) const {
    std::vector<std::string_view> values;
    for (const header& field : entries_) {
        if (ci_equal(field.name, name)) {
            values.push_back(field.value);
        }
    }
    return values;
}

// --- Chunked decoder ---------------------------------------------------------

chunked_decoder::chunked_decoder(parser_limits limits) : limits_(limits) {}

void chunked_decoder::reset() {
    state_ = state::size;
    line_.clear();
    chunk_remaining_ = 0;
    body_size_ = 0;
    trailer_bytes_ = 0;
    saw_cr_ = false;
    crlf_state_ = 0;
    trailers_.clear();
    error_ = rpc_error();
}

bool chunked_decoder::fail(rpc_code code, std::string message) {
    error_ = rpc_error(code, std::move(message));
    state_ = state::failed;
    return false;
}

bool chunked_decoder::finish_size_line() {
    const std::string_view line(line_);
    const std::size_t semicolon = line.find(';');
    const std::string_view size_text = line.substr(0, semicolon);
    if (size_text.empty()) {
        return fail(rpc_code::protocol_error, "empty chunk size");
    }
    std::size_t size = 0;
    for (char c : size_text) {
        if (!is_hex_digit(c)) {
            return fail(rpc_code::protocol_error, "invalid chunk size");
        }
        const std::size_t digit = static_cast<std::size_t>(hex_value(c));
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        if (size > (maximum - digit) / 16u) {
            return fail(rpc_code::protocol_error, "chunk size overflows");
        }
        size = size * 16u + digit;
    }
    if (semicolon != std::string_view::npos &&
        !valid_chunk_extensions(line.substr(semicolon + 1))) {
        return fail(rpc_code::protocol_error, "malformed chunk extension");
    }
    line_.clear();

    if (size == 0) {
        state_ = state::trailers;
        trailer_bytes_ = 0;
        return true;
    }

    if (size > limits_.body_bytes - body_size_) {
        return fail(rpc_code::resource_exhausted,
                    limit_message("body", limits_.body_bytes));
    }
    chunk_remaining_ = size;
    state_ = state::data;
    return true;
}

bool chunked_decoder::finish_trailer_line() {
    if (line_.empty()) {
        state_ = state::done;
        return true;
    }
    trailer_bytes_ += line_.size() + 2u;
    if (trailer_bytes_ > limits_.trailer_block_bytes) {
        return fail(rpc_code::resource_exhausted,
                    limit_message("trailer block", limits_.trailer_block_bytes));
    }
    header field;
    if (!parse_field_line(line_, field, error_)) {
        state_ = state::failed;
        return false;
    }
    line_.clear();
    if (forbidden_trailer(field.name)) {
        return fail(rpc_code::protocol_error, "forbidden trailer field");
    }
    if (trailers_.size() >= limits_.trailer_count) {
        return fail(rpc_code::resource_exhausted,
                    "trailer count exceeds limit of " +
                        std::to_string(limits_.trailer_count));
    }
    trailers_.add(field.name, field.value);
    return true;
}

chunk_status chunked_decoder::feed(span<const std::byte> data, std::string& out,
                                   std::size_t& consumed) {
    consumed = 0;
    if (state_ == state::failed) {
        return chunk_status::error;
    }
    if (state_ == state::done) {
        return chunk_status::complete;
    }

    while (consumed < data.size() && state_ != state::done &&
           state_ != state::failed) {
        const char c = static_cast<char>(data[consumed]);
        switch (state_) {
        case state::size:
            if (saw_cr_) {
                if (c != '\n') {
                    fail(rpc_code::protocol_error,
                         "chunk size line must end with CRLF");
                    break;
                }
                saw_cr_ = false;
                ++consumed;
                finish_size_line();
                break;
            }
            if (c == '\r') {
                saw_cr_ = true;
                ++consumed;
                break;
            }
            if (c == '\n') {
                fail(rpc_code::protocol_error, "bare LF in chunk framing");
                break;
            }
            line_.push_back(c);
            if (line_.size() > limits_.chunk_line_bytes) {
                fail(rpc_code::resource_exhausted,
                     limit_message("chunk line", limits_.chunk_line_bytes));
                break;
            }
            ++consumed;
            break;

        case state::data: {
            const std::size_t available = data.size() - consumed;
            const std::size_t take = std::min(available, chunk_remaining_);
            out.append(reinterpret_cast<const char*>(data.data() + consumed), take);
            body_size_ += take;
            chunk_remaining_ -= take;
            consumed += take;
            if (chunk_remaining_ == 0) {
                state_ = state::data_crlf;
                crlf_state_ = 0;
            }
            break;
        }

        case state::data_crlf:
            if (crlf_state_ == 0) {
                if (c != '\r') {
                    fail(rpc_code::protocol_error,
                         "chunk data must be followed by CRLF");
                    break;
                }
                crlf_state_ = 1;
                ++consumed;
                break;
            }
            if (c != '\n') {
                fail(rpc_code::protocol_error,
                     "chunk data must be followed by CRLF");
                break;
            }
            crlf_state_ = 0;
            state_ = state::size;
            ++consumed;
            break;

        case state::trailers:
            if (saw_cr_) {
                if (c != '\n') {
                    fail(rpc_code::protocol_error,
                         "trailer line must end with CRLF");
                    break;
                }
                saw_cr_ = false;
                ++consumed;
                finish_trailer_line();
                break;
            }
            if (c == '\r') {
                saw_cr_ = true;
                ++consumed;
                break;
            }
            if (c == '\n') {
                fail(rpc_code::protocol_error, "bare LF in chunk framing");
                break;
            }
            line_.push_back(c);
            if (line_.size() > limits_.chunk_line_bytes) {
                fail(rpc_code::resource_exhausted,
                     limit_message("chunk line", limits_.chunk_line_bytes));
                break;
            }
            if (line_.size() + trailer_bytes_ > limits_.trailer_block_bytes) {
                fail(rpc_code::resource_exhausted,
                     limit_message("trailer block", limits_.trailer_block_bytes));
                break;
            }
            ++consumed;
            break;

        case state::done:
        case state::failed:
            break;
        }
    }

    if (state_ == state::failed) {
        return chunk_status::error;
    }
    if (state_ == state::done) {
        return chunk_status::complete;
    }
    return chunk_status::need_more;
}

// --- Chunked encoder ---------------------------------------------------------

status chunked_encoder::append(span<const std::byte> data, std::string& out) const {
    if (max_chunk_bytes_ == 0) {
        return status(rpc_error(rpc_code::encode_error, "chunk size limit is zero"));
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t take = std::min(max_chunk_bytes_, data.size() - offset);
        append_hex(out, take);
        out.append("\r\n", 2);
        out.append(reinterpret_cast<const char*>(data.data() + offset), take);
        out.append("\r\n", 2);
        offset += take;
    }
    return status();
}

status chunked_encoder::finish(std::string& out) const {
    out.append("0\r\n\r\n", 5);
    return status();
}

status chunked_encoder::finish(const header_list& trailers, std::string& out) const {
    out.append("0\r\n", 3);
    for (const header& field : trailers) {
        if (forbidden_trailer(field.name)) {
            return status(rpc_error(rpc_code::encode_error, "forbidden trailer field"));
        }
        const status result = append_header_line(out, field.name, field.value);
        if (!result.ok()) {
            return result;
        }
    }
    out.append("\r\n", 2);
    return status();
}

// --- Parser ------------------------------------------------------------------

http_parser::http_parser(kind type, parser_limits limits)
    : kind_(type), limits_(limits), decoder_(limits) {}

void http_parser::reset() {
    state_ = state::start_line;
    buf_.clear();
    request_ = request{};
    response_ = response{};
    header_bytes_ = 0;
    fixed_remaining_ = 0;
    headers_done_ = false;
    complete_ = false;
    chunked_ = false;
    decoder_.reset();
    error_ = rpc_error();
}

bool http_parser::fail(rpc_code code, std::string message) {
    error_ = rpc_error(code, std::move(message));
    state_ = state::failed;
    return false;
}

http_parser::line_result http_parser::read_line(std::size_t limit, const char* what,
                                                std::string& line,
                                                std::size_t& consumed) {
    const std::size_t newline = buf_.find('\n');
    if (newline == std::string::npos) {
        if (buf_.size() > limit) {
            fail(rpc_code::resource_exhausted, limit_message(what, limit));
            return line_result::error;
        }
        return line_result::need_more;
    }

    std::size_t end = newline;
    if (end > 0 && buf_[end - 1] == '\r') {
        --end;
    }
    for (std::size_t i = 0; i < end; ++i) {
        if (buf_[i] == '\r') {
            fail(rpc_code::protocol_error,
                 std::string(what) + " contains a bare CR");
            return line_result::error;
        }
    }
    if (end > limit) {
        fail(rpc_code::resource_exhausted, limit_message(what, limit));
        return line_result::error;
    }

    line.assign(buf_.data(), end);
    consumed = newline + 1;
    buf_.erase(0, consumed);
    return line_result::found;
}

bool http_parser::parse_request_line(std::string_view line) {
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return fail(rpc_code::protocol_error, "malformed request line");
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return fail(rpc_code::protocol_error, "malformed request line");
    }

    const std::string_view method_text = line.substr(0, first_space);
    const std::string_view target = line.substr(first_space + 1,
                                                second_space - first_space - 1);
    const std::string_view version_text = line.substr(second_space + 1);
    if (!is_token(method_text) || target.empty() || version_text.empty()) {
        return fail(rpc_code::protocol_error, "malformed request line");
    }

    http_version version = http_version::http_1_1;
    if (!parse_version(version_text, version)) {
        return fail(rpc_code::protocol_error,
                    "unsupported HTTP version: " + std::string(version_text));
    }

    target_form form = target_form::origin;
    std::string path;
    std::string query;
    rpc_error target_error;
    if (!classify_target(target, form, path, query, target_error)) {
        error_ = std::move(target_error);
        state_ = state::failed;
        return false;
    }

    const method parsed_method = method_from_token(method_text);
    if (form == target_form::asterisk && parsed_method != method::options) {
        return fail(rpc_code::protocol_error,
                    "asterisk-form target is only valid for OPTIONS");
    }
    if (form == target_form::authority && parsed_method != method::connect) {
        return fail(rpc_code::protocol_error,
                    "authority-form target is only valid for CONNECT");
    }
    if (parsed_method == method::connect && form != target_form::authority) {
        return fail(rpc_code::protocol_error,
                    "CONNECT requires an authority-form target");
    }

    request_.method = parsed_method;
    request_.method_token.assign(method_text);
    request_.form = form;
    request_.target.assign(target);
    request_.path = std::move(path);
    request_.query = std::move(query);
    request_.version = version;
    return true;
}

bool http_parser::parse_status_line(std::string_view line) {
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return fail(rpc_code::protocol_error, "malformed status line");
    }
    const std::string_view version_text = line.substr(0, first_space);
    const std::string_view rest = line.substr(first_space + 1);

    http_version version = http_version::http_1_1;
    if (!parse_version(version_text, version)) {
        return fail(rpc_code::protocol_error,
                    "unsupported HTTP version: " + std::string(version_text));
    }
    if (rest.size() < 3) {
        return fail(rpc_code::protocol_error, "malformed status code");
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!is_digit(rest[i])) {
            return fail(rpc_code::protocol_error, "malformed status code");
        }
    }
    const int status_code = (rest[0] - '0') * 100 + (rest[1] - '0') * 10 +
                            (rest[2] - '0');
    if (status_code < 100) {
        return fail(rpc_code::protocol_error, "invalid status code");
    }
    if (rest.size() > 3 && rest[3] != ' ') {
        return fail(rpc_code::protocol_error, "malformed status line");
    }
    const std::string_view reason =
        rest.size() > 3 ? rest.substr(4) : std::string_view();
    if (!valid_reason_phrase(reason)) {
        return fail(rpc_code::protocol_error, "invalid reason phrase");
    }

    response_.version = version;
    response_.status_code = status_code;
    response_.reason.assign(reason);
    return true;
}

bool http_parser::parse_start_line(std::string_view line) {
    return kind_ == kind::request ? parse_request_line(line) : parse_status_line(line);
}

bool http_parser::parse_header_line(std::string_view line) {
    header field;
    if (!parse_field_line(line, field, error_)) {
        state_ = state::failed;
        return false;
    }
    if (headers().size() >= limits_.header_count) {
        return fail(rpc_code::resource_exhausted,
                    "header count exceeds limit of " +
                        std::to_string(limits_.header_count));
    }
    headers().add(field.name, field.value);
    return true;
}

parse_status http_parser::finish_headers() {
    headers_done_ = true;
    framing_info info;
    if (!analyze_framing(headers(), limits_.body_bytes, info, error_)) {
        state_ = state::failed;
        return parse_status::error;
    }

    for (const header& field : headers()) {
        if (ci_equal(field.name, "expect") &&
            ci_equal(trim_ows(field.value), "100-continue")) {
            request_.expect_continue = true;
        }
    }

    if (kind_ == kind::response && status_forbids_body(response_.status_code)) {
        chunked_ = false;
        fixed_remaining_ = 0;
        state_ = state::done;
        complete_ = true;
        return parse_status::message_complete;
    }

    chunked_ = info.chunked;
    fixed_remaining_ = info.has_cl ? info.content_length : 0;

    if (chunked_) {
        state_ = state::body_chunked;
        return parse_status::headers_complete;
    }
    if (fixed_remaining_ != 0) {
        state_ = state::body_fixed;
        return parse_status::headers_complete;
    }

    state_ = state::done;
    complete_ = true;
    return parse_status::message_complete;
}

parse_status http_parser::feed(span<const std::byte> data) {
    if (state_ == state::failed) {
        return parse_status::error;
    }
    if (state_ == state::done) {
        return parse_status::message_complete;
    }
    if (!data.empty()) {
        buf_.append(reinterpret_cast<const char*>(data.data()), data.size());
    }

    for (;;) {
        switch (state_) {
        case state::start_line: {
            std::string line;
            std::size_t consumed = 0;
            const line_result result =
                read_line(limits_.request_line_bytes, "request line", line, consumed);
            if (result == line_result::need_more) {
                return parse_status::need_more;
            }
            if (result == line_result::error) {
                return parse_status::error;
            }
            if (line.empty()) {
                (void)fail(rpc_code::protocol_error, "empty start line");
                return parse_status::error;
            }
            if (!parse_start_line(line)) {
                return parse_status::error;
            }
            state_ = state::header_lines;
            header_bytes_ = 0;
            break;
        }

        case state::header_lines: {
            const std::size_t remaining =
                header_bytes_ >= limits_.header_block_bytes
                    ? 0
                    : limits_.header_block_bytes - header_bytes_;
            std::string line;
            std::size_t consumed = 0;
            const line_result result =
                read_line(remaining, "header block", line, consumed);
            if (result == line_result::need_more) {
                return parse_status::need_more;
            }
            if (result == line_result::error) {
                return parse_status::error;
            }
            if (line.empty()) {
                // The empty line terminates the field section and is not
                // counted against the header-block limit.
                const parse_status result_status = finish_headers();
                if (result_status == parse_status::error) {
                    return parse_status::error;
                }
                if (result_status == parse_status::message_complete) {
                    return parse_status::message_complete;
                }
                break;
            }
            header_bytes_ += consumed;
            if (header_bytes_ > limits_.header_block_bytes) {
                fail(rpc_code::resource_exhausted,
                     limit_message("header block", limits_.header_block_bytes));
                return parse_status::error;
            }
            if (!parse_header_line(line)) {
                return parse_status::error;
            }
            break;
        }

        case state::body_fixed: {
            const std::size_t take = std::min(buf_.size(), fixed_remaining_);
            if (take != 0) {
                body().append(buf_.data(), take);
                buf_.erase(0, take);
                fixed_remaining_ -= take;
            }
            if (fixed_remaining_ == 0) {
                complete_ = true;
                state_ = state::done;
                return parse_status::message_complete;
            }
            return parse_status::headers_complete;
        }

        case state::body_chunked: {
            std::size_t consumed = 0;
            const span<const std::byte> input(
                reinterpret_cast<const std::byte*>(buf_.data()), buf_.size());
            const chunk_status result = decoder_.feed(input, body(), consumed);
            if (consumed != 0) {
                buf_.erase(0, consumed);
            }
            if (result == chunk_status::error) {
                error_ = decoder_.error();
                state_ = state::failed;
                return parse_status::error;
            }
            if (result == chunk_status::complete) {
                trailers() = decoder_.trailers();
                complete_ = true;
                state_ = state::done;
                return parse_status::message_complete;
            }
            return parse_status::headers_complete;
        }

        case state::done:
            return parse_status::message_complete;

        case state::failed:
            return parse_status::error;
        }
    }
}

parse_status http_parser::current_status() const noexcept {
    if (state_ == state::failed) {
        return parse_status::error;
    }
    if (complete_) {
        return parse_status::message_complete;
    }
    if (headers_done_) {
        return parse_status::headers_complete;
    }
    return parse_status::need_more;
}

span<const std::byte> http_parser::pending() const noexcept {
    return span<const std::byte>(
        reinterpret_cast<const std::byte*>(buf_.data()), buf_.size());
}

// --- Serializer --------------------------------------------------------------

namespace {

status serialize_start_and_headers(std::string& out, std::string_view start_line,
                                   const header_list& headers, bool emit_chunked,
                                   bool emit_content_length, std::size_t body_size) {
    out.append(start_line.data(), start_line.size());
    out.append("\r\n", 2);
    for (const header& field : headers) {
        const status result = append_header_line(out, field.name, field.value);
        if (!result.ok()) {
            return result;
        }
    }
    if (emit_chunked) {
        out.append("Transfer-Encoding: chunked\r\n", 28);
    }
    if (emit_content_length) {
        out.append("Content-Length: ", 16);
        out.append(std::to_string(body_size));
        out.append("\r\n", 2);
    }
    out.append("\r\n", 2);
    return status();
}

} // namespace

status serialize(const request& message, std::string& out,
                 serialize_options options) {
    std::string method_text;
    if (message.method == method::extension) {
        method_text = message.method_token;
    } else {
        method_text = to_token(message.method);
    }
    if (!is_token(method_text)) {
        return status(rpc_error(rpc_code::encode_error, "invalid method token"));
    }

    target_form form = target_form::origin;
    std::string path;
    std::string query;
    rpc_error target_error;
    if (!classify_target(message.target, form, path, query, target_error)) {
        return status(rpc_error(rpc_code::encode_error, target_error.message));
    }
    if (form == target_form::asterisk && message.method != method::options) {
        return status(rpc_error(rpc_code::encode_error,
                                "asterisk-form target is only valid for OPTIONS"));
    }
    if (form == target_form::authority && message.method != method::connect) {
        return status(rpc_error(rpc_code::encode_error,
                                "authority-form target is only valid for CONNECT"));
    }
    if (message.method == method::connect && form != target_form::authority) {
        return status(rpc_error(rpc_code::encode_error,
                                "CONNECT requires an authority-form target"));
    }

    framing_info info;
    rpc_error framing_error;
    if (!analyze_framing(message.headers, std::numeric_limits<std::size_t>::max(),
                         info, framing_error)) {
        return status(rpc_error(rpc_code::encode_error, framing_error.message));
    }

    const bool chunked = options.force_chunked || info.chunked;
    if (options.force_chunked && info.has_cl) {
        return status(rpc_error(rpc_code::encode_error,
                                "Content-Length and chunked encoding must not be combined"));
    }
    if (info.has_cl && options.include_body && !chunked &&
        info.content_length != message.body.size()) {
        return status(rpc_error(rpc_code::encode_error,
                                "Content-Length does not match body size"));
    }
    if (!chunked && !message.trailers.empty()) {
        return status(rpc_error(rpc_code::encode_error,
                                "trailers require chunked transfer encoding"));
    }
    if (!options.include_body && !message.trailers.empty()) {
        return status(rpc_error(rpc_code::encode_error,
                                "trailers cannot be emitted without a body"));
    }

    std::string line(method_text);
    line.push_back(' ');
    line.append(message.target);
    line.push_back(' ');
    line.append(to_string(message.version));

    const bool emit_chunked = options.force_chunked && !info.chunked;
    const bool emit_content_length = !chunked && !info.has_cl;

    std::string result;
    const status header_status =
        serialize_start_and_headers(result, line, message.headers, emit_chunked,
                                    emit_content_length, message.body.size());
    if (!header_status.ok()) {
        return header_status;
    }

    if (options.include_body) {
        if (chunked) {
            chunked_encoder encoder;
            const status append_status =
                encoder.append(span<const std::byte>(
                                   reinterpret_cast<const std::byte*>(message.body.data()),
                                   message.body.size()),
                               result);
            if (!append_status.ok()) {
                return append_status;
            }
            const status finish_status = encoder.finish(message.trailers, result);
            if (!finish_status.ok()) {
                return finish_status;
            }
        } else {
            result.append(message.body);
        }
    }

    out.swap(result);
    return status();
}

status serialize(const response& message, std::string& out,
                 serialize_options options) {
    if (message.status_code < 100 || message.status_code > 999) {
        return status(rpc_error(rpc_code::encode_error, "invalid status code"));
    }
    std::string reason = message.reason;
    if (reason.empty()) {
        reason = default_reason(message.status_code);
    }
    if (!valid_reason_phrase(reason)) {
        return status(rpc_error(rpc_code::encode_error, "invalid reason phrase"));
    }

    framing_info info;
    rpc_error framing_error;
    if (!analyze_framing(message.headers, std::numeric_limits<std::size_t>::max(),
                         info, framing_error)) {
        return status(rpc_error(rpc_code::encode_error, framing_error.message));
    }

    const bool body_forbidden = status_forbids_body(message.status_code);
    if (body_forbidden && !message.body.empty()) {
        return status(rpc_error(rpc_code::encode_error,
                                "this status code must not carry a body"));
    }

    const bool chunked = options.force_chunked || info.chunked;
    if (body_forbidden && chunked) {
        return status(rpc_error(rpc_code::encode_error,
                                "this status code must not use chunked encoding"));
    }
    if (options.force_chunked && info.has_cl) {
        return status(rpc_error(rpc_code::encode_error,
                                "Content-Length and chunked encoding must not be combined"));
    }
    if (info.has_cl && options.include_body && !chunked &&
        info.content_length != message.body.size()) {
        return status(rpc_error(rpc_code::encode_error,
                                "Content-Length does not match body size"));
    }
    if (!chunked && !message.trailers.empty()) {
        return status(rpc_error(rpc_code::encode_error,
                                "trailers require chunked transfer encoding"));
    }
    if (!options.include_body && !message.trailers.empty()) {
        return status(rpc_error(rpc_code::encode_error,
                                "trailers cannot be emitted without a body"));
    }

    std::string line(to_string(message.version));
    line.push_back(' ');
    line.append(std::to_string(message.status_code));
    line.push_back(' ');
    line.append(reason);

    const bool emit_chunked = options.force_chunked && !info.chunked;
    const bool emit_content_length = !chunked && !info.has_cl && !body_forbidden;

    std::string result;
    const status header_status =
        serialize_start_and_headers(result, line, message.headers, emit_chunked,
                                    emit_content_length, message.body.size());
    if (!header_status.ok()) {
        return header_status;
    }

    if (options.include_body) {
        if (chunked) {
            chunked_encoder encoder;
            const status append_status =
                encoder.append(span<const std::byte>(
                                   reinterpret_cast<const std::byte*>(message.body.data()),
                                   message.body.size()),
                               result);
            if (!append_status.ok()) {
                return append_status;
            }
            const status finish_status = encoder.finish(message.trailers, result);
            if (!finish_status.ok()) {
                return finish_status;
            }
        } else {
            result.append(message.body);
        }
    }

    out.swap(result);
    return status();
}

} // namespace crpc::http
